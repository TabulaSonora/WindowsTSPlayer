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
        void OnSongInfoClick(
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

        /// Rebuilds the recent-files menu. Not a coroutine, despite reading a StorageApplicationPermissions
        /// list: Entries() is a synchronous property, and only opening one of them has to await
        /// anything -- which happens inside the item's own handler, not here. It was declared
        /// fire_and_forget by habit, and a function returning that type without ever suspending falls
        /// off its end returning nothing, which is undefined behaviour rather than an idle warning.
        void RebuildRecentMenu();

        void RestoreWindowGeometry();
        void SaveWindowGeometry();

        /// Shows or hides the import button. Hidden once a ROM is in place, because importing is a
        /// first-run step rather than something anyone does twice.
        void ShowRomImport(bool show);

        HWND Hwnd();

        std::unique_ptr<tsgui::SettingsStore> settings_;
        WindowsTSPlayer::PlayerModel model_{ nullptr };

        /// The song information window, while one is open.
        ///
        /// Held rather than let go of, for two reasons that pull the same way. A second click has to
        /// bring the existing window forward instead of opening a duplicate, and this window has to
        /// close it on the way out -- the child holds a subscription to the model and a strong
        /// reference through every marker row, so a window left open would keep the engine alive after
        /// the player had gone.
        Microsoft::UI::Xaml::Window songInfoWindow_{ nullptr };

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
