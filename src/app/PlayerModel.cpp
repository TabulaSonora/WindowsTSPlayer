#include "pch.h"

#include "PlayerModel.h"
#if __has_include("PlayerModel.g.cpp")
#include "PlayerModel.g.cpp"
#endif

#include "PartModel.h"

#include "host/ts_audio_device.hpp"
#include "host/ts_player.hpp"

#include "tabulasonora/rom_image.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <exception>
#include <format>
#include <vector>

using namespace winrt;
using namespace Microsoft::UI::Xaml::Data;
using namespace Windows::Foundation;
using namespace Windows::Foundation::Collections;

namespace {

/// Ten times a second, matching the other two front ends. The render thread publishes five times
/// faster than this; the extra resolution would not survive a display refresh.
constexpr std::chrono::milliseconds tick_interval{ 100 };

} // namespace

namespace winrt::WindowsTSPlayer::implementation
{
    PlayerModel::PlayerModel()
        : player_(std::make_unique<ts::host::Player>())
        , songInfo_(std::make_unique<ts::host::SongInfo>())
        , dispatcher_(Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread())
        , allParts_(single_threaded_vector<WindowsTSPlayer::PartModel>())
        , visibleParts_(single_threaded_observable_vector<WindowsTSPlayer::PartModel>())
    {
        // Sixty-four, created once and never replaced. Which of them the song addresses is the
        // Present property; the collection itself never changes length, which is what makes the
        // visible list a diff rather than a rebuild.
        for (int i = 0; i < TS_MAX_PARTS; ++i) {
            allParts_.Append(make<PartModel>(i));
        }

        routing_.fill(-1);
    }

    // Defined here, not in the header: unique_ptr needs both types complete to destroy them, and
    // keeping the engine's headers out of the header is the point of forward-declaring them.
    PlayerModel::~PlayerModel()
    {
        if (timer_) {
            timer_.Stop();
        }
        if (device_) {
            device_->stop();
        }
    }

    // -- Property plumbing -------------------------------------------------------------------------

    template <typename T>
    void PlayerModel::Set(T& field, T value, hstring const& name)
    {
        if (field != value) {
            field = value;
            propertyChanged_(*this, PropertyChangedEventArgs{ name });
        }
    }

    void PlayerModel::SetText(hstring& field, const std::string& value, hstring const& name)
    {
        hstring wanted = to_hstring(value);
        if (field != wanted) {
            field = std::move(wanted);
            propertyChanged_(*this, PropertyChangedEventArgs{ name });
        }
    }

    event_token PlayerModel::PropertyChanged(PropertyChangedEventHandler const& handler)
    {
        return propertyChanged_.add(handler);
    }

    void PlayerModel::PropertyChanged(event_token const& token) noexcept
    {
        propertyChanged_.remove(token);
    }

    event_token PlayerModel::PresenceChanged(EventHandler<IInspectable> const& handler)
    {
        return presenceChanged_.add(handler);
    }

    void PlayerModel::PresenceChanged(event_token const& token) noexcept
    {
        presenceChanged_.remove(token);
    }

    event_token PlayerModel::RoutingChanged(EventHandler<IInspectable> const& handler)
    {
        return routingChanged_.add(handler);
    }

    void PlayerModel::RoutingChanged(event_token const& token) noexcept
    {
        routingChanged_.remove(token);
    }

    // -- The tick ----------------------------------------------------------------------------------

    void PlayerModel::Refresh()
    {
        const auto snapshot = player_->snapshot();
        const auto rate = static_cast<double>(ts::host::Session::sample_rate);

        Set(position_, static_cast<double>(snapshot.position) / rate, L"Position");
        Set(duration_, static_cast<double>(snapshot.length) / rate, L"Duration");
        Set(activeVoices_, snapshot.activeVoices, L"ActiveVoices");
        Set(voiceCapacity_, snapshot.voiceCapacity, L"VoiceCapacity");
        Set(xgMode_, snapshot.xgMode, L"XgMode");
        Set(underruns_, static_cast<int64_t>(snapshot.underruns), L"Underruns");
        Set(playing_, !snapshot.paused, L"Playing");

        // Solo is a property of the whole mixer, not of one strip: a part is dimmed when something
        // *else* is soloed.
        const bool any_soloed = std::any_of(snapshot.parts.begin(), snapshot.parts.end(),
                                            [](const auto& part) { return part.soloed; });

        std::uint64_t presence = 0;
        std::array<signed char, TS_MAX_PARTS> routing{};

        const uint32_t count = allParts_.Size();
        for (uint32_t i = 0; i < count; ++i) {
            auto part = get_self<PartModel>(allParts_.GetAt(i));
            const auto& state = snapshot.parts[i];

            part->Update(state, any_soloed && !state.soloed);

            if (state.present) {
                presence |= std::uint64_t{ 1 } << i;
            }
            routing[i] = static_cast<signed char>(part->Channel());
        }

        // The visible list has no reason to re-derive on its own: the backing collection never
        // changes length, only the items' properties change. Recomputing it here is what keeps the
        // mixer correct, and only when the set actually differs -- otherwise every row would be
        // touched ten times a second for nothing.
        const bool presence_changed = presence != presence_;
        const bool routing_changed = std::memcmp(routing.data(), routing_.data(), routing.size()) != 0;

        if (presence_changed) {
            presence_ = presence;
        }

        // And the same again for the order. A bulk dump can move a part without changing which parts
        // are addressed at all, so this is a separate question from presence and asked separately.
        if (routing_changed) {
            routing_ = routing;
        }

        if (presence_changed || routing_changed) {
            RebuildVisible();
        }

        // Announced after the list is right, not before: a handler that reads VisibleParts would
        // otherwise see the previous set.
        if (presence_changed) {
            presenceChanged_(*this, nullptr);
        }
        if (routing_changed) {
            routingChanged_(*this, nullptr);
        }

        // Reaching the end pauses rather than spinning at the tail, exactly as the Apple build does.
        if (snapshot.complete && !complete_) {
            Set(complete_, true, L"Complete");
            Pause();
        } else if (!snapshot.complete && complete_) {
            Set(complete_, false, L"Complete");
        }
    }

    void PlayerModel::StartTicking()
    {
        if (timer_ || !dispatcher_) {
            return;
        }

        timer_ = dispatcher_.CreateTimer();
        timer_.Interval(tick_interval);
        timer_.Tick([this](auto&&, auto&&) { Refresh(); });
        timer_.Start();
    }

    // -- The visible list --------------------------------------------------------------------------

    void PlayerModel::RebuildVisible()
    {
        // In the order the strips are *labelled*, which is the receive channel and not the slot.
        //
        // Listing by slot while labelling by channel is worse than either alone: a file that moves a
        // part shows 10, 1, 2, 3 down the left edge, which reads as broken numbering rather than as
        // information. Port first, so a multi-port score still groups the way it is labelled, and
        // the slot breaks ties -- GS allows two parts pointed at one channel and both strips have to
        // appear.
        std::vector<WindowsTSPlayer::PartModel> wanted;
        wanted.reserve(TS_MAX_PARTS);

        for (uint32_t i = 0; i < allParts_.Size(); ++i) {
            auto part = allParts_.GetAt(i);
            if (get_self<PartModel>(part)->Present()) {
                wanted.push_back(part);
            }
        }

        std::sort(wanted.begin(), wanted.end(),
                  [](WindowsTSPlayer::PartModel const& a, WindowsTSPlayer::PartModel const& b) {
                      auto* left = get_self<PartModel>(a);
                      auto* right = get_self<PartModel>(b);
                      if (left->Port() != right->Port()) {
                          return left->Port() < right->Port();
                      }
                      if (left->Channel() != right->Channel()) {
                          return left->Channel() < right->Channel();
                      }
                      return left->Index() < right->Index();
                  });

        // A minimal diff, not Clear() followed by Append(). Clearing would raise a Reset on the
        // observable vector, and a ListView answers Reset by dropping every container and building
        // them again -- so the sixteen strips of a GM score would be rebuilt on every presence
        // change, which is exactly the churn the update-in-place design exists to prevent. Scroll
        // position, focus and any in-flight pointer interaction go with them.
        //
        // O(n^2) over at most sixty-four items, and only on a change. The straightforward version is
        // worth more here than a cleverer one.
        auto& visible = visibleParts_;

        for (uint32_t i = visible.Size(); i-- > 0;) {
            auto current = visible.GetAt(i);
            if (std::find(wanted.begin(), wanted.end(), current) == wanted.end()) {
                visible.RemoveAt(i);
            }
        }

        for (uint32_t i = 0; i < wanted.size(); ++i) {
            if (i < visible.Size() && visible.GetAt(i) == wanted[i]) {
                continue;
            }

            uint32_t found = 0;
            bool have = false;
            for (uint32_t j = i; j < visible.Size(); ++j) {
                if (visible.GetAt(j) == wanted[i]) {
                    found = j;
                    have = true;
                    break;
                }
            }

            if (have) {
                auto item = visible.GetAt(found);
                visible.RemoveAt(found);
                visible.InsertAt(i, item);
            } else {
                visible.InsertAt(i, wanted[i]);
            }
        }
    }

    // -- Loading -----------------------------------------------------------------------------------

    IAsyncOperation<bool> PlayerModel::LoadRomAsync(hstring path, bool verifyFully)
    {
        auto lifetime = get_strong();
        const std::string narrow = to_string(path);

        std::string error;

        // Off the UI thread: a full verification hashes 27 MB, and even the quick path builds the
        // engine over the ROM and loads the wavetables.
        co_await resume_background();
        try {
            player_->load_rom(narrow, verifyFully);
        } catch (const ts::RomIdentityError& wrong) {
            error = wrong.what();
        } catch (const std::exception& failure) {
            error = failure.what();
        }
        co_await wil::resume_foreground(dispatcher_);

        SetText(lastError_, error, L"LastError");
        if (!error.empty()) {
            co_return false;
        }

        SetText(romName_, player_->rom_name(), L"RomName");

        // Adopt the engine's own defaults rather than asserting ours. There is no session to ask
        // before a ROM is loaded, so this is the first moment the real value exists, and a gain
        // fader initialised to a guess would move the sound the instant anyone touched it.
        Set(outputGain_, player_->settings().outputGain, L"OutputGain");

        StartTicking();
        co_return true;
    }

    IAsyncOperation<bool> PlayerModel::LoadSongAsync(hstring path)
    {
        auto lifetime = get_strong();
        const std::string narrow = to_string(path);

        std::string error;
        std::string backend;

        co_await resume_background();
        try {
            player_->load_song(narrow);

            // The device is opened only once there is something to hear. Opening it at startup would
            // hold an audio endpoint for a program sitting on its setup screen.
            if (!device_) {
                device_ = std::make_unique<ts::host::AudioDevice>(*player_);
            }
            device_->start();
            backend = std::format("{} at {} Hz", device_->backend_name(), device_->device_rate());
        } catch (const std::exception& failure) {
            error = failure.what();
        }
        co_await wil::resume_foreground(dispatcher_);

        SetText(lastError_, error, L"LastError");
        if (!error.empty()) {
            co_return false;
        }

        SetText(backendDescription_, backend, L"BackendDescription");

        // Read once here rather than on the tick: it is fixed for the life of a song, and SongName
        // changing is the signal that it is worth reading again.
        *songInfo_ = player_->song_info();
        SetText(songName_, player_->song_name(), L"SongName");

        // Cleared here rather than waiting for the tick to notice: Play() below checks it, and a
        // song loaded after a previous one ran out would otherwise restart instead of playing.
        Set(complete_, false, L"Complete");

        StartTicking();
        Play();

        // One refresh straight away so the mixer is populated before the first tick, rather than a
        // tenth of a second of empty strips every time a song is opened.
        Refresh();
        co_return true;
    }

    void PlayerModel::UnloadSong()
    {
        player_->unload_song();
        *songInfo_ = ts::host::SongInfo{};
        SetText(songName_, std::string{}, L"SongName");
        Set(playing_, false, L"Playing");
        Refresh();
    }

    const ts::host::SongInfo& PlayerModel::SongInfo() const
    {
        // Returned by reference from a member cached at load, not read from the player on demand.
        // Player::song_info copies the whole struct out under the control lock, and this is reached
        // from a window that wants to bind several fields of it -- so asking once per load is right
        // and asking once per caller would take that lock while the render thread wants it.
        return *songInfo_;
    }

    // -- Transport ---------------------------------------------------------------------------------

    void PlayerModel::Play()
    {
        // A finished song starts again rather than sitting at the tail doing nothing, which is what
        // pressing Play on it visibly means.
        if (complete_) {
            Restart();
            return;
        }
        player_->set_paused(false);
        Set(playing_, true, L"Playing");
    }

    void PlayerModel::Pause()
    {
        player_->set_paused(true);
        Set(playing_, false, L"Playing");
    }

    void PlayerModel::TogglePlaying()
    {
        if (playing_) {
            Pause();
        } else {
            Play();
        }
    }

    void PlayerModel::Seek(double seconds)
    {
        player_->seek(static_cast<std::int64_t>(seconds * ts::host::Session::sample_rate));
        Set(complete_, false, L"Complete");
    }

    void PlayerModel::Restart()
    {
        player_->seek(player_->start_frame());
        Set(complete_, false, L"Complete");
        player_->set_paused(false);
        Set(playing_, true, L"Playing");
    }

    void PlayerModel::Panic() { player_->panic(); }

    void PlayerModel::Looping(bool value)
    {
        if (looping_ == value) {
            return;
        }
        player_->set_looping(value);
        Set(looping_, value, L"Looping");
    }

    // -- Gain and export ---------------------------------------------------------------------------

    void PlayerModel::OutputGain(double value)
    {
        value = std::clamp(value, 0.0, 2.0);
        if (outputGain_ == value) {
            return;
        }

        // Read the whole struct back from the session and put it straight back with one field
        // changed, rather than keeping a mirror here. There is one copy of the truth, so a
        // preferences dialog opened twice cannot disagree with itself -- and set_settings only
        // rebuilds the generator when a *structural* field differs, which gain is not.
        TSEngineSettings settings = player_->settings();
        settings.outputGain = value;
        player_->set_settings(settings);

        Set(outputGain_, value, L"OutputGain");
    }

    IAsyncOperation<bool> PlayerModel::ExportWavAsync(hstring path)
    {
        auto lifetime = get_strong();
        const std::string narrow = to_string(path);

        Set(exporting_, true, L"Exporting");

        std::string error;

        co_await resume_background();
        try {
            // The callback is consulted exactly twice -- once at 0.0 before render_to_end and once
            // at 1.0 after it -- so returning true unconditionally is the whole of what it can
            // usefully do. It is not a place to report from and not a place to cancel from; see
            // Session::run_export, where the single-call render is what keeps an export
            // byte-identical to `tabula-sonora render`.
            player_->export_wav(narrow, [](double) { return true; });
        } catch (const std::exception& failure) {
            error = failure.what();
        }
        co_await wil::resume_foreground(dispatcher_);

        Set(exporting_, false, L"Exporting");
        SetText(lastError_, error, L"LastError");
        co_return error.empty();
    }

    // -- Mixer -------------------------------------------------------------------------------------

    void PlayerModel::SetMuted(int32_t part, bool muted) { player_->set_muted(part, muted); }
    void PlayerModel::SetSoloed(int32_t part, bool soloed) { player_->set_soloed(part, soloed); }
    void PlayerModel::ResetChannels() { player_->reset_channels(); }
}
