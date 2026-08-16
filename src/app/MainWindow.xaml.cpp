#include "pch.h"

#include "MainWindow.xaml.h"
#if __has_include("MainWindow.g.cpp")
#include "MainWindow.g.cpp"
#endif

#include "MixerView.xaml.h"
#include "PlayerModel.h"
#include "PrefsDialog.h"
#include "SongFormats.h"
#include "SongInfoWindow.h"
#include "TransportView.xaml.h"
#include "WindowChrome.h"

// IWindowNative, which is how a WinUI 3 desktop window surrenders its HWND. There is no projected
// way to ask: the window is a XAML object and the HWND is an implementation detail everywhere except
// at the places Win32 still demands one, the pickers below being the first.
#include <microsoft.ui.xaml.window.h>

// IInitializeWithWindow. A picker shown from a desktop app has no CoreWindow to parent itself to,
// and without this it throws rather than falling back to anything.
#include <shobjidl_core.h>

#include <algorithm>
#include <format>
#include <string>

using namespace winrt;
using namespace Microsoft::UI::Xaml;
using namespace Windows::ApplicationModel::DataTransfer;
using namespace Windows::Foundation;
using namespace Windows::Storage;
using namespace Windows::Storage::AccessCache;
using namespace Windows::Storage::Pickers;

namespace {

/// The imported ROM's name inside the application's own storage, and the name it is staged under
/// while the copy is in flight.
///
/// Two names rather than one, and the staging one is not decoration. The engine reads the ROM
/// positionally for the whole session, so what it is pointed at has to be complete before it is
/// pointed at. A copy written straight to the final name is a valid path holding a truncated file
/// for as long as the copy takes, and a launch during that window loads it.
constexpr wchar_t rom_name[] = L"SCCore.dll";
constexpr wchar_t rom_staging_name[] = L"SCCore.dll.importing";

} // namespace

namespace winrt::WindowsTSPlayer::implementation
{
    MainWindow::MainWindow()
        : settings_(std::make_unique<tsgui::SettingsStore>())
        , model_(WindowsTSPlayer::PlayerModel())
    {
        Title(L"Tabula Sonora Player");

        // Explicitly, and before anything reaches for a named element. Nothing calls this on a
        // Window's behalf ahead of the constructor body, so the accessors below are null without it
        // and the process dies with an access violation before the window is ever shown. It is
        // idempotent, so calling it here costs nothing if the framework has already been through it.
        InitializeComponent();

        // Shared with the song information window, so the two wear the same chrome. A second window
        // with a system-drawn caption beside a first with a drawn one does not read as one program.
        const auto window = try_as<Window>();
        tsgui::SetWindowIcon(window);
        tsgui::SetUpWindowChrome(window, TitleBarArea(), TitleDragArea(), true);

        RestoreWindowGeometry();

        Transport().Model(model_);
        Mixer().Model(model_);

        // Both tokens are kept, because both of these handlers hold a bare `this`. A C++/WinRT
        // delegate built from `{ this, &Method }` stores a raw pointer and not a reference, so a
        // subscription left in place after the window has gone is a call through a dangling pointer
        // rather than a harmless no-op -- which is precisely what faulted the process on exit.
        //
        // The obvious hardening, `{ get_weak(), &Method }`, is **not available here** and the failure
        // is a wall of template errors in base.h rather than anything naming the cause: a Window
        // implementation derives through a composable base, and the weak-reference support
        // winrt::implements gives an ordinary runtimeclass does not reach it. Revoking below is
        // therefore the whole of the defence rather than the second half of one, which is worth
        // knowing before anyone deletes it as redundant.
        propertyToken_ = model_.PropertyChanged({ this, &MainWindow::OnModelPropertyChanged });
        vectorToken_ = model_.VisibleParts().VectorChanged([this](auto&&, auto&&) { ++listChanges_; });

        // One handler, one rebuild. N keys changed in the preferences cost one pass through the
        // engine rather than N, which is the whole reason the store hands over a struct.
        settings_->changed = [this](const std::string& key) {
            // Window geometry and the ROM's verified flag live here too and mean nothing to the
            // engine. Rebuilding the generator when the window is resized would be an audible bug.
            if (key.rfind("window-", 0) == 0 || key == "rom-verified") {
                return;
            }
            ApplySettings();
        };

        ApplySettings();

        Closed([this](IInspectable const&, WindowEventArgs const&) {
            SaveWindowGeometry();

            // Before the model goes, not after. The information window subscribes to the model and
            // holds it strongly through every marker row, so a copy left open would keep the engine
            // and its render thread alive with nothing left to publish into.
            if (songInfoWindow_ != nullptr) {
                songInfoWindow_.Close();
                songInfoWindow_ = nullptr;
            }

            // Unsubscribed before anything else, because both handlers reach this window through a
            // raw pointer and this window is about to stop existing.
            model_.PropertyChanged(propertyToken_);
            model_.VisibleParts().VectorChanged(vectorToken_);

            // Told to stop, rather than left to stop when its last reference goes.
            //
            // **This window is not the model's last owner**, and believing it was is what made the
            // program fault on the way out. Every mixer strip holds the model too, and those live
            // until the list containing them is collected -- so clearing the three references below
            // and expecting the destructor to run was expecting the wrong thing. The model stayed
            // alive with its 100 ms tick still going, publishing into a window that was already being
            // torn down, and the process died with an access violation just as it closed. The only
            // visible symptom was the cursor showing busy for a moment: the window was gone by then,
            // so there was nothing left to look wrong.
            get_self<implementation::PlayerModel>(model_)->Shutdown();

            Transport().Model(nullptr);
            Mixer().Model(nullptr);
            model_ = nullptr;
        });

        RestoreRom();
        RebuildRecentMenu();
    }

    // -- Settings ----------------------------------------------------------------------------------

    void MainWindow::ApplySettings()
    {
        auto model = get_self<implementation::PlayerModel>(model_);
        model->ApplyEngineSettings(settings_->engine_settings());

        // Applied beside the struct, never through it: the ring is sized once for the maximum
        // latency and this only moves the fill target inside it.
        model->SetLatencyMs(settings_->latency_ms());

        // Looping is set here and is deliberately *not* in engine_settings(), which is what keeps
        // this from being the recursion the GTK build hit.
        //
        // It is the one value the interface can also change directly, from the transport's own
        // toggle. Were it part of the engine struct as well, this function -- which runs as a change
        // handler -- would write a key back into the store, and that closes a loop:
        //
        //     changed "reverb" -> apply -> set looping -> store writes "looping"
        //         -> changed "looping" -> apply -> ...
        //
        // Under GSettings' dconf backend that loop died at the first turn, because writing a value
        // equal to the stored one emits no change, so it was invisible until a flatpak fell back to
        // the keyfile backend and every engine setting recursed until the interface stopped
        // answering. LocalSettings has no such mercy: it writes unconditionally, so the bug would be
        // *more* likely here, not less. The store's setters return early when the value is unchanged,
        // and looping stays out of the struct.
        model_.Looping(settings_->looping());
    }

    fire_and_forget MainWindow::OnPrefsClick(IInspectable const&, RoutedEventArgs const&)
    {
        auto lifetime = get_strong();
        co_await tsgui::ShowPreferencesAsync(*settings_, Content().XamlRoot());
    }

    void MainWindow::OnSongInfoClick(IInspectable const&, RoutedEventArgs const&)
    {
        // Brought forward rather than opened again. The window follows the player across one file
        // and the next, so a second click means "where did that go", not "give me another one".
        if (songInfoWindow_ != nullptr) {
            songInfoWindow_.Activate();
            return;
        }

        songInfoWindow_ = tsgui::CreateSongInfoWindow(model_, *settings_);

        // Cleared when it closes, so the next click builds a fresh one. Without this the handle
        // would outlive the window it names and Activate would be called on a closed window - which
        // does not throw, and so would look like the button having stopped working.
        songInfoWindow_.Closed([this](auto&&, auto&&) { songInfoWindow_ = nullptr; });

        songInfoWindow_.Activate();
    }

    // -- Window -------------------------------------------------------------------------------------

    // Both halves live in WindowChrome, because the song information window remembers its own size
    // the same way and the two traps they step around -- Resize not clamping, and a maximized
    // window's size not being the one to store -- should not be written down twice.
    void MainWindow::RestoreWindowGeometry()
    {
        tsgui::RestoreWindowGeometry(try_as<Window>(), settings_->window_width(),
                                     settings_->window_height(), settings_->window_maximized());
    }

    void MainWindow::SaveWindowGeometry()
    {
        const auto geometry = tsgui::MeasureWindowGeometry(
            try_as<Window>(), settings_->window_width(), settings_->window_height());
        settings_->set_window_geometry(geometry.width, geometry.height, geometry.maximized);
    }

    HWND MainWindow::Hwnd()
    {
        HWND hwnd{};
        check_hresult(this->try_as<::IWindowNative>()->get_WindowHandle(&hwnd));
        return hwnd;
    }

    void MainWindow::SyncHeading()
    {
        // What the title bar says when nothing else is being reported. The program's own name is the
        // fallback rather than the constant it used to be: with a song open, the taskbar button and
        // Alt-Tab already carry the name, and the strip's one line of heading is better spent on
        // which file this is.
        const hstring song = model_.SongName();
        TitleBarText().Text(song.empty() ? hstring{ L"Tabula Sonora Player" } : song);

        const hstring rom = model_.RomName();
        StatusText().Text(rom.empty()
                              ? hstring{ L"No ROM loaded." }
                              : to_hstring(std::format("Sound Canvas voice \xC2\xB7 {}",
                                                       to_string(rom))));
    }

    void MainWindow::OnModelPropertyChanged(IInspectable const&,
                                            Data::PropertyChangedEventArgs const& args)
    {
        const hstring name = args.PropertyName();

        if (name == L"SongName" || name == L"RomName") {
            SyncHeading();
        }

        if (name == L"Position") {
            ++updates_;
            ChurnText().Text(to_hstring(std::format(
                "rows {}   list changes {}   model updates {}",
                model_.VisibleParts().Size(), listChanges_, updates_)));
        } else if (name == L"Looping") {
            // Written back, because the transport's toggle is the other thing that can change it.
            // The store returns early when the value is unchanged, so this cannot bounce.
            settings_->set_looping(model_.Looping());
        } else if (name == L"OutputGain") {
            settings_->set_output_gain(model_.OutputGain());
        }
    }

    // -- The ROM ------------------------------------------------------------------------------------

    void MainWindow::ShowRomImport(bool show)
    {
        // Collapsed rather than merely disabled once a ROM is in place. Importing is something done
        // once, on the first run, and a permanent button for it sits in the row that opens songs --
        // where the thing anyone actually wants is one place to the right.
        //
        // It is not the only way in, which is what makes hiding it safe: dropping a .dll on the
        // window imports it, so replacing a ROM with a different build stays possible without a
        // control that spends the program's whole life pointing at a job already done.
        OpenRomButton().Visibility(show ? Visibility::Visible : Visibility::Collapsed);
    }

    fire_and_forget MainWindow::ImportRom(StorageFile file)
    {
        auto lifetime = get_strong();

        OpenRomButton().IsEnabled(false);
        StatusText().Text(L"Importing SCCore.dll...");

        auto local = ApplicationData::Current().LocalFolder();

        std::string error;
        hstring imported;

        try {
            // Staged, then renamed into place. The rename is atomic where the copy is not, so the
            // final name never names a partial file. This is the discipline the GTK build follows
            // with g_file_move and it exists for the same reason: the engine reads this file
            // positionally for the whole session, so it has to be somewhere stable and ours rather
            // than wherever the user happened to have it.
            auto staged = co_await file.CopyAsync(local, rom_staging_name,
                                                  NameCollisionOption::ReplaceExisting);
            co_await staged.RenameAsync(rom_name, NameCollisionOption::ReplaceExisting);
            imported = (co_await local.GetFileAsync(rom_name)).Path();
        } catch (const hresult_error& failure) {
            error = to_string(failure.message());
        }

        if (!error.empty()) {
            OpenRomButton().IsEnabled(true);
            StatusText().Text(to_hstring("Could not import the ROM: " + error));
            co_return;
        }

        StatusText().Text(L"Verifying...");

        // Verified in full on import, which is the one moment it is worth 27 MB of hashing: this is
        // the first time the program has seen this file. Later launches take the quick path.
        const bool loaded = co_await model_.LoadRomAsync(imported, true);

        OpenRomButton().IsEnabled(true);

        if (!loaded) {
            settings_->set_rom_verified(false);
            ShowRomImport(true);
            StatusText().Text(model_.LastError());
            co_return;
        }

        settings_->set_rom_verified(true);
        ShowRomImport(false);
        SyncHeading();
        OpenSongButton().IsEnabled(true);
        RecentButton().IsEnabled(true);

        // Whatever was named before there was a ROM to play it on. Both paths that load a ROM end
        // here, because a file can arrive before either of them finishes.
        OpenPendingSong();
    }

    fire_and_forget MainWindow::RestoreRom()
    {
        auto lifetime = get_strong();

        auto local = ApplicationData::Current().LocalFolder();

        StorageFile file{ nullptr };
        try {
            file = co_await local.GetFileAsync(rom_name);
        } catch (const hresult_error&) {
            // No ROM imported yet, which is the ordinary first-run state and not an error.
            co_return;
        }

        OpenRomButton().IsEnabled(false);
        StatusText().Text(L"Loading SCCore.dll...");

        // The full hash only if the last import never finished verifying. A file that has been
        // verified once and has not moved since is checked by size and header, which is what makes a
        // second launch quick.
        const bool loaded = co_await model_.LoadRomAsync(file.Path(), !settings_->rom_verified());

        OpenRomButton().IsEnabled(true);

        if (!loaded) {
            settings_->set_rom_verified(false);
            ShowRomImport(true);
            StatusText().Text(model_.LastError());
            co_return;
        }

        settings_->set_rom_verified(true);
        ShowRomImport(false);
        SyncHeading();
        OpenSongButton().IsEnabled(true);
        RecentButton().IsEnabled(true);

        // Whatever was named before there was a ROM to play it on. Both paths that load a ROM end
        // here, because a file can arrive before either of them finishes.
        OpenPendingSong();
    }

    fire_and_forget MainWindow::OnOpenRomClick(IInspectable const&, RoutedEventArgs const&)
    {
        auto lifetime = get_strong();

        FileOpenPicker picker;
        picker.as<::IInitializeWithWindow>()->Initialize(Hwnd());
        picker.SuggestedStartLocation(PickerLocationId::ComputerFolder);
        picker.FileTypeFilter().Append(L".dll");

        const StorageFile file = co_await picker.PickSingleFileAsync();
        if (file == nullptr) {
            co_return;
        }

        ImportRom(file);
    }

    // -- Songs --------------------------------------------------------------------------------------

    IAsyncOperation<bool> MainWindow::OpenSong(hstring path)
    {
        auto lifetime = get_strong();

        OpenSongButton().IsEnabled(false);
        StatusText().Text(L"Loading song...");

        // to_string, not a wide path. The whole host layer is std::string and is shared by copy with
        // the Apple and Linux front ends; app.manifest declares the process code page as UTF-8 so
        // these bytes reach CreateFileA and std::ifstream meaning what they say.
        const bool loaded = co_await model_.LoadSongAsync(path);

        OpenSongButton().IsEnabled(true);

        if (!loaded) {
            StatusText().Text(model_.LastError());
            co_return false;
        }

        SyncHeading();
        ExportButton().IsEnabled(true);
        SongInfoButton().IsEnabled(true);

        listChanges_ = 0;
        updates_ = 0;
        co_return true;
    }

    void MainWindow::OpenActivatedFile(hstring path)
    {
        if (path.empty()) {
            return;
        }

        // No ROM yet, so this waits. Two ways in reach this state and both are ordinary: a fresh
        // install where the ROM has never been imported, and every launch at all, because the ROM is
        // read from storage asynchronously and a file activation arrives while that is still in
        // flight. Refusing here would fail for reasons of timing rather than of fact.
        if (model_.RomName().empty()) {
            pendingSong_ = path;
            StatusText().Text(L"Waiting for SCCore.dll before playing that.");
            return;
        }

        OpenActivatedSong(path);
    }

    fire_and_forget MainWindow::OpenPendingSong()
    {
        auto lifetime = get_strong();

        // Taken and cleared before the first suspension, so a second activation landing during the
        // load is a new pending file rather than a second attempt at this one.
        const hstring path = std::exchange(pendingSong_, hstring{});
        if (path.empty()) {
            co_return;
        }

        OpenActivatedSong(path);
    }

    fire_and_forget MainWindow::OpenActivatedSong(hstring path)
    {
        auto lifetime = get_strong();

        if (co_await OpenSong(path)) {
            // Remembered like any other opening. A file reached through Explorer is one somebody
            // chose just as deliberately as one reached through the picker, and leaving it out of the
            // recent list would make that list quietly wrong about what had been played.
            try {
                RememberSong(co_await StorageFile::GetFileFromPathAsync(path));
                RebuildRecentMenu();
            } catch (const hresult_error&) {
                // The song opened but the file cannot be reached through the storage API, which
                // happens for paths the broker will not hand back. The song is playing; the recent
                // list is one entry short. Not worth telling anyone about.
            }
        }
    }

    fire_and_forget MainWindow::OnOpenSongClick(IInspectable const&, RoutedEventArgs const&)
    {
        auto lifetime = get_strong();

        FileOpenPicker picker;
        picker.as<::IInitializeWithWindow>()->Initialize(Hwnd());
        picker.SuggestedStartLocation(PickerLocationId::MusicLibrary);
        for (const auto* extension : tsgui::kSongExtensions) {
            picker.FileTypeFilter().Append(extension);
        }

        const StorageFile file = co_await picker.PickSingleFileAsync();
        if (file == nullptr) {
            co_return;
        }

        if (co_await OpenSong(file.Path())) {
            RememberSong(file);
            RebuildRecentMenu();
        }
    }

    void MainWindow::RememberSong(StorageFile const& file)
    {
        // The MRU list, not a list of paths in the settings. It holds a token that survives the file
        // being renamed or moved, and -- more to the point on Windows -- it is what grants access
        // back to a file the user chose once, without asking again.
        StorageApplicationPermissions::MostRecentlyUsedList().Add(file, file.Path());
    }

    void MainWindow::RebuildRecentMenu()
    {
        auto items = RecentFlyout().Items();
        items.Clear();

        auto mru = StorageApplicationPermissions::MostRecentlyUsedList();
        const auto entries = mru.Entries();

        uint32_t shown = 0;
        for (const auto& entry : entries) {
            if (shown >= 8) {
                break;
            }

            // The metadata is the path as it was when the file was added, which is what the menu
            // shows. The token is what actually opens it, and the two can disagree once a file has
            // moved -- the label is a label, not the thing being opened.
            const hstring token = entry.Token;
            const hstring label = entry.Metadata;
            if (label.empty()) {
                continue;
            }

            Controls::MenuFlyoutItem item;
            item.Text(label);
            item.Click([this, token](auto&&, auto&&) -> fire_and_forget {
                auto inner = get_strong();
                try {
                    auto file = co_await StorageApplicationPermissions::MostRecentlyUsedList()
                                    .GetFileAsync(token);
                    co_await OpenSong(file.Path());
                } catch (const hresult_error&) {
                    // Gone, moved, or on a drive that is not mounted today. Dropped from the list
                    // rather than left to fail again: the three cases are indistinguishable from
                    // here and the honest answer to all of them is the same.
                    StorageApplicationPermissions::MostRecentlyUsedList().Remove(token);
                    RebuildRecentMenu();
                    StatusText().Text(L"That file is no longer where it was.");
                }
            });
            items.Append(item);
            ++shown;
        }

        RecentButton().IsEnabled(shown > 0 && !model_.RomName().empty());
    }

    // -- Drag and drop ------------------------------------------------------------------------------

    void MainWindow::OnDragOver(IInspectable const&, DragEventArgs const& args)
    {
        // Answered synchronously, so the cursor is right while the pointer is still moving. Whether
        // the payload is actually a song is checked on drop; asking here would mean an await, and
        // DragOver cannot wait.
        args.AcceptedOperation(args.DataView().Contains(StandardDataFormats::StorageItems())
                                   ? DataPackageOperation::Copy
                                   : DataPackageOperation::None);
    }

    fire_and_forget MainWindow::OnDrop(IInspectable const&, DragEventArgs const& args)
    {
        auto lifetime = get_strong();

        if (!args.DataView().Contains(StandardDataFormats::StorageItems())) {
            co_return;
        }

        // Taken before the first await. The DataView is only valid for the duration of the handler,
        // and a deferral is what keeps it alive across one -- without this, the items are gone by the
        // time the file has been read.
        auto deferral = args.GetDeferral();
        const auto items = co_await args.DataView().GetStorageItemsAsync();
        deferral.Complete();

        for (const auto& item : items) {
            auto file = item.try_as<StorageFile>();
            if (file == nullptr) {
                continue;
            }

            if (tsgui::IsSongFile(std::wstring_view{ file.Name() })) {
                if (co_await OpenSong(file.Path())) {
                    RememberSong(file);
                    RebuildRecentMenu();
                }
                co_return;
            }

            // A dropped DLL is taken as a ROM import, which is the only other file this program has
            // any use for and saves a trip through the picker on first run.
            if (std::wstring_view{ file.Name() }.ends_with(L".dll")) {
                ImportRom(file);
                co_return;
            }
        }

        StatusText().Text(L"That is not a file this can play.");
    }

    // -- Export -------------------------------------------------------------------------------------

    fire_and_forget MainWindow::OnExportClick(IInspectable const&, RoutedEventArgs const&)
    {
        auto lifetime = get_strong();

        FileSavePicker picker;
        picker.as<::IInitializeWithWindow>()->Initialize(Hwnd());
        picker.SuggestedFileName(model_.SongName());
        picker.FileTypeChoices().Insert(L"WAV audio", single_threaded_vector<hstring>({ L".wav" }));

        const StorageFile file = co_await picker.PickSaveFileAsync();
        if (file == nullptr) {
            co_return;
        }

        ExportButton().IsEnabled(false);

        // The transport shows its own progress row off the model's Exporting property; nothing needs
        // to be handed to it. There is no cancellation to offer either -- see Session::run_export,
        // which renders in one uninterruptible call.
        const bool ok = co_await model_.ExportWavAsync(file.Path());

        ExportButton().IsEnabled(true);
        if (ok) {
            SyncHeading();
        } else {
            StatusText().Text(model_.LastError());
        }
    }
}
