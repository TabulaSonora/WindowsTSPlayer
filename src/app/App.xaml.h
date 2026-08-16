#pragma once

#include "App.xaml.g.h"

namespace winrt::WindowsTSPlayer::implementation
{
    struct App : AppT<App>
    {
        App();

        void OnLaunched(Microsoft::UI::Xaml::LaunchActivatedEventArgs const& args);

    private:
        // Held so the window is not collected the moment OnLaunched returns. Single-instance
        // redirection and file activation arrive in a later milestone; this is the plain
        // one-window case.
        winrt::Microsoft::UI::Xaml::Window window_{ nullptr };
    };
}
