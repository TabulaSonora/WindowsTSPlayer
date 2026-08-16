#include "pch.h"

#include "MainWindow.xaml.h"
#if __has_include("MainWindow.g.cpp")
#include "MainWindow.g.cpp"
#endif

#include "PlayerModel.h"
#include "TransportView.xaml.h"

// IWindowNative, which is how a WinUI 3 desktop window surrenders its HWND. There is no projected
// way to ask: the window is a XAML object and the HWND is an implementation detail everywhere except
// at the two places Win32 still demands one, the pickers below being the first.
#include <microsoft.ui.xaml.window.h>

// IInitializeWithWindow. A picker shown from a desktop app has no CoreWindow to parent itself to,
// and without this it throws rather than falling back to anything.
#include <shobjidl_core.h>

#include <winrt/Microsoft.UI.Windowing.h>

#include <format>
#include <string>

using namespace winrt;
using namespace Microsoft::UI::Xaml;
using namespace Windows::Storage;
using namespace Windows::Storage::Pickers;

namespace {

/// Where the ROM chosen last time is remembered.
///
/// Hyphenated, matching the GTK build's GSettings keys verbatim so the three ports' documentation
/// can describe one set of names. This is the interim home: the settings milestone replaces it with
/// a SettingsStore that owns every key, and with the import discipline the plan describes, where the
/// file is copied into the app's own LocalFolder rather than merely pointed at.
constexpr wchar_t rom_path_key[] = L"rom-path";

} // namespace

namespace winrt::WindowsTSPlayer::implementation
{
    MainWindow::MainWindow()
        : model_(WindowsTSPlayer::PlayerModel())
    {
        Title(L"Tabula Sonora Player");

        // Explicitly, and before anything reaches for a named element. Nothing calls this on a
        // Window's behalf ahead of the constructor body, so Transport() below is null without it and
        // the process dies with an access violation before the window is ever shown. It is
        // idempotent -- the generated body is guarded by _contentLoaded -- so calling it here costs
        // nothing if the framework has already been through it.
        //
        // Earlier milestones got away with omitting it only because the constructor touched no named
        // element at all.
        InitializeComponent();
        SetWindowIcon();

        Transport().Model(model_);

        model_.PropertyChanged({ this, &MainWindow::OnModelPropertyChanged });

        // Counted rather than trusted. VectorChanged fires on every insert, remove and move of the
        // visible list, so a run where this settles while the update counter keeps climbing is the
        // mixer milestone's whole claim, demonstrated rather than asserted.
        model_.VisibleParts().VectorChanged([this](auto&&, auto&&) { ++listChanges_; });

        Closed([this](IInspectable const&, WindowEventArgs const&) {
            // Dropping the last reference here is what stops the tick and the device: the model's
            // destructor owns that teardown, and leaving it to process exit would let the render
            // thread and the WASAPI feeder outlive the XAML tree they are publishing into.
            Transport().Model(nullptr);
            model_ = nullptr;
        });

        RestoreRom();
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

    void MainWindow::RememberRom(hstring const& path)
    {
        ApplicationData::Current().LocalSettings().Values().Insert(rom_path_key, box_value(path));
    }

    fire_and_forget MainWindow::RestoreRom()
    {
        auto lifetime = get_strong();

        auto values = ApplicationData::Current().LocalSettings().Values();
        const auto stored = values.TryLookup(rom_path_key);
        if (stored == nullptr) {
            co_return;
        }

        const hstring path = unbox_value_or<hstring>(stored, hstring{});
        if (path.empty()) {
            co_return;
        }

        OpenRomButton().IsEnabled(false);
        StatusText().Text(L"Loading remembered ROM...");

        // The quick check, not the full 27 MB hash. This file was verified when it was chosen; what
        // is being confirmed here is that it is still the same file, which size and header answer.
        const bool loaded = co_await model_.LoadRomAsync(path, false);

        OpenRomButton().IsEnabled(true);

        if (!loaded) {
            // Forgotten rather than left to fail again on every launch. A ROM on a drive that is not
            // mounted today is the ordinary case, and it is indistinguishable from one that has been
            // deleted; either way the honest answer is to ask again.
            values.Remove(rom_path_key);
            StatusText().Text(L"The remembered SCCore.dll is gone. Choose it again.");
            co_return;
        }

        StatusText().Text(model_.RomName());
        OpenSongButton().IsEnabled(true);
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
        // The transport syncs itself from the same event; this window only keeps the development
        // readout and the file buttons.
        if (args.PropertyName() == L"Position") {
            ++updates_;
            ChurnText().Text(to_hstring(std::format(
                "rows {}   list changes {}   model updates {}",
                model_.VisibleParts().Size(), listChanges_, updates_)));
        }
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

        OpenRomButton().IsEnabled(false);
        StatusText().Text(L"Loading ROM...");

        // to_string, not a wide path. The whole host layer is std::string and is shared by copy with
        // the Apple and Linux front ends; app.manifest declares the process code page as UTF-8 so
        // these bytes reach CreateFileA and std::ifstream meaning what they say.
        const bool loaded = co_await model_.LoadRomAsync(file.Path(), false);

        OpenRomButton().IsEnabled(true);
        if (!loaded) {
            StatusText().Text(model_.LastError());
            co_return;
        }

        RememberRom(file.Path());
        StatusText().Text(model_.RomName());
        OpenSongButton().IsEnabled(true);
    }

    fire_and_forget MainWindow::OnOpenSongClick(IInspectable const&, RoutedEventArgs const&)
    {
        auto lifetime = get_strong();

        FileOpenPicker picker;
        picker.as<::IInitializeWithWindow>()->Initialize(Hwnd());
        picker.SuggestedStartLocation(PickerLocationId::MusicLibrary);

        // The twelve extensions the Linux front end accepts, verbatim. LDS is the one worth noting:
        // it has no magic number and is recognised by its extension alone, which is what made the
        // two-separator fix in Session::file_name load-bearing rather than cosmetic.
        for (const auto* extension : { L".mid", L".midi", L".rmi", L".mids", L".mus", L".xmi",
                                       L".xmf", L".mxmf", L".gmf", L".hmi", L".hmp", L".lds" }) {
            picker.FileTypeFilter().Append(extension);
        }

        const StorageFile file = co_await picker.PickSingleFileAsync();
        if (file == nullptr) {
            co_return;
        }

        OpenSongButton().IsEnabled(false);
        StatusText().Text(L"Loading song...");

        const bool loaded = co_await model_.LoadSongAsync(file.Path());

        OpenSongButton().IsEnabled(true);
        if (!loaded) {
            StatusText().Text(model_.LastError());
            co_return;
        }

        StatusText().Text(model_.RomName());
        ExportButton().IsEnabled(true);

        // Reset here rather than at construction: the interesting count is per song, and a file
        // loaded after another one has been playing would otherwise start from a number that says
        // nothing.
        listChanges_ = 0;
        updates_ = 0;
    }

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

        // The transport shows its own progress row off the model's Exporting property; nothing
        // needs to be handed to it. There is no cancellation to offer either -- see
        // Session::run_export, which renders in one uninterruptible call.
        const bool ok = co_await model_.ExportWavAsync(file.Path());

        ExportButton().IsEnabled(true);
        StatusText().Text(ok ? model_.RomName() : model_.LastError());
    }
}
