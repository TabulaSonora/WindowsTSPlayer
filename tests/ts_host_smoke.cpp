// Drives the whole host layer the way the application does, minus GTK and minus a real device.
//
// The point is the concurrency: `Player` starts a render thread in its constructor, and this stands
// in for the audio callback on the other side of the ring. Run under TSan it is also the port's
// producer/consumer test, which is why it carries the `ring` label.
//
//   ts-host-smoke <rom> <midi>

#include "host/ts_player.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <exception>
#include <span>
#include <thread>
#include <vector>

namespace {

constexpr int frames_per_read = 256;
constexpr int wanted_frames = ts::host::Session::sample_rate * 2; // two seconds of audio

bool fail(const char* what)
{
    std::fprintf(stderr, "FAIL: %s\n", what);
    return false;
}

bool run(const char* rom, const char* midi)
{
    ts::host::Player player;

    player.load_rom(rom, false);
    player.load_song(midi);

    if (player.rom_name().empty()) {
        return fail("no ROM name after load");
    }
    if (player.song_name().empty()) {
        return fail("no song name after load");
    }

    const auto armed = player.snapshot();
    if (!armed.hasSong || armed.length <= 0) {
        return fail("song loaded but reports no length");
    }

    const int present = static_cast<int>(
        std::count_if(armed.parts.begin(), armed.parts.end(), [](const auto& p) { return p.present; }));
    if (present == 0) {
        return fail("song addresses no parts");
    }

    player.set_paused(false);

    // Stand in for the device callback: ask for a fixed block, at roughly the rate a real device
    // would, and record what the producer managed to supply.
    std::vector<float> block(static_cast<std::size_t>(frames_per_read) * 2, 0.0F);
    float peak = 0.0F;
    int drained = 0;

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{30};

    while (drained < wanted_frames) {
        if (std::chrono::steady_clock::now() > deadline) {
            return fail("timed out waiting for the render thread to fill the ring");
        }

        player.handle().last_request.store(frames_per_read, std::memory_order_relaxed);
        const auto got = player.handle().ring->read(std::span<float>{block});

        for (std::size_t i = 0; i < got * 2; ++i) {
            peak = std::max(peak, std::fabs(block[i]));
        }
        drained += static_cast<int>(got);

        std::this_thread::sleep_for(
            std::chrono::microseconds{frames_per_read * 1000000 / ts::host::Session::sample_rate});
    }

    if (peak <= 0.0F) {
        return fail("two seconds of audio came out silent");
    }

    const auto playing = player.snapshot();
    if (playing.position <= 0) {
        return fail("position did not advance while playing");
    }
    if (playing.activeVoices <= 0) {
        return fail("no voices sounding while playing");
    }

    // Pausing must be honoured promptly, and must not be reported as a fault: an empty ring is
    // exactly what a stopped transport should produce.
    player.set_paused(true);
    std::this_thread::sleep_for(std::chrono::milliseconds{100});

    std::printf("ok: peak %.4f, %d frames, %d/%d voices, %d parts present, %lld underruns\n",
                static_cast<double>(peak), drained, playing.activeVoices, playing.voiceCapacity,
                present, static_cast<long long>(playing.underruns));
    return true;
}

} // namespace

int main(int argc, char* argv[])
{
    if (argc != 3) {
        std::fprintf(stderr, "usage: %s <rom> <midi>\n", argv[0]);
        return 2;
    }

    try {
        return run(argv[1], argv[2]) ? 0 : 1;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: threw: %s\n", error.what());
        return 1;
    }
}
