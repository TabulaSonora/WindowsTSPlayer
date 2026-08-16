#pragma once

#include "MainWindow.g.h"

#include "MediaControls.h"
#include "SettingsStore.h"

#include <memory>

namespace winrt::WindowsTSPlayer::implementation
{
    struct MainWindow : MainWindowT<MainWindow>
    {
        MainWindow();

        WindowsTSPlayer::PlayerModel Model() const { return model_; }

        /// Opens a song that arrived from a file association or the command line.
        ///
        /// Not a projected method -- nothing outside this program calls it, and App reaches it
        /// through get_self like everything else that crosses that boundary.
        ///
        /// A file named before the ROM is loaded is stashed rather than refused. The first run of a
        /// fresh install is exactly the case where someone double-clicks a .mid, and "no ROM yet" is
        /// a state that lasts a few hundred milliseconds on every later launch besides -- refusing on
        /// that basis would be a race the reader loses at random.
        void OpenActivatedFile(hstring path);

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

        /// Writes the title bar's two lines from the model: the song above, what it is playing on
        /// below. The lower line is shared with transient status, so this is also how the strip
        /// returns to saying something true once a message has been shown there.
        void SyncHeading();

        /// Copies a chosen SCCore.dll into the application's own storage and loads it from there.
        winrt::fire_and_forget ImportRom(Windows::Storage::StorageFile file);

        /// Loads the imported ROM, if there is one.
        winrt::fire_and_forget RestoreRom();

        /// Plays whatever was stashed while there was no ROM, and forgets it.
        winrt::fire_and_forget OpenPendingSong();

        /// Opens a song and files it under recents, which is the whole of what an activation does
        /// once there is a ROM to play it on.
        winrt::fire_and_forget OpenActivatedSong(hstring path);

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

        /// The shell's media controls. Built after the window has an HWND, because that is what
        /// GetForWindow needs, and torn down before the model so the now-playing entry does not
        /// outlive the thing it names.
        std::unique_ptr<tsgui::MediaControls> media_;
        WindowsTSPlayer::PlayerModel model_{ nullptr };

        /// A song named before there was a ROM to play it on. Empty the rest of the time.
        hstring pendingSong_;

        /// The two subscriptions this window holds on the model, kept so they can be dropped when it
        /// closes. Both handlers reach back through a bare `this`.
        winrt::event_token propertyToken_{};
        winrt::event_token vectorToken_{};

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
