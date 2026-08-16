#pragma once

#include "host/ts_types.h"

#include <functional>
#include <string>

namespace tsgui
{
    /// Everything the program remembers between runs.
    ///
    /// Fifteen keys, which is the GTK build's sixteen less `midi-auto-connect` -- live MIDI input is
    /// not ported. The names are kept hyphenated and identical to that build's GSettings keys so the
    /// three front ends' documentation can describe one set of names rather than three.
    ///
    /// **The store is the source of truth, not a mirror of the interface.** The dialog writes here,
    /// this raises `changed`, and one place turns that into engine settings. A control that both
    /// wrote a key and set the engine itself would have two paths to the same state, and they would
    /// disagree the first time one of them was reached from somewhere else.
    ///
    /// One key per field rather than a single serialised blob, so a key added later falls back to its
    /// own default instead of invalidating everything stored beside it.
    class SettingsStore
    {
    public:
        SettingsStore();

        // -- Engine construction options. Changing any of these rebuilds the generator. ------------

        int map() const noexcept { return map_; }
        void set_map(int value);

        int polyphony() const noexcept { return polyphony_; }
        void set_polyphony(int value);

        int ports() const noexcept { return ports_; }
        void set_ports(int value);

        bool extended_interpolation() const noexcept { return extendedInterpolation_; }
        void set_extended_interpolation(bool value);

        bool flush_before_sysex() const noexcept { return flushBeforeSysex_; }
        void set_flush_before_sysex(bool value);

        bool reverb() const noexcept { return reverb_; }
        void set_reverb(bool value);

        bool chorus() const noexcept { return chorus_; }
        void set_chorus(bool value);

        bool delay() const noexcept { return delay_; }
        void set_delay(bool value);

        bool efx() const noexcept { return efx_; }
        void set_efx(bool value);

        // -- Applied live, without a rebuild -------------------------------------------------------

        double output_gain() const noexcept { return outputGain_; }
        void set_output_gain(double value);

        int latency_ms() const noexcept { return latencyMs_; }
        void set_latency_ms(int value);

        bool looping() const noexcept { return looping_; }
        void set_looping(bool value);

        // -- Neither. Stored here because there is nowhere better. ---------------------------------

        bool rom_verified() const noexcept { return romVerified_; }
        void set_rom_verified(bool value);

        int window_width() const noexcept { return windowWidth_; }
        int window_height() const noexcept { return windowHeight_; }
        bool window_maximized() const noexcept { return windowMaximized_; }
        void set_window_geometry(int width, int height, bool maximized);

        /// The whole engine struct at once, so N changed keys cost one rebuild rather than N.
        [[nodiscard]] TSEngineSettings engine_settings() const;

        /// Raised after a key has been written, with the key's name. Set once by whoever owns the
        /// store.
        std::function<void(const std::string& key)> changed;

    private:
        void write(const std::string& key, int value);
        void write(const std::string& key, double value);
        void write(const std::string& key, bool value);

        int map_;
        int polyphony_;
        int ports_;
        bool extendedInterpolation_;
        bool flushBeforeSysex_;
        bool reverb_;
        bool chorus_;
        bool delay_;
        bool efx_;
        double outputGain_;
        int latencyMs_;
        bool looping_;
        bool romVerified_;
        int windowWidth_;
        int windowHeight_;
        bool windowMaximized_;
    };
}
