#include "pch.h"

#include "WindowChrome.h"

#include <algorithm>
#include <string>

using namespace winrt;
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::UI::Xaml::Controls;

namespace {

/// Reserves room at each end of the title bar for the buttons the system draws over it.
///
/// The caption buttons are not a fixed width -- they differ with language, with the presence of a
/// maximise button, and on a machine showing the Widgets or Snap affordances -- so the clearance is
/// read back from the window rather than guessed at.
void Reserve(Microsoft::UI::Windowing::AppWindow const& window, Grid const& area)
{
    const auto titleBar = window.TitleBar();

    // Divided by the rasterization scale because the insets are in physical pixels and Padding is in
    // effective ones. On a 150% display the undivided value reserves half again too much, which shows
    // up as the title text sitting oddly far from the left edge.
    const double scale =
        area.XamlRoot() != nullptr ? area.XamlRoot().RasterizationScale() : 1.0;

    area.Padding(ThicknessHelper::FromLengths(titleBar.LeftInset() / scale, 0,
                                              titleBar.RightInset() / scale, 0));
}

} // namespace

namespace tsgui
{
    void SetUpWindowChrome(Window const& window, Grid const& strip, UIElement const& dragRegion,
                           bool tall)
    {
        // Mica Alt as the base, with content raised onto translucent layers above it.
        //
        // The intent is two materials: Mica Alt behind the chrome, and the lighter Mica behind the
        // content. That cannot be had literally. A backdrop is a property of the *window* - one
        // SystemBackdrop, composited by the system behind everything - and UIElement does not
        // implement ICompositionSupportsSystemBackdrop, so there is no way to give a region a
        // material of its own.
        //
        // What produces the same reading is the layering Fluent is built around, and Mica Alt is
        // specifically the kind meant to sit underneath it: the deeper, more strongly tinted base,
        // with content on translucent layers over it. So the window takes BaseAlt, the chrome shows
        // it bare, and each window's content sits on a layer brush that lifts it back towards the
        // weight of plain Mica. The hierarchy is the one intended; only the mechanism differs.
        //
        // Asked for rather than assumed. Mica needs Windows 11 and composition support, and on a
        // machine without either, MicaBackdrop paints nothing at all: the window would come up with a
        // transparent body over the desktop, because the roots deliberately carry no background of
        // their own. Leaving SystemBackdrop unset in that case gets the solid theme colour, and the
        // layer brushes still read correctly over it.
        if (Microsoft::UI::Composition::SystemBackdrops::MicaController::IsSupported()) {
            Media::MicaBackdrop backdrop;
            backdrop.Kind(Microsoft::UI::Composition::SystemBackdrops::MicaKind::BaseAlt);
            window.SystemBackdrop(backdrop);
        }

        // The title bar has to come with it. A system-drawn caption is painted its own opaque colour,
        // so the material would stop at a hard edge an inch below the top of the window - which reads
        // as the backdrop being broken rather than as a caption doing its job.
        window.ExtendsContentIntoTitleBar(true);
        window.SetTitleBar(dragRegion);

        auto appWindow = window.AppWindow();

        if (tall) {
            appWindow.TitleBar().PreferredHeightOption(
                Microsoft::UI::Windowing::TitleBarHeightOption::Tall);
        }

        // AppWindow::Changed, not a title-bar event: AppWindowTitleBar has none. The UWP type had
        // LayoutMetricsChanged and the Windowing one does not, so the insets are re-read whenever the
        // window itself changes, which covers the resize and the move between displays of different
        // scale that would alter them.
        //
        // The handler holds the element weakly and takes the AppWindow from its own sender. Capturing
        // either strongly would hang a reference off the AppWindow that points back at the tree the
        // AppWindow belongs to, and a window that cannot drop its last reference is a window that
        // never finishes closing.
        auto weakArea = make_weak(strip);
        appWindow.Changed([weakArea](Microsoft::UI::Windowing::AppWindow const& sender, auto&&) {
            if (auto area = weakArea.get()) {
                Reserve(sender, area);
            }
        });

        Reserve(appWindow, strip);
    }

    void RestoreWindowGeometry(Window const& window, int width, int height, bool maximized)
    {
        auto appWindow = window.AppWindow();

        const auto work = Microsoft::UI::Windowing::DisplayArea::GetFromWindowId(
                              appWindow.Id(), Microsoft::UI::Windowing::DisplayAreaFallback::Primary)
                              .WorkArea();

        appWindow.Resize({ (std::min)(static_cast<int32_t>(width), work.Width),
                           (std::min)(static_cast<int32_t>(height), work.Height) });

        if (maximized) {
            if (auto presenter =
                    appWindow.Presenter().try_as<Microsoft::UI::Windowing::OverlappedPresenter>()) {
                presenter.Maximize();
            }
        }
    }

    WindowGeometry MeasureWindowGeometry(Window const& window, int storedWidth, int storedHeight)
    {
        auto appWindow = window.AppWindow();
        auto presenter = appWindow.Presenter().try_as<Microsoft::UI::Windowing::OverlappedPresenter>();
        const bool maximized =
            presenter != nullptr
            && presenter.State() == Microsoft::UI::Windowing::OverlappedPresenterState::Maximized;

        if (maximized) {
            return { storedWidth, storedHeight, true };
        }

        const auto size = appWindow.Size();
        return { size.Width, size.Height, false };
    }

    void SetWindowIcon(Window const& window)
    {
        // Resolved against the executable's own directory rather than passed as a relative path.
        // SetIcon resolves a relative path against the process working directory, which is whatever
        // the shell felt like when it launched us and is not the install folder. The symptom of
        // getting this wrong is not an error: the icon simply does not appear.
        //
        // Grown until it fits rather than assuming MAX_PATH. GetModuleFileNameW reports truncation by
        // filling the buffer exactly, so one fixed-size call cannot tell success from a silently cut
        // path.
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

        window.AppWindow().SetIcon(module.substr(0, slash + 1) + L"Assets\\AppIcon.ico");
    }
}
