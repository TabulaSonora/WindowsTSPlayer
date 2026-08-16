#include "pch.h"

#include "Activation.h"

#include "SongFormats.h"

// CommandLineToArgvW. Not in windows.h under WIN32_LEAN_AND_MEAN, which the project defines.
#include <shellapi.h>

#include <thread>

using namespace winrt;
using namespace Microsoft::Windows::AppLifecycle;
using namespace Windows::ApplicationModel::Activation;

namespace {

/// The key every instance of this program registers under.
///
/// One key, so there is one instance. A program that could usefully have two windows open on two
/// songs would key on something per-document; this one owns an audio device and a synthesiser and
/// can only sensibly be playing one thing.
constexpr wchar_t instance_key[] = L"main";

/// The last thing in a command line that looks like a song.
///
/// Split with CommandLineToArgvW rather than by hand, because it is the parser the runtime itself
/// uses -- so it agrees with whatever the shell did on the way in, and a path with spaces in it stays
/// one path instead of becoming two files.
///
/// **Every token is considered, including the first.** Which token holds the program name depends on
/// where the string came from: GetCommandLineW always begins with it, and the Arguments of an
/// activation may or may not, depending on who raised it. Rather than guess, this asks whether a
/// token *is a song* -- which the executable never is, and a switch never is either. That makes the
/// question independent of the caller instead of correct for one caller and wrong for the next.
///
/// The last such token rather than the first: there is no playlist, so the honest reading of "open
/// these five" is the same as opening them one after another.
std::wstring LastSongIn(const wchar_t* commandLine)
{
    if (commandLine == nullptr || *commandLine == L'\0') {
        return {};
    }

    int count = 0;
    wchar_t** argv = CommandLineToArgvW(commandLine, &count);
    if (argv == nullptr) {
        return {};
    }

    std::wstring found;
    for (int index = count - 1; index >= 0; --index) {
        if (tsgui::IsSongFile(argv[index])) {
            found = argv[index];
            break;
        }
    }

    LocalFree(argv);
    return found;
}

/// The song named on this process's own command line.
///
/// This is the `%f` of the GTK build's desktop entry, and on Windows it is also how a file
/// association actually arrives -- see the note on FilePathFrom below.
std::wstring CommandLinePath()
{
    return LastSongIn(GetCommandLineW());
}

/// The song an activation names, whichever of the two ways it names one.
///
/// Both are needed, and finding that out is what this milestone was for. A full-trust packaged
/// desktop application does not implement the UWP file contract -- asking the shell to activate one
/// through it fails with 0x80270254, "this app does not support the contract specified" -- because
/// the shell hands such a program its file on the *command line*, the way it always has for a Win32
/// program. So the Launch branch is the one a double-click actually takes, and the File branch is
/// there because the App SDK's activation arguments are documented to carry it and a manifest is free
/// to make it true.
///
/// The last file rather than the first, when several arrive together. There is no playlist, so the
/// honest reading of "open these five" is the same as opening them one after another, which is what
/// the GTK build settled on for the same reason.
std::wstring FilePathFrom(AppActivationArguments const& args)
{
    if (args == nullptr) {
        return {};
    }

    if (args.Kind() == ExtendedActivationKind::Launch) {
        // The activation's own arguments first, and GetCommandLineW only as a fallback. **These are
        // not the same string when the activation came from somewhere else**, which is the whole
        // point of the redirection: a second process hands its arguments over, and this one has to
        // read *those* rather than its own. Reading its own instead is not an obvious failure either
        // -- it reopens the song it is already playing, from the top, which looks like the file
        // association reopening the wrong file rather than like the handler reading the wrong string.
        if (const auto launch = args.Data().try_as<ILaunchActivatedEventArgs>()) {
            if (auto found = LastSongIn(launch.Arguments().c_str()); !found.empty()) {
                return found;
            }
        }
        return CommandLinePath();
    }

    if (args.Kind() != ExtendedActivationKind::File) {
        return {};
    }

    const auto file = args.Data().try_as<IFileActivatedEventArgs>();
    if (file == nullptr) {
        return {};
    }

    const auto items = file.Files();
    for (uint32_t index = items.Size(); index > 0; --index) {
        if (const auto storage = items.GetAt(index - 1).try_as<Windows::Storage::StorageFile>()) {
            // Path(), which is a raw filesystem path, and this is the assumption the whole port
            // rests on: Session::load_song opens it with std::ifstream, shared verbatim with the
            // Apple and Linux front ends. It holds because AppContainerApplication is false in the
            // project and runFullTrust is declared in the manifest, so this process has the full
            // user token. Were either wrong, the answer would be a StorageFile-based reader inside
            // src/host/, which would fork that directory from the other two.
            //
            // Empty for an item that is not on a local filesystem at all, which a StorageFile is
            // free to be. Skipped rather than reported here; the window says so, because it is the
            // thing with somewhere to say it.
            const hstring path = storage.Path();
            if (!path.empty()) {
                return std::wstring{ path };
            }
        }
    }

    return {};
}

/// Waits for the redirection to complete without deadlocking the thread it was started from.
///
/// Not `.get()` on the returned IAsyncAction, which is the obvious spelling and hangs. This runs on
/// a single-threaded apartment, and the completion has to marshal back through that apartment's
/// message queue -- so blocking the thread outright blocks the very pump the completion needs.
/// CoWaitForMultipleObjects keeps pumping while it waits, which is the documented way out and the
/// reason this needs a thread at all.
/// Returns whether the other instance actually took it.
///
/// **Whether, and not just that it was tried.** The registration outlives the process that made it
/// when that process dies without unregistering -- a crash, or a developer's Stop-Process -- so
/// FindOrRegisterForKey can hand back an instance that no longer exists. Redirecting to it fails,
/// and a caller that treated "tried" as "done" would exit having shown nothing and opened nothing:
/// the program simply would not start, once, for no reason the person running it could see. That is
/// exactly what happened the first time this was tested against a key left behind by a killed build.
bool WaitForRedirect(AppInstance const& instance, AppActivationArguments const& args)
{
    const HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (done == nullptr) {
        return false;
    }

    bool handed_over = false;
    std::thread worker([instance, args, done, &handed_over]() {
        try {
            instance.RedirectActivationToAsync(args).get();
            handed_over = true;
        } catch (const hresult_error&) {
            // Nobody home. The caller starts normally instead, which costs a second window in the
            // rare case the other instance was alive and merely slow, and saves the program from not
            // starting at all in the common one.
        }
        SetEvent(done);
    });

    DWORD index = 0;
    HANDLE handles[] = { done };
    CoWaitForMultipleObjects(CWMO_DEFAULT, INFINITE, 1, handles, &index);

    worker.join();
    CloseHandle(done);
    return handed_over;
}

} // namespace

namespace tsgui
{
    bool RedirectToRunningInstance()
    {
        auto keyed = AppInstance::FindOrRegisterForKey(instance_key);

        // FindOrRegisterForKey is one call doing both halves, which is what makes it safe: two
        // processes racing to start cannot both believe they are the first, because the registration
        // is what decides it rather than a lookup followed by a claim.
        if (keyed.IsCurrent()) {
            return false;
        }

        return WaitForRedirect(keyed, AppInstance::GetCurrent().GetActivatedEventArgs());
    }

    std::wstring ActivatedSongPath()
    {
        return FilePathFrom(AppInstance::GetCurrent().GetActivatedEventArgs());
    }

    std::wstring ActivatedSongPath(AppActivationArguments const& args)
    {
        return FilePathFrom(args);
    }

    std::wstring CommandLineSongPath()
    {
        return CommandLinePath();
    }
}
