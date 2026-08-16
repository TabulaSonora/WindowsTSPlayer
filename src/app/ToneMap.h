#pragma once

#include "tabulasonora/patch_directory.hpp"

// Not namespace WindowsTSPlayer, which would be ambiguous with winrt::WindowsTSPlayer in every
// translation unit that says `using namespace winrt;` -- which is all of them. `tsgui` is the name
// the CMake targets already use for this layer.
namespace tsgui
{
    /// How a module's name is written for a person to read.
    ///
    /// The library's own `tone_map_name` returns the token a command line takes -- `sc8820`, `xg` --
    /// which is right for an argument and wrong on a mixer strip. The values are the module's own
    /// bank codes rather than an ordering of ours, so they are matched explicitly.
    ///
    /// Never translated. These are module designations and have to match the patch charts.
    inline const char* ToneMapDisplayName(int map) noexcept
    {
        switch (static_cast<ts::ToneMap>(map)) {
        case ts::ToneMap::sc55: return "SC-55";
        case ts::ToneMap::sc88: return "SC-88";
        case ts::ToneMap::sc88pro: return "SC-88Pro";
        case ts::ToneMap::sc8820: return "SC-8820";
        case ts::ToneMap::xg: return "XG";
        }
        return "";
    }
}
