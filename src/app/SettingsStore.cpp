#include "pch.h"

#include "SettingsStore.h"

#include "tabulasonora/patch_directory.hpp"

#include <algorithm>

using namespace winrt;
using namespace Windows::Storage;

namespace {

/// Defaults, taken from the GTK build's schema so the three ports start a new profile identically.
///
/// Not "sensible values chosen here": these are the engine's own, and the one that looks arbitrary
/// is not. The window's 830px height is what GTK arrived at after clamping a taller default to the
/// monitor work area, and it is carried over rather than re-derived.
constexpr int default_map = static_cast<int>(ts::ToneMap::sc8820);
constexpr int default_polyphony = 64;
constexpr int default_ports = 2;
constexpr bool default_extended_interpolation = true;
constexpr bool default_flush_before_sysex = false;
constexpr bool default_reverb = true;
constexpr bool default_chorus = true;
constexpr bool default_delay = true;
constexpr bool default_efx = true;
constexpr double default_output_gain = 1.0;
constexpr int default_latency_ms = 40;
constexpr bool default_looping = false;
constexpr bool default_rom_verified = false;
constexpr int default_window_width = 700;
constexpr int default_window_height = 830;
constexpr bool default_window_maximized = false;

ApplicationDataContainer Values()
{
    return ApplicationData::Current().LocalSettings();
}

int ReadInt(const wchar_t* key, int fallback)
{
    const auto stored = Values().Values().TryLookup(key);
    return stored == nullptr ? fallback : unbox_value_or<int32_t>(stored, fallback);
}

double ReadDouble(const wchar_t* key, double fallback)
{
    const auto stored = Values().Values().TryLookup(key);
    return stored == nullptr ? fallback : unbox_value_or<double>(stored, fallback);
}

bool ReadBool(const wchar_t* key, bool fallback)
{
    const auto stored = Values().Values().TryLookup(key);
    return stored == nullptr ? fallback : unbox_value_or<bool>(stored, fallback);
}

} // namespace

namespace tsgui
{
    SettingsStore::SettingsStore()
        : map_(ReadInt(L"map", default_map))
        , polyphony_(ReadInt(L"polyphony", default_polyphony))
        , ports_(ReadInt(L"ports", default_ports))
        , extendedInterpolation_(ReadBool(L"extended-interpolation", default_extended_interpolation))
        , flushBeforeSysex_(ReadBool(L"flush-before-sysex", default_flush_before_sysex))
        , reverb_(ReadBool(L"reverb", default_reverb))
        , chorus_(ReadBool(L"chorus", default_chorus))
        , delay_(ReadBool(L"delay", default_delay))
        , efx_(ReadBool(L"efx", default_efx))
        , outputGain_(ReadDouble(L"output-gain", default_output_gain))
        , latencyMs_(ReadInt(L"latency-ms", default_latency_ms))
        , looping_(ReadBool(L"looping", default_looping))
        , romVerified_(ReadBool(L"rom-verified", default_rom_verified))
        , windowWidth_(ReadInt(L"window-width", default_window_width))
        , windowHeight_(ReadInt(L"window-height", default_window_height))
        , windowMaximized_(ReadBool(L"window-maximized", default_window_maximized))
    {
        // Clamped on the way in, not only on the way out. GSettings enforces a <range> on several of
        // these and LocalSettings enforces nothing at all, so a value that has been corrupted -- or
        // written by an older build with different bounds -- would otherwise reach the engine.
        latencyMs_ = std::clamp(latencyMs_, 10, 400);
        outputGain_ = std::clamp(outputGain_, 0.0, 2.0);
        windowWidth_ = std::clamp(windowWidth_, 360, 8192);
        windowHeight_ = std::clamp(windowHeight_, 360, 8192);
    }

    void SettingsStore::write(const std::string& key, int value)
    {
        Values().Values().Insert(to_hstring(key), box_value(static_cast<int32_t>(value)));
        if (changed) {
            changed(key);
        }
    }

    void SettingsStore::write(const std::string& key, double value)
    {
        Values().Values().Insert(to_hstring(key), box_value(value));
        if (changed) {
            changed(key);
        }
    }

    void SettingsStore::write(const std::string& key, bool value)
    {
        Values().Values().Insert(to_hstring(key), box_value(value));
        if (changed) {
            changed(key);
        }
    }

    void SettingsStore::set_map(int value)
    {
        if (map_ == value) return;
        map_ = value;
        write("map", value);
    }

    void SettingsStore::set_polyphony(int value)
    {
        if (polyphony_ == value) return;
        polyphony_ = value;
        write("polyphony", value);
    }

    void SettingsStore::set_ports(int value)
    {
        if (ports_ == value) return;
        ports_ = value;
        write("ports", value);
    }

    void SettingsStore::set_extended_interpolation(bool value)
    {
        if (extendedInterpolation_ == value) return;
        extendedInterpolation_ = value;
        write("extended-interpolation", value);
    }

    void SettingsStore::set_flush_before_sysex(bool value)
    {
        if (flushBeforeSysex_ == value) return;
        flushBeforeSysex_ = value;
        write("flush-before-sysex", value);
    }

    void SettingsStore::set_reverb(bool value)
    {
        if (reverb_ == value) return;
        reverb_ = value;
        write("reverb", value);
    }

    void SettingsStore::set_chorus(bool value)
    {
        if (chorus_ == value) return;
        chorus_ = value;
        write("chorus", value);
    }

    void SettingsStore::set_delay(bool value)
    {
        if (delay_ == value) return;
        delay_ = value;
        write("delay", value);
    }

    void SettingsStore::set_efx(bool value)
    {
        if (efx_ == value) return;
        efx_ = value;
        write("efx", value);
    }

    void SettingsStore::set_output_gain(double value)
    {
        value = std::clamp(value, 0.0, 2.0);
        if (outputGain_ == value) return;
        outputGain_ = value;
        write("output-gain", value);
    }

    void SettingsStore::set_latency_ms(int value)
    {
        value = std::clamp(value, 10, 400);
        if (latencyMs_ == value) return;
        latencyMs_ = value;
        write("latency-ms", value);
    }

    void SettingsStore::set_looping(bool value)
    {
        if (looping_ == value) return;
        looping_ = value;
        write("looping", value);
    }

    void SettingsStore::set_rom_verified(bool value)
    {
        if (romVerified_ == value) return;
        romVerified_ = value;
        write("rom-verified", value);
    }

    void SettingsStore::set_window_geometry(int width, int height, bool maximized)
    {
        // Written together and without raising three separate changes: this runs on every resize,
        // and each one would otherwise walk the whole changed handler.
        windowWidth_ = std::clamp(width, 360, 8192);
        windowHeight_ = std::clamp(height, 360, 8192);
        windowMaximized_ = maximized;

        auto values = Values().Values();
        values.Insert(L"window-width", box_value(static_cast<int32_t>(windowWidth_)));
        values.Insert(L"window-height", box_value(static_cast<int32_t>(windowHeight_)));
        values.Insert(L"window-maximized", box_value(windowMaximized_));
    }

    TSEngineSettings SettingsStore::engine_settings() const
    {
        TSEngineSettings engine{};
        engine.map = static_cast<TSToneMap>(map_);
        engine.polyphony = polyphony_;
        engine.ports = ports_;
        engine.reverb = reverb_;
        engine.chorus = chorus_;
        engine.delay = delay_;
        engine.efx = efx_;
        engine.extendedInterpolation = extendedInterpolation_;
        engine.flushBeforeSysex = flushBeforeSysex_;
        engine.outputGain = outputGain_;

        // Looping is deliberately not here, and neither is latency. Neither lives in the generator's
        // construction options, and latency in particular must not: the ring is sized once and the
        // setting only moves the fill target inside it. See the apply path for why looping's absence
        // is load-bearing rather than tidiness.
        return engine;
    }
}
