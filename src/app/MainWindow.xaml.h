#pragma once

#include "MainWindow.g.h"

namespace winrt::WindowsTSPlayer::implementation
{
    struct MainWindow : MainWindowT<MainWindow>
    {
        MainWindow();

        WindowsTSPlayer::PlayerModel Model() const { return model_; }

        winrt::fire_and_forget OnOpenRomClick(
            IInspectable const& sender, Microsoft::UI::Xaml::RoutedEventArgs const& args);
        winrt::fire_and_forget OnOpenSongClick(
            IInspectable const& sender, Microsoft::UI::Xaml::RoutedEventArgs const& args);
        void OnPlayPauseClick(
            IInspectable const& sender, Microsoft::UI::Xaml::RoutedEventArgs const& args);
        void OnRestartClick(
            IInspectable const& sender, Microsoft::UI::Xaml::RoutedEventArgs const& args);
        void OnPanicClick(
            IInspectable const& sender, Microsoft::UI::Xaml::RoutedEventArgs const& args);
        void OnLoopClick(
            IInspectable const& sender, Microsoft::UI::Xaml::RoutedEventArgs const& args);

    private:
        void OnModelPropertyChanged(
            IInspectable const& sender,
            Microsoft::UI::Xaml::Data::PropertyChangedEventArgs const& args);
        void UpdateReadouts();

        HWND Hwnd();

        WindowsTSPlayer::PlayerModel model_{ nullptr };

        /// How many times the visible list has been altered, and how many model updates have
        /// arrived.
        ///
        /// This exists to make the milestone's claim checkable rather than assertable: play a
        /// sixteen-part file and the first number must settle and stay put while the second climbs.
        /// A development readout; it goes when the real mixer arrives.
        uint64_t listChanges_{ 0 };
        uint64_t updates_{ 0 };
    };
}

namespace winrt::WindowsTSPlayer::factory_implementation
{
    struct MainWindow : MainWindowT<MainWindow, implementation::MainWindow>
    {
    };
}
