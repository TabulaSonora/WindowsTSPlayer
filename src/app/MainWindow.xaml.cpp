#include "pch.h"

#include "MainWindow.xaml.h"
#if __has_include("MainWindow.g.cpp")
#include "MainWindow.g.cpp"
#endif

#include "MixerView.xaml.h"
#include "PlayerModel.h"
#include "PrefsDialog.h"
#include "TransportView.xaml.h"

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

/// The twelve extensions the Linux front end accepts, verbatim. LDS is the one worth noting: it has
/// no magic number and is recognised by its extension alone, which is what made the two-separator
/// fix in Session::file_name load-bearing rather than cosmetic.
constexpr const wchar_t* song_extensions[] = { L".mid",  L".midi", L".rmi", L".mids",
                                               L".mus",  L".xmi",  L".xmf", L".mxmf",
                                               L".gmf",  L".hmi",  L".hmp", L".lds" };

bool IsSong(std::wstring_view name)
{
    const auto dot = name.find_last_of(L'.');
    if (dot == std::wstring_view::npos) {
        return false;
    }
    std::wstring extension{ name.substr(dot) };
    std::transform(extension.begin(), extension.end(), extension.begin(), ::towlower);
    for (const auto* candidate : song_extensions) {
        if (extension == candidate) {
            return true;
        }
    }
    return false;
}

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
        SetWindowIcon();
        SetUpBackdrop();
        RestoreWindowGeometry();

        Transport().Model(model_);
        Mixer().Model(model_);

        model_.PropertyChanged({ this, &MainWindow::OnModelPropertyChanged });
        model_.VisibleParts().VectorChanged([this](auto&&, auto&&) { ++listChanges_; });

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

            // Dropping the last reference stops the tick and the device: the model's destructor owns
            // that teardown, and leaving it to process exit would let the render thread and the
            // WASAPI feeder outlive the XAML tree they are publishing into.
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

    // -- Window -------------------------------------------------------------------------------------

    void MainWindow::RestoreWindowGeometry()
    {
        auto window = AppWindow();

        // Clamped against the work area, which AppWindow::Resize will not do for us. GTK clamped a
        // default size to the monitor and that is where the stored 830px height came from; restoring
        // it unchecked onto a shorter screen would put the transport off the bottom edge.
        const auto display = Microsoft::UI::Windowing::DisplayArea::GetFromWindowId(
            window.Id(), Microsoft::UI::Windowing::DisplayAreaFallback::Primary);
        const auto work = display.WorkArea();

        const int32_t width = std::min<int32_t>(settings_->window_width(), work.Width);
        const int32_t height = std::min<int32_t>(settings_->window_height(), work.Height);
        window.Resize({ width, height });

        if (settings_->window_maximized()) {
            if (auto presenter = window.Presenter().try_as<Microsoft::UI::Windowing::OverlappedPresenter>()) {
                presenter.Maximize();
            }
        }
    }

    void MainWindow::SaveWindowGeometry()
    {
        auto window = AppWindow();
        auto presenter = window.Presenter().try_as<Microsoft::UI::Windowing::OverlappedPresenter>();
        const bool maximized =
            presenter != nullptr
            && presenter.State() == Microsoft::UI::Windowing::OverlappedPresenterState::Maximized;

        // The size is only worth storing when the window is showing it. A maximized window's size is
        // the screen's, and restoring that as a restored-down size would leave it filling the display
        // with a title bar that says it is not maximized.
        if (maximized) {
            settings_->set_window_geometry(settings_->window_width(), settings_->window_height(), true);
            return;
        }

        const auto size = window.Size();
        settings_->set_window_geometry(size.Width, size.Height, false);
    }

    HWND MainWindow::Hwnd()
    {
        HWND hwnd{};
        check_hresult(this->try_as<::IWindowNative>()->get_WindowHandle(&hwnd));
        return hwnd;
    }

    void MainWindow::OnModelPropertyChanged(IInspectable const&,
                                            Data::PropertyChangedEventArgs const& args)
    {
        const hstring name = args.PropertyName();

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
        StatusText().Text(model_.RomName());
        OpenSongButton().IsEnabled(true);
        RecentButton().IsEnabled(true);
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
        StatusText().Text(model_.RomName());
        OpenSongButton().IsEnabled(true);
        RecentButton().IsEnabled(true);
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

        StatusText().Text(model_.RomName());
        ExportButton().IsEnabled(true);

        listChanges_ = 0;
        updates_ = 0;
        co_return true;
    }

    fire_and_forget MainWindow::OnOpenSongClick(IInspectable const&, RoutedEventArgs const&)
    {
        auto lifetime = get_strong();

        FileOpenPicker picker;
        picker.as<::IInitializeWithWindow>()->Initialize(Hwnd());
        picker.SuggestedStartLocation(PickerLocationId::MusicLibrary);
        for (const auto* extension : song_extensions) {
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

    fire_and_forget MainWindow::RebuildRecentMenu()
    {
        auto lifetime = get_strong();

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

            if (IsSong(std::wstring_view{ file.Name() })) {
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
        StatusText().Text(ok ? model_.RomName() : model_.LastError());
    }

    // -- Chrome -------------------------------------------------------------------------------------

    void MainWindow::SetUpBackdrop()
    {
        // Mica Alt as the window's base, with the mixer lifted onto a layer above it.
        //
        // The intent is two materials: Mica Alt behind the title bar, the file controls and the
        // transport, and the lighter Mica behind the mixer. That cannot be had literally. A backdrop
        // is a property of the *window* - one SystemBackdrop, composited by the system behind
        // everything - and UIElement does not implement ICompositionSupportsSystemBackdrop, so there
        // is no way to give a region a material of its own.
        //
        // What produces the same reading is the layering Fluent is built around, and Mica Alt is
        // specifically the kind meant to sit underneath it: the deeper, more strongly tinted base,
        // with content raised onto translucent layers. So the window takes BaseAlt, the chrome shows
        // it bare, and the mixer sits on a layer brush that lifts it back towards the weight of plain
        // Mica. The hierarchy is the one intended; only the mechanism differs.
        //
        // Asked for rather than assumed. Mica needs Windows 11 and composition support, and on a
        // machine without either, MicaBackdrop paints nothing at all: the window would come up with
        // a transparent body over the desktop, because the root Grid deliberately has no background
        // of its own. Leaving SystemBackdrop unset in that case gets the solid theme colour, and the
        // mixer's layer brush still reads correctly over it.
        if (Microsoft::UI::Composition::SystemBackdrops::MicaController::IsSupported()) {
            Media::MicaBackdrop backdrop;
            backdrop.Kind(Microsoft::UI::Composition::SystemBackdrops::MicaKind::BaseAlt);
            SystemBackdrop(backdrop);
        }

        // The title bar has to come with it. A system-drawn caption is painted its own opaque colour,
        // so the material would stop at a hard edge below the top of the window.
        ExtendsContentIntoTitleBar(true);
        SetTitleBar(TitleBarArea());

        // The caption buttons are not a fixed width -- they differ with language, with the presence
        // of a maximise button, and on a machine that shows the Widgets or Snap affordances -- so the
        // clearance is read back from the window rather than guessed at. Re-read on every change,
        // because moving the window between displays of different scale changes it.
        const auto reserve = [this]() {
            const auto titleBar = AppWindow().TitleBar();
            const double scale = TitleBarArea().XamlRoot() != nullptr
                                     ? TitleBarArea().XamlRoot().RasterizationScale()
                                     : 1.0;
            TitleBarArea().Padding(
                ThicknessHelper::FromLengths(titleBar.LeftInset() / scale, 0,
                                             titleBar.RightInset() / scale, 0));
        };

        // AppWindow::Changed, not a title-bar event: AppWindowTitleBar has none. The UWP type had
        // LayoutMetricsChanged and the Windowing one does not, so the insets are re-read whenever the
        // window itself changes, which covers the resize and the move between displays that would
        // alter them.
        AppWindow().Changed([reserve](auto&&, auto&&) { reserve(); });
        reserve();
    }

    void MainWindow::SetWindowIcon()
    {
        // The taskbar and Start entries come from the manifest's logos, but the title bar, the
        // Alt-Tab card and the system menu read the HWND's icon, and WinUI 3 sets none from the
        // package -- so the icon is right everywhere except the window itself until this runs.
        //
        // Resolved against the executable's own directory rather than passed as a relative path.
        // SetIcon resolves a relative path against the process working directory, which is whatever
        // the shell felt like when it launched us and is not the install folder.
        //
        // Grown until it fits rather than assuming MAX_PATH. GetModuleFileNameW reports truncation
        // by filling the buffer exactly, so one fixed-size call cannot tell success from a silently
        // cut path.
        std::wstring module;
        DWORD length = 0;
        for (std::size_t size = MAX_PATH; size <= 32768; size *= 2) {
            module.resize(size);
            length = GetModuleFileNameW(nullptr, module.data(), static_cast<DWORD>(module.size()));
            if (length == 0) {
                return;
            }
            if (length < module.size()) {
                break;
            }
        }
        module.resize(length);

        const auto slash = module.find_last_of(L"/\\");
        if (slash == std::wstring::npos) {
            return;
        }

        const std::wstring icon = module.substr(0, slash + 1) + L"Assets\\AppIcon.ico";
        AppWindow().SetIcon(icon);
    }
}
