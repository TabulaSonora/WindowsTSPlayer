#pragma once

#include "MainWindow.g.h"

#include <memory>

// Forward-declared rather than included.
//
// The host layer's headers drag in the engine's, and the engine's are compiled by the CMake half
// with its own flags. Keeping them out of a header the XAML compiler also generates against means
// the generated code, the projection and the engine never have to agree on warning levels or on
// what windows.h has already defined. The cost is a destructor that has to be declared here and
// defined in the .cpp, where both types are complete.
namespace ts::host {
class Player;
class AudioDevice;
}

namespace winrt::WindowsTSPlayer::implementation
{
    struct MainWindow : MainWindowT<MainWindow>
    {
        MainWindow();
        ~MainWindow();

        winrt::fire_and_forget OnOpenRomClick(
            IInspectable const& sender, Microsoft::UI::Xaml::RoutedEventArgs const& args);
        winrt::fire_and_forget OnOpenSongClick(
            IInspectable const& sender, Microsoft::UI::Xaml::RoutedEventArgs const& args);
        void OnPlayPauseClick(
            IInspectable const& sender, Microsoft::UI::Xaml::RoutedEventArgs const& args);

    private:
        void Tick();
        HWND Hwnd();

        std::unique_ptr<ts::host::Player> player_;
        std::unique_ptr<ts::host::AudioDevice> device_;

        Microsoft::UI::Dispatching::DispatcherQueueTimer timer_{ nullptr };
    };
}

namespace winrt::WindowsTSPlayer::factory_implementation
{
    struct MainWindow : MainWindowT<MainWindow, implementation::MainWindow>
    {
    };
}
