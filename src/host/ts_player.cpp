#include "host/ts_player.hpp"

#ifdef _WIN32
#    ifndef WIN32_LEAN_AND_MEAN
#        define WIN32_LEAN_AND_MEAN
#    endif
#    ifndef NOMINMAX
#        define NOMINMAX
#    endif
#    include <windows.h>
// avrt.h has to follow windows.h; it does not include it itself.
#    include <avrt.h>
#else
#    include <pthread.h>
#    include <sched.h>
#endif

#include <algorithm>
#include <chrono>
#include <utility>

namespace ts::host {

namespace {

/// Sized once, for the largest lead that can ever be asked for.
///
/// The lead is adjustable but the ring is not: reallocating it would mean swapping the buffer the
/// audio callback is reading from, and there is no safe moment to do that. Sizing for the maximum
/// and moving the target fill inside it costs a few hundred kilobytes and no synchronisation at all.
std::size_t ring_frames()
{
    const auto largest = std::max<std::int64_t>(
        static_cast<std::int64_t>(Player::block_frames) * 4,
        static_cast<std::int64_t>(Player::max_latency_ms) * Session::sample_rate / 1000);
    return 2 * static_cast<std::size_t>(largest);
}

} // namespace

Player::Player() : ring_(ring_frames())
{
    midi_inbox_.reserve(256);

    // Nothing is loaded yet, so an empty ring is intended rather than a fault.
    ring_.set_starvation_expected(true);

    renderer_ = std::thread{[this] { run(); }};
}

Player::~Player()
{
    quit_.store(true, std::memory_order_relaxed);
    if (renderer_.joinable()) {
        renderer_.join();
    }
}

void Player::load_rom(const std::string& path, bool verify_fully)
{
    // Both, and in this order: an export borrows the note renderer this is about to replace, so it
    // has to finish first. Every path that takes both takes them this way round.
    const std::lock_guard<std::mutex> exporting{export_lock_};
    const std::lock_guard<std::mutex> guard{lock_};
    session_.load_rom(path, verify_fully);
    publish();
}

void Player::load_song(const std::string& path)
{
    const std::lock_guard<std::mutex> guard{lock_};
    session_.load_song(path);

    // The song's own start, not zero, and the difference matters now that arming one skips its
    // silent lead-in: seeking to zero here would undo that immediately and put the transport back
    // in the silence it was just moved past.
    //
    // The seek is still queued rather than skipped, because it is doing a second job -- the render
    // thread flushes the ring on a pending seek, and without it the previous song's last ring-full
    // would play over the new one's opening.
    pending_seek_ = session_.position();
    audible_.store(session_.position(), std::memory_order_relaxed);
    publish();
}

void Player::unload_song()
{
    const std::lock_guard<std::mutex> guard{lock_};
    session_.unload_song();
    audible_.store(0, std::memory_order_relaxed);
    publish();
}

void Player::set_settings(const TSEngineSettings& settings)
{
    const std::lock_guard<std::mutex> guard{lock_};
    session_.set_settings(settings);
    publish();
}

TSEngineSettings Player::settings() const
{
    const std::lock_guard<std::mutex> guard{lock_};
    return session_.settings();
}

void Player::set_looping(bool looping)
{
    const std::lock_guard<std::mutex> guard{lock_};
    session_.set_looping(looping);
    publish();
}

void Player::seek(std::int64_t frame)
{
    const std::lock_guard<std::mutex> guard{lock_};
    pending_seek_ = std::max<std::int64_t>(0, frame);
}

std::int64_t Player::start_frame() const
{
    const std::lock_guard<std::mutex> guard{lock_};
    return session_.start_frame();
}

void Player::panic()
{
    const std::lock_guard<std::mutex> guard{lock_};
    session_.panic();
}

void Player::set_paused(bool paused)
{
    // While paused the ring is meant to run dry, so the consumer must not count that as an
    // underrun -- the display would fill with faults that are just a stopped transport.
    //
    // Unpausing deliberately does *not* clear the flag. The ring is empty at that moment and the
    // render thread has not had a block's notice, so the first callbacks would come up short and be
    // counted as dropouts -- one every time the user pressed Play. The render loop clears it once
    // the ring is half full, which is the same prefill upstream does before starting its device.
    if (paused) {
        ring_.set_starvation_expected(true);
    }

    paused_.store(paused, std::memory_order_relaxed);

    // Cut whatever the song was holding, after the flag is set so the sequencer has already stopped
    // advancing. Without this a note held across a pause never reaches its note-off -- the render
    // loop goes on rendering, because a stopped transport still has to answer a keyboard -- and the
    // chord sounds until playback resumes.
    if (paused) {
        const std::lock_guard<std::mutex> guard{lock_};
        session_.silence();
    }
}

void Player::set_latency_ms(int milliseconds) noexcept
{
    const int clamped = std::clamp(milliseconds, min_latency_ms, max_latency_ms);
    lead_frames_.store(clamped * Session::sample_rate / 1000, std::memory_order_relaxed);
}

int Player::latency_ms() const noexcept
{
    return lead_frames_.load(std::memory_order_relaxed) * 1000 / Session::sample_rate;
}

std::string Player::rom_name() const
{
    const std::lock_guard<std::mutex> guard{lock_};
    return session_.rom_name();
}

std::string Player::song_name() const
{
    const std::lock_guard<std::mutex> guard{lock_};
    return session_.song_name();
}

SongInfo Player::song_info() const
{
    const std::lock_guard<std::mutex> guard{lock_};
    return session_.song_info();
}

void Player::export_wav(const std::string& path, const std::function<bool(double)>& progress)
{
    // Held for the whole export, and it is what keeps the borrowed note renderer alive; `lock_` is
    // taken only long enough to lift the plan out. Holding `lock_` across the render instead would
    // block the render thread for the length of the export -- which is to say, stop the audio.
    const std::lock_guard<std::mutex> exporting{export_lock_};

    Session::ExportPlan plan;
    {
        const std::lock_guard<std::mutex> guard{lock_};
        plan = session_.plan_export();
    }

    Session::run_export(plan, path, progress);
}

void Player::set_muted(int part, bool muted)
{
    session_.set_muted(part, muted);
}

void Player::set_soloed(int part, bool soloed)
{
    session_.set_soloed(part, soloed);
}

void Player::reset_channels()
{
    session_.reset_channels();
}

void Player::send_channel(int port, int status, int data1, int data2)
{
    const std::lock_guard<std::mutex> guard{midi_lock_};
    // Bounded rather than growing: a source that outruns the render thread this badly is broken,
    // and dropping the newest message is better than letting a MIDI callback allocate.
    if (midi_inbox_.size() < 4096) {
        midi_inbox_.push_back({port, status, data1, data2});
    }
}

SessionSnapshot Player::snapshot() const
{
    const std::lock_guard<std::mutex> guard{snapshot_lock_};
    return snapshot_;
}

void Player::publish()
{
    SessionSnapshot taken;
    session_.capture(taken);

    taken.paused = paused_.load(std::memory_order_relaxed);
    taken.position = audible_.load(std::memory_order_relaxed);
    taken.underruns = ring_.underruns();
    const auto [left, right] = ring_.peak();
    taken.peakLeft = left;
    taken.peakRight = right;

    const std::lock_guard<std::mutex> guard{snapshot_lock_};
    snapshot_ = std::move(taken);
}

void Player::run()
{
    // Deliberately not a hard real-time thread in the strict sense: this one renders whole blocks
    // and takes a lock, which is precisely what an audio callback must not do. The device callback
    // below it has the deadline, and the ring is what buys this thread the freedom to miss one.
    //
    // What it does ask for is protection from the one failure it cannot absorb: being descheduled
    // behind an unrelated CPU-bound process for longer than the lead. Every platform spells that
    // differently and all three are best-effort -- when the request fails the thread simply runs at
    // normal priority, which is fine at the default lead, and is why none of them reports.
#ifdef _WIN32
    // Windows takes a wide string and imposes no length limit, so unlike Linux there is nothing to
    // truncate around; the name is kept identical anyway so a trace from any port reads alike.
    SetThreadDescription(GetCurrentThread(), L"ts-render");

    // MMCSS rather than a raw priority bump. Registering with the "Pro Audio" task class tells the
    // scheduler *why* this thread matters, where THREAD_PRIORITY_TIME_CRITICAL only tells it how
    // much -- and since this thread takes `lock_`, a raw time-critical priority invites a priority
    // inversion against whichever control thread holds it that a task-class boost does not. It is
    // also the closer analogue of what macOS does by joining the device's os_workgroup.
    //
    // The handle is reverted at the end of the function rather than leaked, because the class
    // registration is per-thread and outlives nothing here.
    DWORD mmcss_task = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &mmcss_task);
    if (mmcss == nullptr) {
        // The MMCSS service can be stopped or the task profile absent. One step above normal is
        // enough to stay ahead of ordinary background work, and is all that is wanted -- this is a
        // fallback, not a second attempt at the same guarantee.
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    }
#else
    // Linux takes the thread as an argument and truncates at 16 bytes including the terminator, so
    // the reverse-DNS name upstream uses will not fit.
    pthread_setname_np(pthread_self(), "ts-render");

    // Without an RLIMIT_RTPRIO allowance or CAP_SYS_NICE this call fails and the thread runs at
    // normal priority.
    sched_param param{};
    param.sched_priority = 10;
    pthread_setschedparam(pthread_self(), SCHED_FIFO, &param);
#endif

    // What the callback has been asking for, decayed so the lead follows the device down again
    // after it has been up. Frames, at the engine's rate.
    int callback_frames = 0;

    std::vector<float> left(block_frames, 0.0F);
    std::vector<float> right(block_frames, 0.0F);
    std::vector<float> block(static_cast<std::size_t>(block_frames) * 2, 0.0F);
    std::vector<MidiMessage> midi;
    midi.reserve(256);

    int until_publish = 0;

    /// Whether live MIDI arrived since the last block. Thread-local to the render loop: the inbox
    /// is drained here and nowhere else.
    bool live_activity = false;

    while (!quit_.load(std::memory_order_relaxed)) {
        // How long to wait before looking again, decided under the lock and slept *outside* it.
        // Sleeping while holding it would make an idle transport hold the session almost
        // continuously, and every control call would queue behind a nap it has nothing to do with.
        std::chrono::microseconds nap{0};

        {
            {
                const std::lock_guard<std::mutex> inbox{midi_lock_};
                midi.swap(midi_inbox_);
                midi_inbox_.clear();
            }

            const std::lock_guard<std::mutex> guard{lock_};

            live_activity = live_activity || !midi.empty();
            for (const auto& message : midi) {
                session_.send_channel(message.port, message.status, message.data1, message.data2);
            }
            midi.clear();

            if (pending_seek_) {
                // Drop what is queued so the new position is heard now rather than a ring's worth
                // of audio later. The consumer performs the drop; nothing may be written until it
                // has.
                ring_.set_starvation_expected(true);
                ring_.request_flush();
                session_.seek(*pending_seek_);
                audible_.store(session_.position(), std::memory_order_relaxed);
                pending_seek_.reset();
            }

            // Whether the *song* should be advancing.
            const bool transport_idle = paused_.load(std::memory_order_relaxed)
                                        || !session_.has_rom() || session_.complete();

            // Whether there is nonetheless sound to make.
            //
            // A stopped transport is not the same thing as a silent instrument: a key held on a
            // MIDI keyboard, or the release tail of one just let go, has to keep being rendered.
            // Upstream conflated the two and could only sound live notes over a playing song --
            // pressing a key with the transport stopped produced nothing at all.
            //
            // Live messages count for one block on their own so that a controller change with no
            // note behind it is not dropped; after that, sounding voices are what keep it awake,
            // which covers releases and held pedals without a timer.
            const bool live = session_.has_rom() && (live_activity || session_.active_voices() > 0);
            live_activity = false;

            const bool idle = transport_idle && !live;

            // A device block is not ours to choose and does not stay put: a backend may hand us a
            // different period after a device change, and a lead shorter than one block can never
            // satisfy a single callback -- every one of them would come up short no matter how fast
            // the engine renders. So the floor follows what the callback actually asks for, and the
            // setting is a floor of its own rather than a ceiling.
            const auto request = static_cast<int>(handle_.last_request.load(std::memory_order_relaxed));
            callback_frames = std::max(request, callback_frames - (callback_frames / 16) - 1);

            const auto configured = lead_frames_.load(std::memory_order_relaxed);
            const auto lead = static_cast<std::size_t>(
                std::min(std::max(configured, 3 * callback_frames),
                         static_cast<int>(ring_.capacity() - block_frames)));

            if (idle) {
                // Re-armed every iteration while idle, not once on the way in. Pausing leaves a
                // lead's worth of audio still queued, so a single set on the way in was undone by
                // the clear below on the very next pass -- and then the ring drained into a
                // callback that counted every empty block as a fault. A paused transport is the
                // one time an empty ring is exactly what was asked for.
                ring_.set_starvation_expected(true);
            } else if (ring_.queued() >= lead / 2) {
                // Half the lead is enough to call the ring primed: waiting for all of it would
                // leave the flag set through the first blocks after Play, which is exactly when
                // the callback is most likely to arrive early.
                ring_.set_starvation_expected(false);
            }

            if (ring_.flush_pending()) {
                nap = std::chrono::microseconds{500};
            } else if (idle) {
                nap = std::chrono::milliseconds{10};
            } else if (ring_.queued() >= lead
                       || ring_.writable() < static_cast<std::size_t>(block_frames)) {
                // The lead is reached, so wait rather than run further ahead. Napping for a
                // fraction of a block keeps the thread responsive to a seek without spinning.
                nap = std::chrono::microseconds{block_frames * 1000000 / Session::sample_rate / 4};
            } else {

                // The sequencer is only stepped when the transport is actually running. Rendering
                // live goes straight to the generator, so a note played while stopped sounds
                // without dragging the song forward under it.
                if (transport_idle) {
                    session_.render_live(left, right);
                } else {
                    session_.render(left, right);
                }

                // Interleaved into the ring, which is what the callback copies out of in one pass.
                for (int frame = 0; frame < block_frames; ++frame) {
                    const auto at = static_cast<std::size_t>(frame);
                    block[at * 2] = left[at];
                    block[at * 2 + 1] = right[at];
                }
                ring_.write(block);

                // Only while the song is actually moving. Live blocks add to the ring without
                // advancing the sequencer, so this subtraction would walk the reported position
                // backwards and drag the scrubber with it.
                if (!transport_idle) {
                    audible_.store(session_.position() - static_cast<std::int64_t>(ring_.queued()),
                                   std::memory_order_relaxed);
                }
            }

            // Roughly every 20 ms of audio: far finer than any display refresh, and cheap against
            // a block of synthesis.
            if (--until_publish <= 0) {
                publish();
                until_publish = std::max(1, Session::sample_rate / 50 / block_frames);
            }
        }

        if (nap.count() > 0) {
            std::this_thread::sleep_for(nap);
        }
    }

#ifdef _WIN32
    // Paired with AvSetMmThreadCharacteristicsW above. The registration is per-thread and this
    // thread is about to end, but reverting is still the documented contract and keeps the MMCSS
    // bookkeeping honest if the service is counting registrations.
    if (mmcss != nullptr) {
        AvRevertMmThreadCharacteristics(mmcss);
    }
#endif
}

} // namespace ts::host
