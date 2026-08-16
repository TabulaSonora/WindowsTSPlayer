#pragma once

#include "MainWindow.g.h"

#include "SettingsStore.h"

#include <memory>

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
        winrt::fire_and_forget OnPrefsClick(
            IInspectable const& sender, Microsoft::UI::Xaml::RoutedEventArgs const& args);

        void OnDragOver(IInspectable const& sender, Microsoft::UI::Xaml::DragEventArgs const& args);
        winrt::fire_and_forget OnDrop(
            IInspectable const& sender, Microsoft::UI::Xaml::DragEventArgs const& args);

    private:
        void OnModelPropertyChanged(
            IInspectable const& sender,
            Microsoft::UI::Xaml::Data::PropertyChangedEventArgs const& args);

        /// Turns whatever the store now holds into engine settings, in one call.
        void ApplySettings();

        /// Copies a chosen SCCore.dll into the application's own storage and loads it from there.
        winrt::fire_and_forget ImportRom(Windows::Storage::StorageFile file);

        /// Loads the imported ROM, if there is one.
        winrt::fire_and_forget RestoreRom();

        winrt::Windows::Foundation::IAsyncOperation<bool> OpenSong(hstring path);

        void RememberSong(Windows::Storage::StorageFile const& file);
        winrt::fire_and_forget RebuildRecentMenu();

        void RestoreWindowGeometry();
        void SaveWindowGeometry();

        /// Shows or hides the import button. Hidden once a ROM is in place, because importing is a
        /// first-run step rather than something anyone does twice.
        void ShowRomImport(bool show);

        void SetWindowIcon();
        void SetUpBackdrop();

        HWND Hwnd();

        std::unique_ptr<tsgui::SettingsStore> settings_;
        WindowsTSPlayer::PlayerModel model_{ nullptr };

        /// How many times the visible list has been altered, and how many model updates have
        /// arrived. A development readout; it goes when there is nothing left to demonstrate.
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
