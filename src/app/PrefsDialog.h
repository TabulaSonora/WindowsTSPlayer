#pragma once

#include "SettingsStore.h"

#include <winrt/Microsoft.UI.Xaml.Controls.h>

namespace tsgui
{
    /// Builds and shows the preferences.
    ///
    /// A ContentDialog rather than a window, which is the ContentDialog/AdwPreferencesDialog
    /// correspondence the port has been following. Note that it needs its XamlRoot set before it is
    /// shown or it throws outright -- a dialog in a desktop app has no implicit one to inherit, and
    /// the exception names neither the dialog nor the missing root.
    ///
    /// Every control writes the store and nothing else. The store raises its change, and one apply
    /// path turns that into engine settings; a control that also poked the engine would give the same
    /// state two owners.
    winrt::Windows::Foundation::IAsyncAction ShowPreferencesAsync(
        SettingsStore& settings,
        winrt::Microsoft::UI::Xaml::XamlRoot const& root);
}
