#include "pch.h"

#include "MainWindow.xaml.h"
#if __has_include("MainWindow.g.cpp")
#include "MainWindow.g.cpp"
#endif

#include "host/ts_audio_device.hpp"
#include "host/ts_player.hpp"

// IWindowNative, which is how a WinUI 3 desktop window surrenders its HWND. There is no projected
// way to ask: the window is a XAML object and the HWND is an implementation detail everywhere
// except at the two places Win32 still demands one -- the pickers below being the first.
#include <microsoft.ui.xaml.window.h>

// IInitializeWithWindow. A picker shown from a desktop app has no CoreWindow to parent itself to,
// and without this it throws rather than falling back to anything.
#include <shobjidl_core.h>

#include <chrono>
#include <exception>
#include <format>
#include <string>

using namespace winrt;
using namespace Microsoft::UI::Xaml;
using namespace Windows::Storage;
using namespace Windows::Storage::Pickers;

namespace {

/// Frames at the engine's 32 kHz, as m:ss. The transport gets a real one later; this is the two
/// lines the seam test needs to show that the position is advancing at the right rate.
std::string format_time(std::int64_t frames)
{
    if (frames < 0) {
        frames = 0;
    }
    const auto total = frames / ts::host::Session::sample_rate;
    return std::format("{}:{:02}", total / 60, total % 60);
}

} // namespace

namespace winrt::WindowsTSPlayer::implementation
{
    MainWindow::MainWindow()
        : player_(std::make_unique<ts::host::Player>())
    {
        // The render thread starts with the player and idles: it is created paused and with no
        // session, so it parks rather than spinning. Building it here rather than on first load
        // means the ring and the thread exist before anything can ask them to keep up.

        Title(L"Tabula Sonora Player");

        // The display tick, at the same 10 Hz the Apple and Linux front ends use. Fast enough that
        // a voice count looks live, slow enough that it costs nothing -- and, more to the point,
        // decoupled from the render thread entirely: this reads a snapshot the renderer published,
        // it never reaches into the engine.
        timer_ = DispatcherQueue().CreateTimer();
        timer_.Interval(std::chrono::milliseconds(100));
        timer_.Tick([this](auto&&, auto&&) { Tick(); });
        timer_.Start();

        // Tear down in the reverse of the order things were built, and do it here rather than
        // leaving it to process exit. The device's feeder thread and the render thread both outlive
        // the XAML tree otherwise, and a WASAPI client still reading a ring whose owner is being
        // destroyed is the one race in this program that would be genuinely hard to diagnose.
        Closed([this](IInspectable const&, WindowEventArgs const&) {
            if (timer_) {
                timer_.Stop();
            }
            if (device_) {
                device_->stop();
            }
            device_.reset();
            player_.reset();
        });
    }

    // Defined here, not in the header: unique_ptr needs both types complete to destroy them, and
    // keeping the engine's headers out of the header is the whole point of forward-declaring them.
    MainWindow::~MainWindow() = default;

    HWND MainWindow::Hwnd()
    {
        HWND hwnd{};
        check_hresult(this->try_as<::IWindowNative>()->get_WindowHandle(&hwnd));
        return hwnd;
    }

    fire_and_forget MainWindow::OnOpenRomClick(IInspectable const&, RoutedEventArgs const&)
    {
        auto lifetime = get_strong();

        FileOpenPicker picker;
        picker.as<::IInitializeWithWindow>()->Initialize(Hwnd());
        picker.SuggestedStartLocation(PickerLocationId::ComputerFolder);
        picker.FileTypeFilter().Append(L".dll");

        const StorageFile file = co_await picker.PickSingleFileAsync();
        if (file == nullptr) {
            co_return;
        }

        // to_string, not a wide path. The whole host layer is std::string and is shared by copy with
        // the Apple and Linux front ends; app.manifest declares the process code page as UTF-8 so
        // that these bytes reach CreateFileA and std::ifstream meaning what they say. A path with
        // non-ASCII characters is the case that proves it, and it fails silently without that line.
        const std::string path = to_string(file.Path());

        OpenRomButton().IsEnabled(false);
        StatusText().Text(L"Loading ROM...");

        std::string error;

        // The quick identity check, not the full 27 MB hash -- but still off the UI thread, because
        // building the engine over the ROM loads the wavetables and is not instant either.
        co_await resume_background();
        try {
            player_->load_rom(path, false);
        } catch (const std::exception& e) {
            error = e.what();
        } catch (const hresult_error& e) {
            error = to_string(e.message());
        }
        co_await wil::resume_foreground(DispatcherQueue());

        OpenRomButton().IsEnabled(true);
        if (!error.empty()) {
            StatusText().Text(to_hstring("ROM failed to load: " + error));
            co_return;
        }

        StatusText().Text(to_hstring("ROM loaded: " + player_->rom_name()));
        OpenSongButton().IsEnabled(true);
    }

    fire_and_forget MainWindow::OnOpenSongClick(IInspectable const&, RoutedEventArgs const&)
    {
        auto lifetime = get_strong();

        FileOpenPicker picker;
        picker.as<::IInitializeWithWindow>()->Initialize(Hwnd());
        picker.SuggestedStartLocation(PickerLocationId::MusicLibrary);

        // The twelve extensions the Linux front end accepts, verbatim. LDS is the one worth noting:
        // it has no magic number and is recognised by its extension alone, which is what made the
        // two-separator fix in Session::file_name load-bearing rather than cosmetic.
        for (const auto* extension : { L".mid", L".midi", L".rmi", L".mids", L".mus", L".xmi",
                                       L".xmf", L".mxmf", L".gmf", L".hmi", L".hmp", L".lds" }) {
            picker.FileTypeFilter().Append(extension);
        }

        const StorageFile file = co_await picker.PickSingleFileAsync();
        if (file == nullptr) {
            co_return;
        }

        const std::string path = to_string(file.Path());

        OpenSongButton().IsEnabled(false);
        StatusText().Text(L"Loading song...");

        std::string error;

        co_await resume_background();
        try {
            player_->load_song(path);

            // The device is opened only once there is something to hear. Opening it at startup
            // would hold an audio endpoint for a program sitting on its setup screen.
            if (!device_) {
                device_ = std::make_unique<ts::host::AudioDevice>(*player_);
            }
            device_->start();
            player_->set_paused(false);
        } catch (const std::exception& e) {
            error = e.what();
        } catch (const hresult_error& e) {
            error = to_string(e.message());
        }
        co_await wil::resume_foreground(DispatcherQueue());

        OpenSongButton().IsEnabled(true);
        if (!error.empty()) {
            StatusText().Text(to_hstring("Song failed to load: " + error));
            co_return;
        }

        StatusText().Text(to_hstring("Playing: " + player_->song_name()));
        PlayPauseButton().IsEnabled(true);
        PlayPauseButton().Content(box_value(L"Pause"));

        if (device_) {
            BackendText().Text(to_hstring(std::format(
                "{} at {} Hz; engine renders at {} Hz",
                device_->backend_name(), device_->device_rate(), ts::host::Session::sample_rate)));
        }
    }

    void MainWindow::OnPlayPauseClick(IInspectable const&, RoutedEventArgs const&)
    {
        const bool paused = !player_->paused();
        player_->set_paused(paused);
        PlayPauseButton().Content(box_value(paused ? L"Play" : L"Pause"));
    }

    void MainWindow::Tick()
    {
        if (!player_) {
            return;
        }

        const ts::host::SessionSnapshot s = player_->snapshot();
        if (!s.hasROM) {
            return;
        }

        EngineText().Text(to_hstring(std::format(
            "voices     {:3} / {}\n"
            "position   {} / {}\n"
            "parts      {}\n"
            "mode       {}\n"
            "underruns  {}\n"
            "peak       {:.3f}  {:.3f}",
            s.activeVoices, s.voiceCapacity,
            format_time(s.position), format_time(s.length),
            s.partCount,
            s.xgMode ? "XG" : "GS",
            s.underruns,
            s.peakLeft, s.peakRight)));

        // The complete-to-paused transition, which the real player model owns later. Here it only
        // keeps the button honest once a song has run out.
        if (s.complete && !s.paused) {
            player_->set_paused(true);
            PlayPauseButton().Content(box_value(L"Play"));
        }
    }
}
