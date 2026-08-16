#pragma once

#include <winrt/Microsoft.UI.Xaml.Controls.h>

namespace tsgui
{
    /// The chrome both of this program's windows wear: the Mica Alt backdrop, content extended into
    /// the title bar, and the caption buttons kept clear of whatever is drawn up there.
    ///
    /// Here rather than repeated in each window because the two have to agree. A second window with a
    /// system-drawn caption beside a first with a drawn one does not read as the same program, and the
    /// caption-inset arithmetic below is the sort of thing that gets fixed in one copy and not the
    /// other.
    ///
    /// `titleBarArea` is the element the window draws its own title into. It is handed to
    /// `SetTitleBar`, so it becomes the region that drags the window, and it is given left and right
    /// padding matching the system's own insets.
    void SetUpWindowChrome(winrt::Microsoft::UI::Xaml::Window const& window,
                           winrt::Microsoft::UI::Xaml::Controls::Grid const& titleBarArea);

    /// A window's size, and whether it is showing that size or filling the screen.
    struct WindowGeometry
    {
        int width;
        int height;
        bool maximized;
    };

    /// Restores a window to a stored size, clamped to the display it will appear on.
    ///
    /// The clamp is the reason this exists rather than a bare Resize. AppWindow::Resize will not do
    /// it, and a stored height taller than today's screen puts the bottom of the window past the
    /// bottom of the display, where it cannot be reached or dragged back. Both of this program's
    /// windows restore sizes measured on some other machine's monitor, so both need it.
    void RestoreWindowGeometry(winrt::Microsoft::UI::Xaml::Window const& window,
                               int width, int height, bool maximized);

    /// What is worth storing for a window as it closes.
    ///
    /// `storedWidth` and `storedHeight` are what is already on record, and they are returned unchanged
    /// when the window is maximized. That is the other half of the same trap: a maximized window's
    /// size is the screen's, and writing that back as the restored-down size leaves the next launch
    /// filling the display while its title bar says it is not maximized -- with no size left anywhere
    /// to restore *to*.
    [[nodiscard]] WindowGeometry MeasureWindowGeometry(
        winrt::Microsoft::UI::Xaml::Window const& window, int storedWidth, int storedHeight);

    /// Points a window at the packaged icon.
    ///
    /// The taskbar and Start entries come from the manifest's logos, but the title bar, the Alt-Tab
    /// card and the system menu read the HWND's icon, and WinUI 3 sets none from the package -- so
    /// without this the icon is right everywhere except on the window itself.
    void SetWindowIcon(winrt::Microsoft::UI::Xaml::Window const& window);
}
