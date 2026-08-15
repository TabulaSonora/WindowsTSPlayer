#ifndef TSGUI_HOST_TS_TYPES_H
#define TSGUI_HOST_TS_TYPES_H

// Plain C, deliberately: this header is included both by the Objective-C++ façade and by the C++
// that talks to the engine, so it cannot depend on Foundation. An enum with a fixed underlying
// type is what NS_ENUM expands to anyway, and the Swift importer treats it the same way.

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Which module's tone map program changes resolve against.
///
/// Values match `ts::ToneMap` exactly; they are the module's own bank codes, not an ordering of
/// ours.
typedef enum : int32_t {
    TSToneMapSC55 = 1,
    TSToneMapSC88 = 2,
    TSToneMapSC88Pro = 3,
    TSToneMapSC8820 = 4,
    TSToneMapXG = 0x77,
} TSToneMap;

/// The settings that live in the generator's construction options.
///
/// Changing any of these rebuilds the `ToneGenerator` -- but not the `NoteRenderer`, so the 27 MB
/// of tables are read once per session no matter how often the vintage changes. Part settings are
/// carried across the rebuild by replaying the messages that made them; sounding voices are not.
typedef struct {
    TSToneMap map;

    /// Voices before stealing. The hardware's own limit is 64.
    int32_t polyphony;

    /// 1, 2 or 4 -- giving 16, 32 or 64 parts. The module has two.
    int32_t ports;

    bool reverb;
    bool chorus;
    bool delay;
    bool efx;

    /// The wide band-limiting resampler, and with it the removal of the module's pitch increment
    /// ceiling.
    ///
    /// On by default, and the one place the engine knowingly departs from `SCCore.dll`: the
    /// module's 4-tap kernel saturates at four times a wave's native rate, which pins a portamento
    /// glide that begins high enough and leaves it motionless until it has descended past the
    /// ceiling. Off reproduces the module exactly, aliasing and all -- which is what anything being
    /// compared against the DLL needs.
    bool extendedInterpolation;

    /// Deliver the SysEx the module's input queue would discard.
    ///
    /// Off by default, and the *second* place this program can knowingly depart from `SCCore.dll`
    /// -- the same shape as `extendedInterpolation` above, and to be treated the same way.
    ///
    /// The module's input queue takes 2,048 packets per control tick and silently drops the rest,
    /// so a file whose opening is a bulk dump larger than that never has its tail delivered and the
    /// hardware plays whatever the dump's surviving prefix chose. The engine reproduces that.
    /// Turning this on starts a fresh queue window at every SysEx message, which is something the
    /// module cannot be made to do: upstream measured `TG_flushMidi` before every message and the
    /// render came back byte-identical, because the bound is on the ready buffer that only
    /// `TG_Process` drains. So this plays a file as written rather than as the hardware receives
    /// it, and anything being compared against the DLL wants it off.
    bool flushBeforeSysex;

    /// Linear gain on the finished mix. Applied live, without a rebuild.
    double outputGain;
} TSEngineSettings;

/// The engine's defaults: an SC-8820 with the module's own polyphony, two ports and every effect.
extern TSEngineSettings TSEngineSettingsDefault(void);

/// Parts the engine can address at most -- sixty-four, `port * 16 + channel`.
#define TS_MAX_PARTS 64

#ifdef __cplusplus
} // extern "C"
#endif

#endif // TSGUI_HOST_TS_TYPES_H
