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
        winrt::fire_and_forget OnExportClick(
            IInspectable const& sender, Microsoft::UI::Xaml::RoutedEventArgs const& args);

    private:
        /// Loads the ROM remembered from last time, if there still is one. Fire-and-forget from the
        /// constructor: a full identity check reads 27 MB and has no business blocking the window
        /// from appearing.
        winrt::fire_and_forget RestoreRom();

        void RememberRom(hstring const& path);
        void SetWindowIcon();

        void OnModelPropertyChanged(
            IInspectable const& sender,
            Microsoft::UI::Xaml::Data::PropertyChangedEventArgs const& args);

        HWND Hwnd();

        WindowsTSPlayer::PlayerModel model_{ nullptr };

        /// How many times the visible list has been altered, and how many model updates have
        /// arrived.
        ///
        /// This exists to make the mixer milestone's claim checkable rather than assertable: play a
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
