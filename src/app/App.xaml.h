#pragma once

#include "App.xaml.g.h"

#include <winrt/Microsoft.Windows.AppLifecycle.h>

#include <string>

namespace winrt::WindowsTSPlayer::implementation
{
    struct App : AppT<App>
    {
        App();

        void OnLaunched(Microsoft::UI::Xaml::LaunchActivatedEventArgs const& args);

    private:
        /// Takes a file from an activation and gives it to the window, whichever thread it arrived
        /// on.
        void OpenActivated(std::wstring path);

        // Held so the window is not collected the moment OnLaunched returns, and held as the
        // projected MainWindow rather than as a Window: get_self needs the projected type its
        // implementation belongs to, and a Microsoft::UI::Xaml::Window is the base rather than that.
        winrt::WindowsTSPlayer::MainWindow window_{ nullptr };

        /// The activation subscription, revoked when this object goes.
        ///
        /// Auto-revoking rather than a bare token, because the thing raising the event is the process
        /// singleton and outlives the application object. A handler left registered on it would be
        /// called with a window that no longer exists.
        winrt::Microsoft::Windows::AppLifecycle::AppInstance::Activated_revoker activated_;
    };
}
