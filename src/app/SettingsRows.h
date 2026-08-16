#pragma once

#include <string>
#include <vector>

namespace tsgui
{
    /// The Adw.PreferencesGroup and Adw.ActionRow vocabulary, built here because it has to be.
    ///
    /// The obvious answer is the WinUI Community Toolkit's SettingsCard and SettingsExpander, and it
    /// is not available: CommunityToolkit.WinUI.* is a .NET library and this is a pure C++/WinRT
    /// application, which has no way to reference one.
    ///
    /// Free functions returning elements, not runtimeclasses. These are internal building blocks
    /// that nothing outside this program ever names; making them projected types would cost an .idl,
    /// a MIDL pass, a factory and a ControlTemplate apiece to say what a Grid says directly. The
    /// preferences are assembled in code rather than in markup for the same reason -- fifteen rows
    /// that differ only in their control read better as a loop than as fifteen blocks of XAML.

    /// The metrics every row in this program shares, exported because two files build rows on them:
    /// the preferences below, and the song information window, whose rows differ enough in content --
    /// a selectable value, a paragraph of the file's own prose, a marker that can be clicked -- that
    /// they cannot go through MakeSettingsRow, but must still line up beside each other.
    inline constexpr double kRowCornerRadius = 4.0;
    inline constexpr double kRowPaddingX = 14.0;
    inline constexpr double kRowPaddingY = 10.0;

    /// The surface a row is drawn on: bordered, rounded and padded, in the theme's own card colours.
    winrt::Microsoft::UI::Xaml::FrameworkElement MakeCard(
        winrt::Microsoft::UI::Xaml::UIElement const& content);

    /// One row: a title, an optional sentence under it, and a control on the right.
    ///
    /// An empty description collapses rather than reserving its line, because most rows do not need
    /// one and a row that kept the space would be taller than it has any reason to be.
    winrt::Microsoft::UI::Xaml::FrameworkElement MakeSettingsRow(
        std::wstring_view title,
        std::wstring_view description,
        winrt::Microsoft::UI::Xaml::UIElement const& control);

    /// A titled group of rows.
    winrt::Microsoft::UI::Xaml::FrameworkElement MakeSettingsGroup(
        std::wstring_view title,
        std::vector<winrt::Microsoft::UI::Xaml::FrameworkElement> const& rows);
}
