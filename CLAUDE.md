# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

A WinUI 3 / C++/WinRT front end for **NativeTS**, a C++ reimplementation of the Roland Sound Canvas
VA voice. NativeTS is the `nativets` submodule and is the engine; this repository is only the host
layer and the interface. It is the third front end, after the SwiftUI
[AppleTSPlayer](https://github.com/TabulaSonora/AppleTSPlayer) and the GTK4
[LinuxTSPlayer](https://github.com/TabulaSonora/LinuxTSPlayer), and much of the design is
deliberately traceable to both — comments say so where it matters.

**`src/host/` is a third copy of a shared layer**, not a fork. The same directory exists in the
Apple and Linux front ends. Keep every Windows change either purely additive or `#ifdef`-guarded, so
the three stay diffable and a fix in one can be carried to the others.

## Prerequisites that are not optional

- `git submodule update --init --recursive` (or `-DTSGUI_NATIVETS_DIR=…`, or a sibling
  `../NativeTS`, which the build falls back to).
- **The engine is inert without `SCCore.dll`** from a licensed Roland SOUND Canvas VA 1.1.6:
  27,347,456 bytes, SHA-256 `117e6aa1…c620bdb1`. Never committed. Read as data, never loaded as
  code.
- Visual Studio **Build Tools 2026** with `Microsoft.VisualStudio.ComponentGroup.WindowsAppDevelopment.VC.BuildTools`
  — note that is the VS2026 spelling; the VS2022 name was `…ComponentGroup.WindowsAppSDK.Cpp`. The
  Build Tools carry CMake, Ninja and NuGet restore, so none of those needs a separate install.

## Nothing Roland-derived is committed

Not the DLL, and not anything generated from it: `tables/` (extracted table slices), `fixtures/`
(differential oracles) and `*.wav` all carry Roland's rights and are all gitignored. The generators
say so themselves. Check `git status` before committing after any test run — a stray render in the
tree is a licensing problem, not a tidiness one.

## Commands

```powershell
cmake --preset dev                 # Debug app over an /O2 engine
cmake --build --preset dev
msbuild src\app\WindowsTSPlayer.vcxproj -t:Restore -p:Configuration=Debug -p:Platform=x64
msbuild src\app\WindowsTSPlayer.vcxproj -p:Configuration=Debug -p:Platform=x64
Add-AppxPackage -Register src\app\bin\x64\Debug\AppX\AppxManifest.xml
```

`build.ps1` runs both halves in order. **Running only the MSBuild half links a stale engine**, which
is the failure this wrapper exists to prevent.

Two ROM environment variables, and they are not the same one:

- `TS_SCCORE_DLL` — the `tabula-sonora` CLI and this application.
- `TS_SCCORE` — the engine's own Catch2 suite.

## Two build rules that are not preferences

- **Never add `/fp:fast`** (or LTO for packaging). The engine's control path is 16-bit fixed point
  and depends on wrapping; its float-narrowing guarantees break if `a*b+c` is fused into an FMA. The
  required `/fp:precise` arrives automatically through `ts::tabulasonora`, set by the engine itself.
  MSVC has no `-fwrapv`; the engine compensates in `src/dsp/fixed.hpp`, and there is a documented
  history of MSVC miscompiling that code under optimisation.
- **Never run the engine unoptimised.** At `/Od` the synth renders ~1.4× realtime and no ring can
  absorb that. `TSGUI_FAST_DEBUG` (ON by default) compiles the engine at `/O2` in Debug while
  leaving application sources at `/Od`.

`TSGUI_FAST_DEBUG` is not a one-line option on MSVC. `/RTC1` and `/O2` are mutually exclusive and
produce `D8016`, a hard error, and both `/RTC1` and `/Od` arrive in the global `CMAKE_CXX_FLAGS_DEBUG`
where they cannot be removed per target. They are stripped globally and `/Od` handed back to the
application-side targets. Note `/O2` does **not** remove `_ITERATOR_DEBUG_LEVEL=2`, which
bounds-checks every `std::vector`/`std::span` access in the DSP inner loops; changing that is an ABI
change and would break linking, so if a Debug build is too slow the answer is a `RelWithDebInfo`
CMake half, not a CRT flag.

## The two-build-system seam

CMake owns the engine and `src/host/`; MSBuild owns XAML, MIDL and cppwinrt. The direction is
one-way on purpose — MSBuild reads a props file CMake generates, and CMake never invokes MSBuild,
because the reverse would be a cycle neither could break.

The Debug↔`dev` / Release↔`release` mapping in the `.vcxproj` is what keeps the CRT consistent.
Mixing them gives `LNK2038 _ITERATOR_DEBUG_LEVEL mismatch`. The vcpkg triplet is
`x64-windows-static-md` for the same reason: static third-party libraries over the dynamic CRT the
Windows App SDK requires.

## Testing

`export-matches-cli` is the load-bearing one, unchanged in intent from the Linux build: it renders
through the export path and byte-compares the WAV against `tabula-sonora render` built from the same
engine tree, so it asks only whether this program renders the engine faithfully.

**There is no ThreadSanitizer on Windows.** MSVC has none and clang-cl does not support
`-fsanitize=thread`. `host-smoke` still drives the ring across two threads but no longer *detects*
races. A change to `ts_player.cpp`'s synchronisation must be TSan-checked in LinuxTSPlayer, which
remains the canonical place that test runs under a race detector.

The engine's own suite needs `tables/` and `fixtures/modulation.json` generated locally or a third of
it skips — see README. The modulation fixture feeds the differential oracle for the fixed-point
control path, which is the single most valuable test on this platform.

## Conventions

Comments here explain *why*, in full prose, and are dense — non-obvious decisions carry a paragraph
naming the alternative that was rejected and the concrete failure it caused. Match that when
editing; a change that removes a constraint should remove the comment that guards it, and a new
constraint deserves the same treatment.

Tone and drum-kit names, module designations and SysEx message names are never translated; they come
out of `SCCore.dll` at runtime and are the module's own display text, which has to match the patch
charts.
