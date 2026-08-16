#pragma once

#include "SettingsStore.h"

#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Windows.Media.h>
#include <winrt/WindowsTSPlayer.h>

namespace tsgui
{
    /// The shell's media controls: the now-playing flyout, the headset and keyboard media keys, and
    /// whatever else asks Windows what is playing.
    ///
    /// The counterpart of MPRIS2 in the GTK build and of the now-playing centre in the Apple one, and
    /// it takes the same lesson from both: **the elapsed position is published once with a rate, and
    /// the shell extrapolates from there.** So the timeline is announced when the state actually
    /// changes and never on the display tick. Pushing it ten times a second would not merely be
    /// wasteful -- it re-bases the extrapolation on every update, and the flyout's clock stutters
    /// rather than running.
    ///
    /// Identity-gated, which is part of why this program is packaged at all. GetForWindow needs an
    /// HWND belonging to a process the shell can name.
    ///
    /// One thing the MPRIS build does is deliberately not carried over: it mapped MPRIS's Volume onto
    /// the engine's output gain. Windows exposes per-application volume through the audio session,
    /// which the WASAPI stream already provides, so the shell's own mixer works without being asked
    /// and output gain stays a purely in-app control. Two volumes in series, one of them invisible
    /// from the app, is a thing nobody can reason about.
    class MediaControls
    {
    public:
        /// Attaches to the window and starts publishing. Holds the model but owns none of it.
        MediaControls(winrt::WindowsTSPlayer::PlayerModel const& model,
                      SettingsStore const& settings,
                      HWND window);
        ~MediaControls();

        MediaControls(MediaControls const&) = delete;
        MediaControls& operator=(MediaControls const&) = delete;

        /// Republishes status, metadata and timeline. Call on a state change, never on the tick.
        void Sync();

        /// Stops publishing and drops the model. Called before the model is torn down, so the shell
        /// is not left holding a now-playing entry for a program that has gone.
        void Shutdown();

    private:
        void OnButton(winrt::Windows::Media::SystemMediaTransportControlsButton button);

        winrt::WindowsTSPlayer::PlayerModel model_{ nullptr };

        /// The thread every one of these handlers has to get back onto before touching the model.
        /// Taken at construction, which happens on it; SMTC callbacks arrive on a pool thread and
        /// have no way to find it for themselves.
        winrt::Microsoft::UI::Dispatching::DispatcherQueue dispatcher_{ nullptr };
        SettingsStore const* settings_{ nullptr };

        winrt::Windows::Media::SystemMediaTransportControls controls_{ nullptr };
        winrt::event_token buttonToken_{};
        winrt::event_token repeatToken_{};
        winrt::event_token positionToken_{};

        /// What was last published, so Sync can tell a real change from a repeat. The shell is told
        /// only when one of these moves.
        winrt::hstring publishedSong_;
        bool publishedPlaying_{ false };
        bool publishedLooping_{ false };
        double publishedPosition_{ 0.0 };
    };
}
