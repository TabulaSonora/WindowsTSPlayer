# Tabula Sonora Player for Windows

A native WinUI 3 front end for [NativeTS](https://github.com/TabulaSonora/NativeTS), a C++
reimplementation of the Roland Sound Canvas VA synthesizer voice.

It is the third front end for the same engine, after the SwiftUI
[Apple build](https://github.com/TabulaSonora/AppleTSPlayer) and the GTK4
[Linux build](https://github.com/TabulaSonora/LinuxTSPlayer), and aims at the same feature set:
transport with seeking and looping at the file's own loop points, a mixer strip per part the file
addresses, engine settings, and WAV export through the library's own writer so the bytes match
`tabula-sonora render`.

It plays far more than SMF — RIFF-MIDI, DirectMusic `MIDS`, DOOM `MUS`, Miles `XMI`, `GMF`, both HMI
containers, Mobile XMF and LDS tracker files — because the engine converts all of them to SMF on the
way in.

**Status: in development.** The engine and host layer come first; the interface follows.

## The ROM

**The engine is inert without `SCCore.dll`** from a licensed Roland SOUND Canvas VA 1.1.6 install:
exactly 27,347,456 bytes, SHA-256 `117e6aa1…c620bdb1`. It is not included and cannot be — it holds
Roland's 24 MB wave ROM. It is read as *data* and never loaded as code. The application asks for it
on first launch, verifies it, and keeps its own copy under the app's local data folder.

Nothing Roland-derived is committed here, and that includes anything *generated* from the DLL:
extracted tables, differential fixtures and rendered audio all carry the same rights. `.gitignore`
excludes each category. See `NOTICE.md`.

## Building

Prerequisites: **Visual Studio Build Tools 2026** with the C++ and Windows App SDK components, the
Windows 11 SDK, and [vcpkg](https://github.com/microsoft/vcpkg) with `VCPKG_ROOT` set. No separate
CMake, Ninja or NuGet install is needed — the Build Tools carry all three.

The build has two halves, and they run in that order. CMake and vcpkg build the engine and the host
layer as static libraries; MSBuild builds the WinUI 3 application and links them.

```powershell
git clone --recurse-submodules https://github.com/TabulaSonora/WindowsTSPlayer.git
cd WindowsTSPlayer

cmake --preset release
cmake --build --preset release

msbuild src\app\WindowsTSPlayer.vcxproj -t:Restore -p:Configuration=Release -p:Platform=x64
msbuild src\app\WindowsTSPlayer.vcxproj -p:Configuration=Release -p:Platform=x64
```

`build.ps1` runs both halves for you, which is the point of it: building only the second leaves the
application linking a stale engine.

The engine is the `nativets` submodule. `TSGUI_NATIVETS_DIR` overrides it, and if the submodule was
not checked out the build falls back to a sibling `../NativeTS` before giving up with a message
saying so.

### Running it

The application is packaged as MSIX. For development it needs no signature at all — with Developer
Mode enabled, register the loose layout:

```powershell
Add-AppxPackage -Register src\app\bin\x64\Release\AppX\AppxManifest.xml
```

Package identity is what makes the file associations and the media transport controls work, so a
registered build is the only one where those can be tested.

### Two build rules that are not preferences

- **Never add `/fp:fast`** (or LTO for packaging). The engine's control path is 16-bit fixed point
  and depends on wrapping; its float-narrowing guarantees break if the compiler fuses `a*b+c` into
  an FMA. The required `/fp:precise` arrives automatically through `ts::tabulasonora`.
- **Do not run the engine unoptimised.** At `/Od` the synth renders only ~1.4× realtime, which no
  ring can absorb. `TSGUI_FAST_DEBUG` (on by default) compiles the engine at `/O2` even in Debug.

## Testing

```powershell
$env:TS_SCCORE_DLL = "<path to SCCore.dll>"    # the CLI and the GUI read this
$env:TS_SCCORE     = "<path to SCCore.dll>"    # the engine's own suite reads this one
ctest --preset release --output-on-failure
```

The load-bearing test is `export-matches-cli`: it renders a song through this program's export path
and byte-compares the WAV against `tabula-sonora render`. Both sides are built from the same engine
tree, so the test asks only whether *this program* renders the engine faithfully, and does not turn
red every time upstream changes the voice.

The engine's own suite is worth running once on a new toolchain, and it needs two generated inputs
that are not in the tree — both Roland-derived, both gitignored:

```powershell
tabula-sonora extract-tables --dll <SCCore.dll> tables
py tools\dump_modulation.py <SCCore.dll> fixtures\modulation.json
```

Without them a third of the suite skips. The modulation fixture in particular feeds the differential
oracle for the fixed-point control path, which is the test that matters most on a compiler this
engine has not been built with before: it is written independently of the C++, with explicit 16-bit
masks at every point the engine's arithmetic wraps.

## Differences from the Linux build

- **Live MIDI input is not implemented.** The Linux build takes ALSA sequencer input; there is no
  Windows equivalent here yet, so this is a file player.
- **System Media Transport Controls** replace MPRIS2, so the media keys and the Windows now-playing
  flyout drive the transport.
- **File associations** are declared in the package manifest rather than through
  `shared-mime-info`.

## Licence

BSD 3-Clause. `NOTICE.md` must travel with any binary: reverb and chorus coefficients compiled into
the engine are Roland-derived.
