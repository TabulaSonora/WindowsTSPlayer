#include "pch.h"

#include "MediaControls.h"

#include "ToneMap.h"

// ISystemMediaTransportControlsInterop. GetForCurrentView is the documented way in and is UWP-only:
// it wants a CoreWindow, which a desktop application does not have. This interop interface is the
// desktop path and takes an HWND instead.
#include <systemmediatransportcontrolsinterop.h>

#include <cmath>
#include <chrono>
#include <string>

using namespace winrt;
using namespace Windows::Media;

namespace {

/// How far the position may move between two announcements before it counts as a jump.
///
/// The display ticks ten times a second, so ordinary playback advances about 0.1s per refresh.
/// Anything much larger in one step is a seek, and a seek is exactly the moment the shell's
/// extrapolated clock has gone wrong and has to be re-based. Half a second is loose enough that no
/// ordinary tick reaches it -- including one arriving late behind a busy render -- and tight enough
/// that a seek of any consequence is caught. A seek shorter than this leaves the flyout's clock off
/// by less than the half-second it was already rounding away.
constexpr double jump_threshold = 0.5;

/// The name a person would call the piece.
///
/// The extension goes: it is noise in a shell notification, where the file name is being shown as a
/// title rather than as a path. The same trimming the MPRIS build does for xesam:title.
hstring TitleOf(hstring const& song)
{
    std::wstring_view name{ song };
    const auto dot = name.find_last_of(L'.');
    return hstring{ dot == std::wstring_view::npos ? name : name.substr(0, dot) };
}

Windows::Foundation::TimeSpan Seconds(double value)
{
    if (!(value > 0.0)) {
        value = 0.0;
    }
    return std::chrono::duration_cast<Windows::Foundation::TimeSpan>(
        std::chrono::duration<double>{ value });
}

} // namespace

namespace tsgui
{
    MediaControls::MediaControls(winrt::WindowsTSPlayer::PlayerModel const& model,
                                 SettingsStore const& settings,
                                 HWND window)
        : model_(model)
        , dispatcher_(winrt::Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread())
        , settings_(&settings)
    {
        auto interop = get_activation_factory<SystemMediaTransportControls,
                                              ::ISystemMediaTransportControlsInterop>();
        check_hresult(interop->GetForWindow(window, guid_of<SystemMediaTransportControls>(),
                                            put_abi(controls_)));

        controls_.IsEnabled(true);

        // Next and Previous stay off for the life of the program rather than being toggled: there is
        // no playlist and there is never going to be one, so a shell that greys them out permanently
        // is telling the truth. The other three follow whether a song is loaded.
        controls_.IsNextEnabled(false);
        controls_.IsPreviousEnabled(false);

        // Handlers arrive on a background thread -- this is the counterpart of D-Bus method dispatch
        // in the GTK build, and touching the model from here is the classic first bug. Every one of
        // them hops to the model's own thread before doing anything.
        buttonToken_ = controls_.ButtonPressed(
            [this](auto&&, SystemMediaTransportControlsButtonPressedEventArgs const& args) {
                OnButton(args.Button());
            });

        repeatToken_ = controls_.AutoRepeatModeChangeRequested(
            [this](auto&&, AutoRepeatModeChangeRequestedEventArgs const& args) {
                const bool looping = args.RequestedAutoRepeatMode() != MediaPlaybackAutoRepeatMode::None;
                auto model = model_;
                if (model == nullptr || dispatcher_ == nullptr) {
                    return;
                }
                dispatcher_.TryEnqueue([model, looping]() { model.Looping(looping); });
            });

        positionToken_ = controls_.PlaybackPositionChangeRequested(
            [this](auto&&, PlaybackPositionChangeRequestedEventArgs const& args) {
                const double seconds =
                    std::chrono::duration<double>{ args.RequestedPlaybackPosition() }.count();
                auto model = model_;
                if (model == nullptr || dispatcher_ == nullptr) {
                    return;
                }
                dispatcher_.TryEnqueue([model, seconds]() { model.Seek(seconds); });
            });

        Sync();
    }

    MediaControls::~MediaControls()
    {
        Shutdown();
    }

    void MediaControls::Shutdown()
    {
        if (controls_ == nullptr) {
            return;
        }

        controls_.ButtonPressed(buttonToken_);
        controls_.AutoRepeatModeChangeRequested(repeatToken_);
        controls_.PlaybackPositionChangeRequested(positionToken_);

        // Closed rather than merely disabled, and then disabled. Closed is what tells the shell the
        // session is over; leaving it Paused would keep a now-playing entry for a program that has
        // gone, which the flyout will happily show until something else claims it.
        controls_.PlaybackStatus(MediaPlaybackStatus::Closed);
        controls_.IsEnabled(false);

        controls_ = nullptr;
        model_ = nullptr;
    }

    void MediaControls::OnButton(SystemMediaTransportControlsButton button)
    {
        auto model = model_;
        if (model == nullptr || dispatcher_ == nullptr) {
            return;
        }

        dispatcher_.TryEnqueue([model, button]() {
            switch (button) {
            case SystemMediaTransportControlsButton::Play:
                model.Play();
                break;
            case SystemMediaTransportControlsButton::Pause:
                model.Pause();
                break;
            case SystemMediaTransportControlsButton::Stop:
                // Stop means back to the start, held there. There is no stopped state of its own in
                // this model -- and "the start" is the song's first note, the same place Play on a
                // finished song goes, rather than the silent lead-in.
                model.Pause();
                model.Restart();
                break;
            default:
                // Next, Previous, Record, FastForward, Rewind, Channel and the rest. Declared
                // unsupported above, so a shell should not be sending them; ignored rather than
                // asserted on, because a shell is free to try.
                break;
            }
        });
    }

    void MediaControls::Sync()
    {
        if (controls_ == nullptr || model_ == nullptr) {
            return;
        }

        const hstring song = model_.SongName();
        const bool playing = model_.Playing();
        const bool looping = model_.Looping();
        const double position = model_.Position();
        const bool hasSong = !song.empty();

        const bool songChanged = song != publishedSong_;

        // A jump rather than a tick. Position moves about a tenth of a second per refresh, so a step
        // larger than that is a seek, and a seek is precisely when the shell's extrapolation has gone
        // wrong. Anything smaller is left alone: re-basing on every tick is what makes the flyout's
        // clock stutter instead of run.
        const bool jumped = std::abs(position - publishedPosition_) > jump_threshold;

        if (!songChanged && playing == publishedPlaying_ && looping == publishedLooping_ && !jumped) {
            return;
        }

        controls_.IsPlayEnabled(hasSong);
        controls_.IsPauseEnabled(hasSong);
        controls_.IsStopEnabled(hasSong);

        controls_.PlaybackStatus(!hasSong          ? MediaPlaybackStatus::Closed
                                 : playing         ? MediaPlaybackStatus::Playing
                                                   : MediaPlaybackStatus::Paused);

        // Track, not List: the repeat this offers is the file's own loop points, not a queue
        // wrapping round, and there is no queue to wrap.
        controls_.AutoRepeatMode(looping ? MediaPlaybackAutoRepeatMode::Track
                                         : MediaPlaybackAutoRepeatMode::None);

        // The metadata only when the song does. DisplayUpdater::Update is what redraws the flyout,
        // and calling it for a play/pause makes the artwork and title flicker for no reason.
        if (songChanged) {
            auto updater = controls_.DisplayUpdater();
            updater.ClearAll();
            updater.Type(MediaPlaybackType::Music);

            if (hasSong) {
                auto music = updater.MusicProperties();
                music.Title(TitleOf(song));

                // The module stands in for the artist, as it does in both other front ends. It is
                // the thing that gives a rendition its character, and a Standard MIDI File names no
                // performer -- so the alternative is a blank line where every other player shows one.
                music.Artist(to_hstring(tsgui::ToneMapDisplayName(settings_->map())));
            }

            updater.Update();
        }

        // Announced once per state change, with the range as well as the point, because the shell
        // needs both to draw a scrubber it can drag.
        SystemMediaTransportControlsTimelineProperties timeline;
        timeline.StartTime(Seconds(0.0));
        timeline.MinSeekTime(Seconds(0.0));
        timeline.Position(Seconds(position));
        timeline.MaxSeekTime(Seconds(model_.Duration()));
        timeline.EndTime(Seconds(model_.Duration()));
        controls_.UpdateTimelineProperties(timeline);

        publishedSong_ = song;
        publishedPlaying_ = playing;
        publishedLooping_ = looping;
        publishedPosition_ = position;
    }
}
