#include "pch.h"

#include "App.xaml.h"
#include "Activation.h"
#include "MainWindow.xaml.h"

// No "App.g.cpp" include here, unlike MainWindow.xaml.cpp. App is not in Windows metadata at all
// (see App.idl), so cppwinrt generates no factory for it and there is nothing to pull in.

#include <utility>

using namespace winrt;
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::Windows::AppLifecycle;

namespace winrt::WindowsTSPlayer::implementation
{
    App::App()
    {
#if defined(_DEBUG) && !defined(DISABLE_XAML_GENERATED_BREAK_ON_UNHANDLED_EXCEPTION)
        // A XAML exception that reaches here has already lost its C++ stack, so the debugger break
        // is the only place the message is still legible. Without it the process exits with an
        // unhelpful code and no indication of which binding or handler threw.
        UnhandledException([](IInspectable const&, UnhandledExceptionEventArgs const& e) {
            if (IsDebuggerPresent()) {
                auto message = e.Message();
                __debugbreak();
            }
        });
#endif
    }

    void App::OnLaunched(LaunchActivatedEventArgs const&)
    {
        // The argument is deliberately ignored. It reports "Launch" unconditionally, even when the
        // process was started by a file association, which is the documented trap; the real
        // activation comes from AppInstance::GetCurrent().GetActivatedEventArgs() below.
        window_ = make<MainWindow>();
        window_.Activate();

        // Guarded for the same reason wWinMain is: without package identity every one of these
        // throws, and an unhandled throw out of OnLaunched takes the window down with it. A build
        // started from its own output directory has no identity and is still worth being able to run.
        try {
            // Redirected activations, which arrive while this process is already running because
            // someone opened a second file. Registered after the window exists, so a redirection
            // landing immediately has somewhere to go.
            activated_ = AppInstance::GetCurrent().Activated(
                auto_revoke, [this](auto&&, AppActivationArguments const& args) {
                    OpenActivated(tsgui::ActivatedSongPath(args));
                });

            OpenActivated(tsgui::ActivatedSongPath());
        } catch (const hresult_error&) {
            // No identity, so no activation to read and nothing to redirect. The command line is
            // still there and still means what it says, so an unpackaged run opens the file it was
            // given rather than silently ignoring it.
            OpenActivated(tsgui::CommandLineSongPath());
        }
    }

    void App::OpenActivated(std::wstring path)
    {
        if (path.empty() || window_ == nullptr) {
            return;
        }

        // Every activation event arrives on a background thread -- this is the counterpart of D-Bus
        // method dispatch in the GTK build, and touching the tree from here is the classic first bug.
        // TryEnqueue rather than a check for the right thread: the launch-time call is already on the
        // UI thread and would run inline, but posting it uniformly means the window is fully
        // constructed and activated before anything asks it to open a file.
        auto window = window_;
        window.DispatcherQueue().TryEnqueue([window, path = std::move(path)]() {
            auto self = get_self<MainWindow>(window);

            // Forward first, because a program that opens a file behind whatever the reader is
            // looking at has done half a job. This is the gtk_window_present the GTK build makes on
            // the same path.
            window.Activate();
            self->OpenActivatedFile(hstring{ path });
        });
    }
}

/// The entry point, ours rather than the generated one.
///
/// DISABLE_XAML_GENERATED_MAIN in the project frees the name, and this then does the whole job the
/// generated body did -- the apartment, Application::Start, constructing App -- rather than doing the
/// redirect and handing off to the generated helper.
///
/// **Handing off was tried first and does not work.** The generated helper calls init_apartment
/// itself, so the thread is initialised twice; and, more decisively, under
/// DISABLE_XAML_GENERATED_MAIN it only constructs App when a
/// `decltype(App())` SFINAE probe says it can, which MSVC answers *false* for -- a C++/WinRT
/// implementation type deliberately has a non-public destructor, and the probe is meant to see past
/// that. So Application::Start ran with no Application ever created and the process died about a
/// second in, inside Microsoft.UI.Xaml.dll, with a stowed exception and no window. Constructing App
/// unconditionally here is three lines and depends on nothing.
///
/// **The redirection has to happen here and cannot move into App.** By the time OnLaunched runs there
/// is a window, an engine and a claim on the audio device, and discovering then that another instance
/// owns all three is too late to do anything graceful about it.
int __stdcall wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    // Exactly once, and before the redirect, because the AppInstance calls it makes are WinRT and
    // need an apartment.
    winrt::init_apartment(winrt::apartment_type::single_threaded);

    // Caught, and starting anyway is the right answer rather than the lazy one.
    //
    // Every call inside needs package identity, and a build run straight out of its own output
    // directory has none -- which is exactly how this gets started under a debugger. Uncaught, the
    // throw unwinds out of a function nothing is guarding and the process aborts: a Visual C++
    // Runtime dialog saying "abort() has been called", no window, and nothing naming the cause.
    //
    // What is lost by carrying on is single-instancing, and what is gained is a program that runs.
    // That is the right way round: two windows is a nuisance, and no window at all with a dialog box
    // for a diagnostic is not a program.
    try {
        if (tsgui::RedirectToRunningInstance()) {
            return 0;
        }
    } catch (const winrt::hresult_error&) {
    }

    Application::Start([](auto&&) { make<winrt::WindowsTSPlayer::implementation::App>(); });
    return 0;
}
