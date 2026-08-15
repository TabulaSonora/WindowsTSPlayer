#include "host/ts_audio_device.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#    define NOMINMAX
#endif
#include <windows.h>
// These four have to follow windows.h; none of them includes it.
#include <audioclient.h>
#include <avrt.h>
#include <mmdeviceapi.h>
#include <objbase.h>

#include <atomic>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>

namespace ts::host {

// WASAPI directly, rather than the miniaudio the Linux front end uses.
//
// This file is not shared with the other two ports, and never was: the Apple build has its own
// `AudioOutput` over an AVAudioSourceNode, and the Linux build has this class over miniaudio. Each
// port writes its own device layer natively, and this is the Windows one.
//
// miniaudio earns its keep on Linux for a reason that does not exist here. Its value there is
// choosing among PipeWire, PulseAudio, ALSA and JACK in the order a Linux desktop wants -- four
// competing audio servers, any of which may be the live one. Windows has exactly one modern answer,
// so the abstraction would be paying for portability this project does not use, in a repository
// that is Windows-only by definition. Dropping it also drops a vcpkg dependency and a whole
// translation unit of vendored code compiled with warnings off.
//
// It buys two things beyond that. A route change becomes a real device-change notification rather
// than a blind reopen, which is what `restart` exists for; and `backend_name` becomes a statement
// rather than a runtime guess.

namespace {

/// COM apartment, scoped to the object that needs it.
///
/// WASAPI is COM, and the thread that opens a device must have initialised an apartment. Doing it
/// per-object rather than once at start-up keeps the host layer free of process-wide side effects:
/// this is a library, and a host that has already chosen its own apartment model must not have that
/// choice overwritten. RPC_E_CHANGED_MODE means exactly that -- somebody got here first with a
/// different model -- and is deliberately not an error, since the existing apartment serves.
class ComScope {
public:
    ComScope() noexcept
    {
        const HRESULT result = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        owned_ = SUCCEEDED(result) && result != S_FALSE;
    }

    ~ComScope()
    {
        if (owned_) {
            ::CoUninitialize();
        }
    }

    ComScope(const ComScope&) = delete;
    ComScope& operator=(const ComScope&) = delete;
    ComScope(ComScope&&) = delete;
    ComScope& operator=(ComScope&&) = delete;

private:
    bool owned_ = false;
};

/// Minimal owning pointer, so the many COM interfaces below do not each need a release path.
template <typename T>
class ComPtr {
public:
    ComPtr() = default;

    ~ComPtr() { reset(); }

    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;

    ComPtr(ComPtr&& other) noexcept : pointer_{other.pointer_} { other.pointer_ = nullptr; }

    ComPtr& operator=(ComPtr&& other) noexcept
    {
        if (this != &other) {
            reset();
            pointer_ = other.pointer_;
            other.pointer_ = nullptr;
        }
        return *this;
    }

    [[nodiscard]] T** put() noexcept
    {
        reset();
        return &pointer_;
    }

    [[nodiscard]] void** put_void() noexcept { return reinterpret_cast<void**>(put()); }
    [[nodiscard]] T* get() const noexcept { return pointer_; }
    T* operator->() const noexcept { return pointer_; }
    explicit operator bool() const noexcept { return pointer_ != nullptr; }

    void reset() noexcept
    {
        if (pointer_ != nullptr) {
            pointer_->Release();
            pointer_ = nullptr;
        }
    }

private:
    T* pointer_ = nullptr;
};

/// A handle that closes itself. Used for the event WASAPI signals and nothing else.
class Handle {
public:
    Handle() = default;
    explicit Handle(HANDLE handle) noexcept : handle_{handle} {}

    ~Handle() { reset(); }

    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& other) noexcept : handle_{other.handle_} { other.handle_ = nullptr; }

    Handle& operator=(Handle&& other) noexcept
    {
        if (this != &other) {
            reset();
            handle_ = other.handle_;
            other.handle_ = nullptr;
        }
        return *this;
    }

    [[nodiscard]] HANDLE get() const noexcept { return handle_; }
    explicit operator bool() const noexcept { return handle_ != nullptr; }

    void reset() noexcept
    {
        if (handle_ != nullptr) {
            ::CloseHandle(handle_);
            handle_ = nullptr;
        }
    }

private:
    HANDLE handle_ = nullptr;
};

/// How much buffer to ask WASAPI for, in 100 ns units -- 60 ms.
///
/// Not the player's lead, and deliberately unrelated to it. This is only how much room the device
/// has to be refilled within; `Player` decides how far ahead to render, and the ring is what
/// absorbs a late block. Asking for a large device buffer here would not make playback smoother,
/// because a starved ring reads as silence either way -- it would only make `stop` slower to take
/// effect. Shared mode treats this as a request and returns what it actually allocated.
constexpr REFERENCE_TIME requested_duration = 60 * 10'000;

} // namespace

struct AudioDevice::Impl {
    Player* player = nullptr;

    ComScope com;

    ComPtr<IMMDeviceEnumerator> enumerator;
    ComPtr<IMMDevice> endpoint;
    ComPtr<IAudioClient> client;
    ComPtr<IAudioRenderClient> render;
    Handle ready;

    /// Frames in the device buffer, as WASAPI allocated it rather than as it was asked for.
    UINT32 buffer_frames = 0;

    /// What the endpoint's mix actually runs at, for `device_rate`. Not what the stream is opened
    /// at -- that is always the engine's rate.
    int mix_rate = 0;

    std::thread feeder;
    std::atomic<bool> quit{false};
    std::atomic<bool> started{false};

    void run();
};

namespace {

void throw_if_failed(HRESULT result, const char* message)
{
    if (FAILED(result)) {
        throw std::runtime_error{message};
    }
}

} // namespace

/// The consumer, and the only code in this project that runs under a hard deadline.
///
/// It records what the device asked for -- the render thread reads that to size its lead, since the
/// period is not ours to choose and can change under us -- and then copies one span out of the ring.
/// `FrameRing::read` zero-pads any shortfall and counts it as an underrun, so there is nothing to
/// check and nothing to decide here.
///
/// Event-driven rather than polled: WASAPI signals `ready` each period, which is both lower latency
/// and lower power than waking on a timer and asking how much room there is.
///
/// This thread is the one with the deadline, so unlike the render thread it does no allocation, no
/// locking and no engine work at all -- one `GetBuffer`, one ring read, one `ReleaseBuffer`.
void AudioDevice::Impl::run()
{
    // The apartment is per-thread, so this thread needs its own regardless of the one the object
    // was constructed on.
    const ComScope thread_com;

    // The same MMCSS registration the render thread makes, and for a stronger reason: this one
    // genuinely cannot be late. With miniaudio gone, nothing else is going to do this for us.
    DWORD mmcss_task = 0;
    HANDLE mmcss = ::AvSetMmThreadCharacteristicsW(L"Pro Audio", &mmcss_task);
    if (mmcss == nullptr) {
        ::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    }

    while (!quit.load(std::memory_order_relaxed)) {
        // A timeout rather than INFINITE so a device that stops signalling -- unplugged, or a
        // driver fault -- cannot wedge this thread past `stop`. Two periods' worth is long enough
        // that a healthy device never reaches it.
        if (::WaitForSingleObject(ready.get(), 200) != WAIT_OBJECT_0) {
            continue;
        }

        // Shared mode: the padding is what the mixer has not yet consumed, so the room to write is
        // whatever is left of the buffer. (Exclusive mode would hand over the whole buffer each
        // period instead, which is one of several reasons the two are not interchangeable here.)
        UINT32 padding = 0;
        if (FAILED(client->GetCurrentPadding(&padding))) {
            continue;
        }

        const UINT32 available = buffer_frames > padding ? buffer_frames - padding : 0;
        if (available == 0) {
            continue;
        }

        BYTE* data = nullptr;
        if (FAILED(render->GetBuffer(available, &data))) {
            continue;
        }

        player->handle().last_request.store(available, std::memory_order_relaxed);
        player->handle().ring->read(
            std::span<float>{reinterpret_cast<float*>(data), static_cast<std::size_t>(available) * 2});

        render->ReleaseBuffer(available, 0);
    }

    if (mmcss != nullptr) {
        ::AvRevertMmThreadCharacteristics(mmcss);
    }
}

AudioDevice::AudioDevice(Player& player) : impl_(std::make_unique<Impl>())
{
    impl_->player = &player;
}

AudioDevice::~AudioDevice()
{
    stop();
}

void AudioDevice::start()
{
    if (impl_->started.load(std::memory_order_relaxed)) {
        return;
    }

    throw_if_failed(::CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                       IID_PPV_ARGS(impl_->enumerator.put())),
                    "No audio backend could be initialised");

    // The default console endpoint: what Windows itself considers "the speakers", and what changes
    // under us when the user picks a different output. eMultimedia would second-guess that choice.
    throw_if_failed(
        impl_->enumerator->GetDefaultAudioEndpoint(eRender, eConsole, impl_->endpoint.put()),
        "No audio output device could be opened");

    throw_if_failed(impl_->endpoint->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                              impl_->client.put_void()),
                    "No audio output device could be opened");

    // Read the endpoint's own mix format purely to report it. The stream below is opened at the
    // engine's rate regardless; this is what `device_rate` answers with, and it is the number a bug
    // report wants, since it is where the resampling lands.
    WAVEFORMATEX* mix = nullptr;
    if (SUCCEEDED(impl_->client->GetMixFormat(&mix)) && mix != nullptr) {
        impl_->mix_rate = static_cast<int>(mix->nSamplesPerSec);
        ::CoTaskMemFree(mix);
    }

    // 32-bit float, stereo, at the engine's own rate -- not the device's.
    //
    // WAVEFORMATEXTENSIBLE rather than a bare WAVEFORMATEX: shared-mode float is only unambiguous
    // through the subformat GUID, and some drivers reject the plain IEEE_FLOAT tag outright.
    WAVEFORMATEXTENSIBLE format{};
    format.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    format.Format.nChannels = 2;
    format.Format.nSamplesPerSec = static_cast<DWORD>(Session::sample_rate);
    format.Format.wBitsPerSample = 32;
    format.Format.nBlockAlign =
        static_cast<WORD>(format.Format.nChannels * format.Format.wBitsPerSample / 8);
    format.Format.nAvgBytesPerSec = format.Format.nSamplesPerSec * format.Format.nBlockAlign;
    format.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    format.Samples.wValidBitsPerSample = 32;
    format.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
    format.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;

    // AUTOCONVERTPCM is what makes the engine's 32 kHz openable on hardware that runs at 48: it
    // lets the audio engine insert its own sample-rate converter instead of rejecting the format.
    // Without it this call fails with AUDCLNT_E_UNSUPPORTED_FORMAT on essentially every device,
    // because 32 kHz is not a rate modern hardware mixes at.
    //
    // One conversion, performed by Windows, exactly where miniaudio used to perform it -- so the
    // engine still never has to be told what the device is doing, which is the property worth
    // keeping. (The Apple build connects its source node at 32 kHz for the same reason.)
    const DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                        AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;

    throw_if_failed(impl_->client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, requested_duration, 0,
                                              &format.Format, nullptr),
                    "The audio output device would not accept the engine's format");

    throw_if_failed(impl_->client->GetBufferSize(&impl_->buffer_frames),
                    "The audio output device would not start");

    impl_->ready = Handle{::CreateEventW(nullptr, FALSE, FALSE, nullptr)};
    if (!impl_->ready) {
        throw std::runtime_error{"The audio output device would not start"};
    }

    throw_if_failed(impl_->client->SetEventHandle(impl_->ready.get()),
                    "The audio output device would not start");

    throw_if_failed(impl_->client->GetService(IID_PPV_ARGS(impl_->render.put())),
                    "The audio output device would not start");

    // Started before the client, so the first period WASAPI signals already has a consumer waiting
    // for it rather than finding none and glitching on the very first buffer.
    impl_->quit.store(false, std::memory_order_relaxed);
    impl_->feeder = std::thread{[impl = impl_.get()] { impl->run(); }};

    if (FAILED(impl_->client->Start())) {
        impl_->quit.store(true, std::memory_order_relaxed);
        if (impl_->feeder.joinable()) {
            impl_->feeder.join();
        }
        throw std::runtime_error{"The audio output device would not start"};
    }

    impl_->started.store(true, std::memory_order_relaxed);
}

void AudioDevice::stop() noexcept
{
    impl_->quit.store(true, std::memory_order_relaxed);

    // Signalled so the feeder does not sit out its full timeout before noticing.
    if (impl_->ready) {
        ::SetEvent(impl_->ready.get());
    }
    if (impl_->feeder.joinable()) {
        impl_->feeder.join();
    }

    // Only after the feeder has gone: releasing the render client while it is inside GetBuffer is
    // a use-after-free, and the join above is what makes that impossible rather than unlikely.
    if (impl_->client) {
        impl_->client->Stop();
    }

    impl_->render.reset();
    impl_->ready.reset();
    impl_->client.reset();
    impl_->endpoint.reset();
    impl_->enumerator.reset();
    impl_->buffer_frames = 0;

    impl_->started.store(false, std::memory_order_relaxed);
}

void AudioDevice::restart()
{
    stop();
    start();
}

bool AudioDevice::running() const noexcept
{
    return impl_->started.load(std::memory_order_relaxed);
}

std::string AudioDevice::backend_name() const
{
    // A statement rather than a runtime query. There is one backend here by construction, which is
    // the whole reason this file does not use an abstraction layer.
    return impl_->started.load(std::memory_order_relaxed) ? "WASAPI (shared)" : std::string{};
}

int AudioDevice::device_rate() const noexcept
{
    return impl_->mix_rate;
}

} // namespace ts::host
