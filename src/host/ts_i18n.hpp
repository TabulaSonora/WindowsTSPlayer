#pragma once

// Translation for the host layer.
//
// There is no GUI toolkit below this line -- the rule this whole directory is built on -- and on
// Windows there is no libintl either. The catalogues here are MRT `.resw`, and the only thing that
// can read those is Windows.ApplicationModel.Resources, which is WinRT. Including that here would
// break the rule outright.
//
// So the lookup is *injected* rather than linked: the application installs a resolver before
// anything else runs, a test binary linking this layer alone installs none and reads English, and
// TS_() is spelled the same in all three ports. The Linux and Apple builds bind the same macro to
// dgettext instead; the call sites do not know the difference.
//
// The names are deliberately not _, N_, C_, NC_ or Q_. A translation unit that included both this
// header and <glib/gi18n.h> would otherwise redefine glib's on top of it -- the collision the Linux
// build hit, and worth keeping the guard against even where glib is absent.

#include <atomic>
#include <cstdio>
#include <string>

namespace ts::host {

/// How this layer's own prose is translated.
///
/// A plain function pointer rather than std::function, and read through an atomic rather than a
/// mutex: it is set once, from one thread, before any other work, and then read from several --
/// the render thread reaches `Session::load_song`'s error path -- so it has to be readable without
/// allocating and without blocking.
using Translator = const char* (*)(const char* msgid) noexcept;

/// Installs the resolver. Call once, before anything that can translate.
void set_translator(Translator translator) noexcept;

/// Looks `msgid` up, or returns it unchanged when no resolver is installed.
///
/// **A resolver must return a pointer valid for the rest of the process.** Callers spend the result
/// immediately -- `std::runtime_error(TS_("..."))`, or `format_text(TS_("..."), ...)` below -- but
/// "immediately" is not "before the temporary dies": a resolver returning `.c_str()` of a local
/// std::string would dangle inside the very expression that consumed it. The application-side
/// implementation therefore interns every string it has ever returned and never erases. There are
/// twelve of them and the display language cannot change mid-process, so the cache is bounded and
/// never invalidated.
[[nodiscard]] const char* translate(const char* msgid) noexcept;

/// Look the string up now.
#define TS_(String) ::ts::host::translate(String)

/// Mark the string for extraction without looking it up, for initialisers and static tables where
/// the lookup has to happen later at the point of use.
#define TS_N_(String) (String)

/// printf into a std::string.
///
/// Here rather than in either caller because both files that translate need it. std::snprintf and
/// not any toolkit's formatter for the reason this header exists at all -- there is nothing below
/// this line -- and not std::format either: a translated format string only exists at run time, and
/// std::format wants its own at compile time.
///
/// Sized by asking snprintf what it needs before writing, so a translation longer than the English
/// original is never truncated.
template <typename... Args>
std::string format_text(const char* format, Args... args)
{
    const int needed = std::snprintf(nullptr, 0, format, args...);
    if (needed <= 0) {
        return {};
    }
    std::string text(static_cast<std::size_t>(needed), '\0');
    std::snprintf(text.data(), static_cast<std::size_t>(needed) + 1, format, args...);
    return text;
}

} // namespace ts::host
