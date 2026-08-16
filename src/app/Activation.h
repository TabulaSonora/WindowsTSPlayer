#pragma once

#include <winrt/Microsoft.Windows.AppLifecycle.h>

#include <string>

namespace tsgui
{
    /// How this process was started, and where a file it was started with is.
    ///
    /// The whole of this file exists because a packaged application can be launched twice for
    /// reasons that have nothing to do with the person using it: double-clicking a second file in
    /// Explorer starts a second process, and without the redirection below that second process would
    /// come up with its own window, its own engine and its own claim on the audio device. The GTK
    /// build gets the same behaviour from GApplication uniqueness plus G_APPLICATION_HANDLES_OPEN,
    /// in two lines, because the platform does the work; here it is explicit.

    /// Hands this process's activation to the instance already running, if there is one.
    ///
    /// Returns true when the caller should exit immediately and show nothing. The window it would
    /// have shown belongs to the other process, which has by then been told what to open.
    ///
    /// Must run before the XAML application starts. Once Application::Start is under way there is a
    /// window and a message loop, and the redirection would be handing a file to an instance that is
    /// itself already showing one.
    [[nodiscard]] bool RedirectToRunningInstance();

    /// The song this process was activated with, or empty if it was launched without one.
    ///
    /// Read from AppInstance::GetCurrent().GetActivatedEventArgs() and **not** from the
    /// LaunchActivatedEventArgs handed to OnLaunched, which reports Launch unconditionally even when
    /// the process was started by a file association. That is the documented trap, and the symptom is
    /// specific enough to be worth naming: double-clicking a file opens the program with nothing
    /// loaded, exactly as if the association were misconfigured.
    [[nodiscard]] std::wstring ActivatedSongPath();

    /// The same, out of an activation that arrived by redirection from another process.
    [[nodiscard]] std::wstring ActivatedSongPath(
        winrt::Microsoft::Windows::AppLifecycle::AppActivationArguments const& args);

    /// The song named on this process's command line, asked for without going through AppInstance.
    ///
    /// The fallback for a run with no package identity, where every call above throws. It is also
    /// what a file association actually delivers to a full-trust packaged application: the shell
    /// hands such a program its file as an argument rather than through the UWP file contract, which
    /// is why this is load-bearing and not a convenience for developers.
    [[nodiscard]] std::wstring CommandLineSongPath();
}
