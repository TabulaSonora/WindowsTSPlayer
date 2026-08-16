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
