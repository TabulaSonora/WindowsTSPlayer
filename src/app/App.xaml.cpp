#include "pch.h"

#include "App.xaml.h"
#include "MainWindow.xaml.h"

// No "App.g.cpp" include here, unlike MainWindow.xaml.cpp. App is not in Windows metadata at all
// (see App.idl), so cppwinrt generates no factory for it and there is nothing to pull in.

using namespace winrt;
using namespace Microsoft::UI::Xaml;

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
        // activation arrives from AppInstance::GetCurrent().GetActivatedEventArgs(). Nothing here
        // needs it yet -- file activation is a later milestone -- but taking it from the wrong
        // place now would be a bug that survived into that milestone.
        window_ = make<MainWindow>();
        window_.Activate();
    }
}
