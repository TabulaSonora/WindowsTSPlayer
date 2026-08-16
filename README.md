# Tabula Sonora Player for Windows

[![build](https://github.com/TabulaSonora/WindowsTSPlayer/actions/workflows/build.yml/badge.svg)](https://github.com/TabulaSonora/WindowsTSPlayer/actions/workflows/build.yml)

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

**Status: in development.** The engine, the host layer and the interface are in place -- transport,
mixer, settings, song information, file associations and the media transport controls. Translations
are not: every string is English and lives in the source rather than in a resource file.

## The ROM

**The engine is inert without `SCCore.dll`** from a licensed Roland SOUND Canvas VA 1.1.6 install:
exactly 27,347,456 bytes, SHA-256 `117e6aa1…c620bdb1`. It is not included and cannot be — it holds
Roland's 24 MB wave ROM. It is read as *data* and never loaded as code. The application asks for it
on first launch, verifies it, and keeps its own copy under the app's local data folder.

Nothing Roland-derived is committed here, and that includes anything *generated* from the DLL:
extracted tables, differential fixtures and rendered audio all carry the same rights. `.gitignore`
excludes each category. See `NOTICE.md`.

## Building

Prerequisites: **Visual Studio Build Tools 2026** with
`Microsoft.VisualStudio.ComponentGroup.WindowsAppDevelopment.VC.BuildTools` (that is the 2026
spelling; under 2022 it was `…ComponentGroup.WindowsAppSDK.Cpp`), the Windows 11 SDK, and
[vcpkg](https://github.com/microsoft/vcpkg) with `VCPKG_ROOT` set. No separate CMake, Ninja or NuGet
install is needed — the Build Tools carry all three.

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
Add-AppxPackage -Register src\app\x64\Release\WindowsTSPlayer\AppxManifest.xml
```

That path is the output directory itself. There is no `AppX\` subdirectory — the manifest,
`resources.pri` and the compiled `.xbf` land beside the `.exe`, and `AppX\` belongs to the
separate-packaging-project flow this repository does not use.

Package identity is what makes the file associations and the media transport controls work, so a
registered build is the only one where those can be tested.

**Registering a Debug build additionally needs both Debug VCLibs frameworks**, which the Build Tools
do not install and which Windows garbage-collects once nothing references them. Without them
`Add-AppxPackage -Register` fails with `0x80073CFB`, naming the framework it wants and saying nothing
about where to find it. They ship in the SDK:

```powershell
$sdk = "${env:ProgramFiles(x86)}\Microsoft SDKs\Windows Kits\10\ExtensionSDKs"
Add-AppxPackage "$sdk\Microsoft.VCLibs\14.0\Appx\Debug\x64\Microsoft.VCLibs.x64.Debug.14.00.appx"
Add-AppxPackage "$sdk\Microsoft.VCLibs.Desktop\14.0\Appx\Debug\x64\Microsoft.VCLibs.x64.Debug.14.00.Desktop.appx"
```

Release uses the retail pair, which is already present on any machine that has run a Store app.

### Two build rules that are not preferences

- **Never add `/fp:fast`** (or LTO for packaging). The engine's control path is 16-bit fixed point
  and depends on wrapping; its float-narrowing guarantees break if the compiler fuses `a*b+c` into
  an FMA. The required `/fp:precise` arrives automatically through `ts::tabulasonora`.
- **Do not run the engine unoptimised.** At `/Od` the synth renders only ~1.4× realtime, which no
  ring can absorb. `TSGUI_FAST_DEBUG` (on by default) compiles the engine at `/O2` even in Debug.

## Testing

```powershell
cmake --preset release "-DTSGUI_TEST_ROM=<path to SCCore.dll>" "-DTSGUI_TEST_MIDI=<some .mid>"
cmake --build --preset release
ctest --preset release --output-on-failure
```

**Pass the ROM on the configure line, not in the environment.** `TSGUI_TEST_ROM` is a `CACHE`
variable, so `$env:TS_SCCORE_DLL` seeds it exactly once and every later configure reuses whatever was
cached the first time — which, if the tree was ever configured without it, is nothing. A run that has
skipped two thirds of the suite still says `100% tests passed`, because ctest has nothing to say
about tests that were never registered.

**A correct run is three tests.** One is `song-info` alone, and that is the signal to go back and
check the configure line. The engine's own suite reads a *different* variable, `TS_SCCORE`; setting
only one of the two is the usual reason a third of a run silently disappears.

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

### Continuous integration

`.github/workflows/build.yml` builds on `windows-latest` and runs the suite. It has no `SCCore.dll`
and never will — that is 24 MB of Roland's wave ROM — so exactly one test runs there, `song-info`,
which exercises the whole metadata reader over files it assembles byte by byte. The workflow asserts
that name is present rather than trusting the pass count, for the reason above.

CI uses a `ci` preset built on Ninja. The `dev` and `release` presets name the
`Visual Studio 18 2026` generator, which is right on a development machine and exists on no runner;
Ninja takes whichever compiler is on `PATH` and has no year baked into it. The application half is a
separate job because what fails there is the toolchain rather than the code.

## Differences from the Linux build

- **Live MIDI input is not implemented.** The Linux build takes ALSA sequencer input; there is no
  Windows equivalent here yet, so this is a file player.
- **System Media Transport Controls** replace MPRIS2, so the media keys and the Windows now-playing
  flyout drive the transport.
- **File associations** are declared in the package manifest rather than through
  `shared-mime-info`. Windows dispatches on extension alone, so the 107 lines of magic-number rules
  that build needs have no counterpart here.
- **There are no translations.** The Linux and Apple builds carry Spanish and Japanese catalogues;
  this one is English only, with its strings in the source rather than in `.resw` resources.
- **No ThreadSanitizer.** MSVC has none and clang-cl does not support `-fsanitize=thread` on
  Windows, so `host-smoke` still drives the ring across two threads here but no longer *detects*
  races. A change to `src/host/ts_player.cpp`'s synchronisation has to be checked in the Linux build,
  which stays the canonical place that test runs under a race detector.

## Licence

BSD 3-Clause. `NOTICE.md` must travel with any binary: reverb and chorus coefficients compiled into
the engine are Roland-derived.
