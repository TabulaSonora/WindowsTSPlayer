#pragma once

#include <algorithm>
#include <string>
#include <string_view>

namespace tsgui
{
    /// The twelve extensions this program accepts, verbatim from the Linux front end.
    ///
    /// LDS is the one worth noting: it has no magic number and is recognised by its extension alone,
    /// which is what made the two-separator fix in Session::file_name load-bearing rather than
    /// cosmetic.
    ///
    /// There is a second copy of this list, in Package.appxmanifest, and it cannot be helped -- the
    /// shell reads the manifest before any of this code exists. A third copy inside the program would
    /// be a choice, though, which is why this header exists: the picker, the drop target and the
    /// command-line parser all read the list from here.
    inline constexpr const wchar_t* kSongExtensions[] = {
        L".mid", L".midi", L".rmi",  L".mids", L".mus", L".xmi",
        L".xmf", L".mxmf", L".gmf",  L".hmi",  L".hmp", L".lds"
    };

    /// Whether a name ends in one of them, case-insensitively.
    [[nodiscard]] inline bool IsSongFile(std::wstring_view name)
    {
        const auto dot = name.find_last_of(L'.');
        if (dot == std::wstring_view::npos) {
            return false;
        }

        std::wstring extension{ name.substr(dot) };
        std::transform(extension.begin(), extension.end(), extension.begin(), ::towlower);

        for (const auto* candidate : kSongExtensions) {
            if (extension == candidate) {
                return true;
            }
        }
        return false;
    }
}
