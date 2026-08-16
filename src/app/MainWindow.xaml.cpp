#include "pch.h"

#include "MainWindow.xaml.h"
#if __has_include("MainWindow.g.cpp")
#include "MainWindow.g.cpp"
#endif

#include "PlayerModel.h"

// IWindowNative, which is how a WinUI 3 desktop window surrenders its HWND. There is no projected
// way to ask: the window is a XAML object and the HWND is an implementation detail everywhere except
// at the two places Win32 still demands one, the pickers below being the first.
#include <microsoft.ui.xaml.window.h>

// IInitializeWithWindow. A picker shown from a desktop app has no CoreWindow to parent itself to,
// and without this it throws rather than falling back to anything.
#include <shobjidl_core.h>

#include <format>
#include <string>

using namespace winrt;
using namespace Microsoft::UI::Xaml;
using namespace Windows::Storage;
using namespace Windows::Storage::Pickers;

namespace {

/// Seconds as m:ss. The transport gets a real one later.
std::string FormatTime(double seconds)
{
    const auto total = static_cast<long long>(seconds < 0.0 ? 0.0 : seconds);
    return std::format("{}:{:02}", total / 60, total % 60);
}

} // namespace

namespace winrt::WindowsTSPlayer::implementation
{
    MainWindow::MainWindow()
        : model_(WindowsTSPlayer::PlayerModel())
    {
        Title(L"Tabula Sonora Player");

        model_.PropertyChanged({ this, &MainWindow::OnModelPropertyChanged });

        // Counted rather than trusted. VectorChanged fires on every insert, remove and move of the
        // visible list, so a run where this settles while the update counter keeps climbing is the
        // milestone's whole claim, demonstrated rather than asserted.
        model_.VisibleParts().VectorChanged([this](auto&&, auto&&) { ++listChanges_; });

        Closed([this](IInspectable const&, WindowEventArgs const&) {
            // Dropping the last reference here is what stops the tick and the device: the model's
            // destructor owns that teardown, and leaving it to process exit would let the render
            // thread and the WASAPI feeder outlive the XAML tree they are publishing into.
            model_ = nullptr;
        });
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

        if (name == L"Playing") {
            PlayPauseButton().Content(box_value(model_.Playing() ? L"Pause" : L"Play"));
        } else if (name == L"SongName" || name == L"RomName") {
            UpdateReadouts();
        }

        // Position advances on every tick while a song plays, so counting its notifications counts
        // ticks for as long as the readout is worth reading. It deliberately stops while paused
        // rather than being wired to a second timer: the claim being demonstrated is about a
        // *playing* song, and a counter that climbed with nothing happening would prove less.
        if (name == L"Position") {
            ++updates_;
            UpdateReadouts();
        } else if (name == L"ActiveVoices" || name == L"Underruns") {
            UpdateReadouts();
        }
    }

    void MainWindow::UpdateReadouts()
    {
        if (model_ == nullptr) {
            return;
        }

        StatusText().Text(to_hstring(std::format(
            "ROM: {}    Song: {}    {}",
            to_string(model_.RomName()).empty() ? "none" : to_string(model_.RomName()),
            to_string(model_.SongName()).empty() ? "none" : to_string(model_.SongName()),
            to_string(model_.BackendDescription()))));

        EngineText().Text(to_hstring(std::format(
            "voices {:3}/{}   {} / {}   {}   underruns {}",
            model_.ActiveVoices(), model_.VoiceCapacity(),
            FormatTime(model_.Position()), FormatTime(model_.Duration()),
            model_.XgMode() ? "XG" : "GS",
            model_.Underruns())));

        ChurnText().Text(to_hstring(std::format(
            "rows {}   list changes {}   model updates {}",
            model_.VisibleParts().Size(), listChanges_, updates_)));
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

        OpenSongButton().IsEnabled(true);
        UpdateReadouts();
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

        PlayPauseButton().IsEnabled(true);
        RestartButton().IsEnabled(true);
        PanicButton().IsEnabled(true);
        LoopButton().IsEnabled(true);

        // Reset here rather than at construction: the interesting count is per song, and a file
        // loaded after another one has been playing would otherwise start from a number that says
        // nothing.
        listChanges_ = 0;
        updates_ = 0;

        UpdateReadouts();
    }

    void MainWindow::OnPlayPauseClick(IInspectable const&, RoutedEventArgs const&)
    {
        model_.TogglePlaying();
    }

    void MainWindow::OnRestartClick(IInspectable const&, RoutedEventArgs const&)
    {
        model_.Restart();
    }

    void MainWindow::OnPanicClick(IInspectable const&, RoutedEventArgs const&)
    {
        model_.Panic();
    }

    void MainWindow::OnLoopClick(IInspectable const& sender, RoutedEventArgs const&)
    {
        model_.Looping(sender.as<Controls::Primitives::ToggleButton>().IsChecked().Value());
    }
}
