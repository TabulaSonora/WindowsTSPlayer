#pragma once

#include "SettingsStore.h"

#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/WindowsTSPlayer.h>

namespace tsgui
{
    /// Builds the window that shows what a file says about itself: its container and timing, the
    /// module it asks for, its own text, its markers, its tracks and its lyric sheet.
    ///
    /// A window rather than a dialog, and that is the whole point of it. The player stays usable
    /// underneath, and this stays open across one file and the next -- reading the credits of a piece
    /// while it plays is the case it exists for, and a modal dialog would make that impossible.
    ///
    /// Assembled in code rather than declared in markup, and not for the reason the preferences are.
    /// There, fifteen near-identical rows read better as a loop. Here the content's *shape* is a
    /// function of the file: a row per track, a group that exists only when there are markers, a page
    /// that is either a lyric sheet or a shrug. There is no static tree to write down, so the markup
    /// would be an empty frame and every element would still be built here.
    ///
    /// Returned rather than shown. The caller owns it, which is what lets it be brought forward
    /// instead of opened twice and closed when the player closes.
    winrt::Microsoft::UI::Xaml::Window CreateSongInfoWindow(
        winrt::WindowsTSPlayer::PlayerModel const& model,
        SettingsStore& settings);
}
