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

Two of them are owed back the other way, and neither is Windows-specific:

- `encoding_code_page` is additive and sits beside `encoding_name`, which the other two pass to
  iconv. Windows has no iconv and `MultiByteToWideChar` takes a number, so it needed its own answer
  to the same question.
- **`is_shift_jis` weighs its evidence instead of demanding unanimity**, and that is a bug fix all
  three want. It used to return false at the first byte that did not fit, so one damaged field
  condemned a whole file to cp1252 — `Haru-no-umi.mid` has thirty-nine sound Japanese pairs and six
  bad bytes, and every track name in it rendered as mojibake on the strength of the six. It is the
  only non-additive change to this directory; the body changed and the signature did not, so it
  still diffs cleanly.

## Prerequisites that are not optional

- `git submodule update --init --recursive` (or `-DTSGUI_NATIVETS_DIR=…`, or a sibling
  `../NativeTS`, which the build falls back to).
- **The engine is inert without `SCCore.dll`** from a licensed Roland SOUND Canvas VA 1.1.6:
  27,347,456 bytes, SHA-256 `117e6aa1…c620bdb1`. Never committed. Read as data, never loaded as
  code.
- Visual Studio **Build Tools 2026** with `Microsoft.VisualStudio.ComponentGroup.WindowsAppDevelopment.VC.BuildTools`
  — note that is the VS2026 spelling; the VS2022 name was `…ComponentGroup.WindowsAppSDK.Cpp`. The
  Build Tools carry CMake, Ninja and NuGet restore, so none of those needs a separate install.
- **Registering a Debug build needs both Debug VCLibs frameworks installed**, which the Build Tools do
  not install and Windows garbage-collects once nothing references them. Without them
  `Add-AppxPackage -Register` fails with `0x80073CFB` naming the framework it wants and nothing about
  where to find it — they ship in the SDK, at
  `…\Windows Kits\10\ExtensionSDKs\Microsoft.VCLibs{,.Desktop}\14.0\Appx\Debug\x64\`, and both are
  needed: `Microsoft.VCLibs.140.00.Debug` and `…Debug.UWPDesktop`. Release uses the retail pair, which
  is already present on any machine that has run a Store app.

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
Add-AppxPackage -Register src\app\x64\Debug\WindowsTSPlayer\AppxManifest.xml
```

The loose layout is the output directory itself — `AppxManifest.xml`, `resources.pri` and the
compiled `.xbf` land beside the `.exe`. There is no `AppX\` subdirectory; that belongs to the
separate-packaging-project flow this repository does not use.

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
application-side targets. **The strip must precede the engine's `add_subdirectory`** — a
subdirectory copies the parent's variables when it is added, so stripping afterwards leaves the
engine's own targets with the `/RTC1` they inherited and the build dies with `D8016` naming neither
cause nor file. Release never shows it, because `/RTC1` exists only in Debug.

Note `/O2` does **not** remove `_ITERATOR_DEBUG_LEVEL=2`, which
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

Four settings in `WindowsTSPlayer.vcxproj` look removable and are not. Each is commented at the site
with the error it causes; the short version:

- **`ApplicationType=Windows Store`** is what a WinUI 3 *desktop* app needs, counter-intuitively. It
  sets `WindowsStoreApp`, which imports `Microsoft.Cpp.AppContainerApplication.props`, which is the
  only thing that points at the C++ XAML targets. Remove it and `MarkupCompilePass1`/`Pass2` never
  run — cppwinrt still emits `MainWindow.g.h`, so the failure surfaces as a missing `App.xaml.g.h`
  and every `x:Name` being an undeclared identifier.
- **`NuGetTargetMoniker` and `RuntimeIdentifiers`** pay for that: the same props sets
  `TargetPlatformIdentifier=UAP`, so NuGet demands `UAP,Version=v10.0` and `win10-x64` while
  restoring any `.vcxproj` as `native,Version=v0.0` regardless.
- **`AppContainerApplication=false`** is not a restatement of the default. `WindowsPackageType=MSIX`
  defaults it to *true*, and an AppContainer refuses `Session::load_song`'s `std::ifstream` on a raw
  path — which is the whole reason `src/host/` can stay identical to the other two ports.
- **`module.g.cpp` is listed in `ClCompile` by hand.** cppwinrt generates it and its own targets
  never compile it; without it the link fails on `WINRT_CanUnloadNow` and `WINRT_GetActivationFactory`.

`App` is deliberately absent from `App.idl`. Declaring it puts it in `module.g.cpp`, which then wants
a `factory_implementation::App` the XAML-generated headers do not define. `MainWindow` is the
opposite case and must be declared.

## Activation, and two things that are not what the documentation implies

`DISABLE_XAML_GENERATED_MAIN` is set, and `wWinMain` in `App.xaml.cpp` does the whole job — the
apartment, the single-instance question, `Application::Start`, constructing `App`. **Do not shorten
it to a redirect plus a call to the generated `wXamlGeneratedMain()`.** That was tried. Under the
define, the generated body only constructs `App` when a `decltype(App())` SFINAE probe says it can,
and MSVC answers *false* — a C++/WinRT implementation type has a non-public destructor, which is the
very thing the probe is written to see past. `Application::Start` then runs with no `Application`
ever created and the process dies about a second in, inside `Microsoft.UI.Xaml.dll`, with a stowed
exception (`0xc000027b`), no window, and nothing naming the cause. The generated helper also calls
`init_apartment` a second time.

**A full-trust packaged desktop app does not implement the UWP file contract.** Asking the shell to
activate one through it fails with `0x80270254`, "this app does not support the contract specified".
The shell hands such a program its file on the *command line*, the way it always has for a Win32
program, so `ExtendedActivationKind::Launch` is the branch a double-click actually takes and
`::File` is there only because the App SDK's arguments are documented to carry it.

Two consequences worth keeping: a redirected activation must be read from
`ILaunchActivatedEventArgs::Arguments()` and **not** from `GetCommandLineW()`, which is the receiving
process's own and makes the running instance reopen the song it is already playing; and
`RedirectActivationToAsync` failing must not be treated as success, because a registration outlives a
process that dies without unregistering, and a caller that exits anyway simply never starts.

## Testing

`export-matches-cli` is the load-bearing one, unchanged in intent from the Linux build: it renders
through the export path and byte-compares the WAV against `tabula-sonora render` built from the same
engine tree, so it asks only whether this program renders the engine faithfully.

**`TS_SCCORE_DLL` in the environment does nothing once the tree has been configured without it**, and
a run that skips two thirds of the suite still says `100% tests passed`. `TSGUI_TEST_ROM` is a
`CACHE` variable, so the environment seeds it exactly once and every later configure reuses the empty
value that was cached first. Pass it, and the MIDI file the render tests need, on the configure line:

```powershell
cmake --preset dev "-DTSGUI_TEST_ROM=<SCCore.dll>" "-DTSGUI_TEST_MIDI=<a .mid>"
```

A correct run is **three** tests. One is `song-info` alone, which means the other two were never
registered — that is the signal to check, because ctest reports nothing amiss about tests that do not
exist.

**There is no ThreadSanitizer on Windows.** MSVC has none and clang-cl does not support
`-fsanitize=thread`. `host-smoke` still drives the ring across two threads but no longer *detects*
races. A change to `ts_player.cpp`'s synchronisation must be TSan-checked in LinuxTSPlayer, which
remains the canonical place that test runs under a race detector.

The engine's own suite needs `tables/` and `fixtures/modulation.json` generated locally or a third of
it skips — see README. The modulation fixture feeds the differential oracle for the fixed-point
control path, which is the single most valuable test on this platform.

## CI, and why it builds differently from you

`.github/workflows/build.yml`, on `windows-latest`, in two jobs. The first builds the engine, the
host layer and the tests; the second builds the application. They are separate because they fail for
different reasons — the first going red means the code is wrong, the second means the runner needs
different arguments.

Two things are pinned to a Visual Studio no runner has, and both had to become overridable rather
than assumed:

- **The generator.** `dev` and `release` name `Visual Studio 18 2026`. The `ci` preset uses Ninja
  instead, which takes whichever `cl.exe` is on `PATH`.
- **The toolset.** `PlatformToolset` is now a conditional default of `v145` rather than a fixed
  value, so CI can pass `v143`. Note what that actually needs: building with `-p:PlatformToolset=v143`
  on a machine that has the v143 *compiler* still fails with `MSB8020`, "the build tools for 'v143'
  application Type UWP cannot be found". `ApplicationType=Windows Store` pulls in the UWP build tools
  for the chosen toolset, not merely the compiler.

`vcpkg.json` carries a `builtin-baseline`. Without one, a vcpkg instance that is not a git clone —
the copy bundled with Visual Studio, and whatever a runner has — refuses to resolve ports at all:
`this vcpkg instance requires a manifest with a specified baseline`.

CI runs **one** test, `song-info`, and the workflow asserts that name is present rather than reading
the pass count, for the reason in the Testing section above.

## Conventions

Comments here explain *why*, in full prose, and are dense — non-obvious decisions carry a paragraph
naming the alternative that was rejected and the concrete failure it caused. Match that when
editing; a change that removes a constraint should remove the comment that guards it, and a new
constraint deserves the same treatment.

Tone and drum-kit names, module designations and SysEx message names are never translated; they come
out of `SCCore.dll` at runtime and are the module's own display text, which has to match the patch
charts.
