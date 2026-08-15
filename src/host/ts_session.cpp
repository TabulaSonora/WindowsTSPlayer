#include "host/ts_session.hpp"

#include "host/ts_i18n.hpp"

#include "tabulasonora/engine_noise.hpp"
#include "tabulasonora/patch_directory.hpp"
#include "tabulasonora/sequence.hpp"
#include "tabulasonora/wav_writer.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <stdexcept>

extern "C" TSEngineSettings TSEngineSettingsDefault(void)
{
    const ts::ToneGeneratorOptions defaults;
    TSEngineSettings settings;
    settings.map = static_cast<TSToneMap>(defaults.map);
    settings.polyphony = defaults.polyphony;
    settings.ports = defaults.ports;
    settings.reverb = defaults.reverb;
    settings.chorus = defaults.chorus;
    settings.delay = defaults.delay;
    settings.efx = defaults.efx;
    settings.extendedInterpolation = defaults.extended_interpolation;
    settings.flushBeforeSysex = defaults.flush_before_sysex;
    settings.outputGain = defaults.output_gain;
    return settings;
}

namespace ts::host {

namespace {

std::string trimmed(std::string_view text)
{
    const auto begin = text.find_first_not_of(" \t");
    if (begin == std::string_view::npos) {
        return {};
    }
    const auto end = text.find_last_not_of(" \t");
    return std::string{text.substr(begin, end - begin + 1)};
}

std::string file_name(const std::string& path)
{
    // Both separators, rather than the platform's own under an #ifdef. A path reaching this layer
    // can come from a file picker, a shell file association or a command line, and only the first
    // is reliably backslashed on Windows -- the other two hand over whatever the caller typed.
    //
    // This is not cosmetics. The result feeds `smf::load` below for format detection, and LDS has
    // no magic number and is recognised by its extension alone, so a name left as the whole
    // "C:\songs\track.lds" is an unplayable file rather than an ugly window title.
    //
    // Not std::filesystem::path::filename(), whose .string() narrows through the active code page
    // and would undo the UTF-8 the manifest asks for.
    const auto slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

} // namespace

void Session::load_rom(const std::string& path, bool verify_fully)
{
    // Built before anything is torn down, so a bad file leaves the running engine alone.
    auto opened = RomImage::open(
        path, verify_fully ? RomVerification::full : RomVerification::quick);

    unload_rom();

    rom_.emplace(std::move(opened));
    notes_.emplace(*rom_);
    rom_name_ = file_name(path);
    rebuild();
}

void Session::unload_rom()
{
    // Reverse of the order they were built in: each borrows the one above it, and a player left
    // holding a pointer into a destroyed generator is the documented way to get this wrong.
    player_.reset();
    engine_.reset();
    notes_.reset();
    rom_.reset();
    rom_name_.clear();
}

void Session::load_song(const std::string& path)
{
    std::ifstream file{path, std::ios::binary};
    if (!file) {
        // The message reaches the user: TsPlayerModel turns this into a GError and the window
        // shows it as the body of an alert. Formatted whole rather than concatenated around the
        // name, so a translator can put the quotes and the name where their language wants them.
        /* TRANSLATORS: %s is the name of the file that could not be opened. */
        throw std::runtime_error(format_text(TS_("Cannot read '%s'."), file_name(path).c_str()));
    }

    const std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>{file},
                                          std::istreambuf_iterator<char>{}};

    // The name rides along for format detection: LDS has no magic and is recognised by it.
    smf::Song parsed = smf::load(bytes, sample_rate, file_name(path));
    auto events = std::move(parsed.events);

    // Which channel addresses the file sends on. The port is folded exactly as the engine folds it,
    // because a file tagged for four ports playing on a two-port engine lands its port C traffic on
    // port A.
    //
    // Deliberately not "which parts the file touches": a part is reached by matching its receive
    // channel, not by being indexed with it, so the two are the same question only until something
    // moves a part. `capture` turns these channels into strips by asking the engine what is
    // listening to each of them.
    const int ports = std::max(1, settings_.ports);
    used_channels_.fill(false);
    for (const auto& message : events) {
        if (message.kind != MidiEventKind::channel) {
            continue;
        }
        const int address =
            ((message.port & (ports - 1)) * Sequence::channel_count) + message.channel();
        if (address >= 0 && address < TS_MAX_PARTS) {
            used_channels_[static_cast<std::size_t>(address)] = true;
        }
    }

    song_length_ = events.empty() ? 0 : events.back().position;
    song_name_ = file_name(path);
    song_events_ = std::move(events);
    song_loop_ = parsed.loop;

    // Kept beside the events because `arm_player` rebuilds the `smf::Song` from these three fields
    // rather than holding the parsed one, and a `Song` assembled without this has `first_note`
    // zero -- which compiles, and quietly turns `skip_lead_in` into a no-op.
    song_first_note_ = parsed.first_note;

    // Taken from the same bytes, after the parse rather than before it: `smf::load` is what decides
    // whether this file is playable at all, and a file it throws on should not leave a half-filled
    // information window behind describing something that never loaded.
    song_info_ = read_song_info(bytes, file_name(path), sample_rate);
    song_info_.length = song_length_;
    if (song_loop_) {
        song_info_.has_loop = true;
        song_info_.loop_start = song_loop_->start;
        song_info_.loop_end = song_loop_->end;
        song_info_.loop_soft = song_loop_->soft;
    }

    if (engine_) {
        engine_->reset();
        arm_player();
    }
}

void Session::unload_song()
{
    player_.reset();
    song_events_.clear();
    song_name_.clear();
    song_length_ = 0;
    song_loop_.reset();
    song_first_note_ = 0;
    song_info_ = {};
    used_channels_.fill(false);
    if (engine_) {
        engine_->reset();
    }
}

void Session::set_settings(const TSEngineSettings& settings)
{
    // Gain is live; everything else lives in the construction options and needs a new generator.
    //
    // Extended interpolation belongs in this list even though nothing about the generator's *shape*
    // changes with it: `ToneGenerator` copies it out of its options into each voice's setup at
    // note-on and offers no setter, so a session that only stored the new value would go on
    // sounding the old kernel for as long as the generator lived -- the toggle would appear to do
    // nothing until some other setting happened to rebuild, and then take effect retroactively.
    //
    // Flushing before SysEx is here for a weaker reason than the rest: the generator reads it out
    // of its stored options on every message rather than latching it, so it would take effect on
    // its own -- but nothing hands a generator new options without constructing one, so a rebuild
    // is the only way this session has to deliver the change at all.
    const bool structural = settings.map != settings_.map
                            || settings.polyphony != settings_.polyphony
                            || settings.ports != settings_.ports || settings.reverb != settings_.reverb
                            || settings.chorus != settings_.chorus || settings.delay != settings_.delay
                            || settings.efx != settings_.efx
                            || settings.extendedInterpolation != settings_.extendedInterpolation
                            || settings.flushBeforeSysex != settings_.flushBeforeSysex;

    settings_ = settings;

    if (!engine_) {
        return;
    }

    if (structural) {
        rebuild();
    } else {
        engine_->set_output_gain(settings_.outputGain);
    }
}

void Session::set_looping(bool looping)
{
    looping_ = looping;
    if (player_) {
        player_->set_loop_count(looping ? -1 : 1);
    }
}

void Session::seek(std::int64_t frame)
{
    if (player_) {
        player_->seek(std::max<std::int64_t>(0, frame));
    }
}

std::int64_t Session::position() const noexcept
{
    if (player_) {
        return player_->position();
    }
    if (engine_) {
        return engine_->position();
    }
    return 0;
}

bool Session::complete() const noexcept
{
    // A looping song has no completion: the position wraps rather than passing the end. Saying so
    // here is also what lets Play restart a song that finished before looping was turned on.
    if (looping_) {
        return false;
    }
    return player_.has_value()
           && player_->position()
                  >= song_length_ + static_cast<std::int64_t>(tail_seconds * sample_rate);
}

void Session::panic()
{
    if (engine_) {
        engine_->reset();
    }
}

void Session::silence()
{
    if (!engine_) {
        return;
    }

    constexpr int all_sound_off = 120;

    const int ports = std::max(1, settings_.ports);
    for (int port = 0; port < ports; ++port) {
        for (int channel = 0; channel < Sequence::channel_count; ++channel) {
            send_control(port, channel, all_sound_off, 0);
        }
    }
}

void Session::render(std::span<float> left, std::span<float> right)
{
    if (player_) {
        player_->render(left, right);
        return;
    }
    if (engine_) {
        engine_->render(left, right);
        return;
    }

    // The caller reuses its block, so leaving it untouched would queue the previous one again --
    // a stutter rather than the silence that is meant.
    std::fill(left.begin(), left.end(), 0.0F);
    std::fill(right.begin(), right.end(), 0.0F);
}

void Session::render_live(std::span<float> left, std::span<float> right)
{
    if (engine_) {
        engine_->render(left, right);
        return;
    }

    std::fill(left.begin(), left.end(), 0.0F);
    std::fill(right.begin(), right.end(), 0.0F);
}

int Session::active_voices() const noexcept
{
    return engine_ ? engine_->active_voices() : 0;
}

void Session::send_channel(int port, int status, int data1, int data2)
{
    if (engine_) {
        engine_->send_channel(port, status, data1, data2);
    }
}

void Session::send_control(int port, int channel, int controller, int value)
{
    if (engine_) {
        engine_->send_channel(port, 0xB0 | (channel & 0x0F), controller, value);
    }
}

void Session::capture(SessionSnapshot& into) const
{
    into.hasROM = has_rom();
    into.hasSong = has_song();
    into.looping = looping_;
    into.complete = complete();
    into.position = position();
    into.length = song_length_;

    // Cleared rather than overwritten: a rebuild with fewer ports leaves strips behind otherwise,
    // and they would go on showing whatever the wider engine last had on them.
    into.parts = {};

    if (!engine_) {
        into.partCount = Sequence::channel_count;
        into.activeVoices = 0;
        into.voiceCapacity = 0;
        into.noteCount = 0;
        into.drumKit = -1;
        into.xgMode = false;
        return;
    }

    into.activeVoices = engine_->active_voices();
    into.voiceCapacity = engine_->voice_slots();
    into.noteCount = engine_->note_count();
    into.drumKit = engine_->drum_kit();
    into.xgMode = engine_->xg_mode();

    // The engine's own part count, not the mask's: `ChannelMask` is 64 wide because a four-port
    // engine has that many parts to mute, but asking this engine for one past its own reads memory
    // that is not a part.
    into.partCount = engine_->parts();

    // Per-part activity from the pool itself; there is no per-part counter, and a walk over at
    // most 64 handles ten times a second costs nothing.
    std::array<int, TS_MAX_PARTS> counts{};
    for (const auto& voice : engine_->voices().active()) {
        const int part = voice.channel();
        if (part >= 0 && part < TS_MAX_PARTS) {
            ++counts[static_cast<std::size_t>(part)];
        }
    }

    for (int index = 0; index < into.partCount && index < TS_MAX_PARTS; ++index) {
        const Part& part = engine_->part(index);
        PartState& state = into.parts[static_cast<std::size_t>(index)];

        // Asked of the engine per part, not taken from the settings: a bank LSB names a vintage and
        // XG System On moves every part onto the XG map, so one map for the whole mixer names the
        // wrong instrument as soon as a file switches mode. Drum routing moves the same way.
        const bool drums = engine_->part_is_drum(index);
        const int kit = engine_->part_drum_kit(index);

        state.program = part.program;
        state.bank = part.bank;
        state.bankLsb = part.bank_lsb;
        state.volume = part.volume();
        state.expression = part.expression();
        state.pan = part.pan;
        state.voices = counts[static_cast<std::size_t>(index)];
        state.muted = channels_.is_muted(index);
        state.soloed = channels_.is_soloed(index);
        state.drums = drums;
        state.kit = kit;
        state.rxChannel = engine_->part_rx_channel(index);

        // Whether the file reaches this part, asked through the channel the part is *listening on*
        // rather than through its slot -- the same walk the engine does to deliver a message, which
        // is what makes this the question "will this strip make a sound".
        //
        // Matching is within a port, so the file's channel is looked up on this part's own port. A
        // receive channel outside 0-15 is the module's "off", and a part detached from every
        // channel hears nothing however much the file sends.
        const int port = index / Sequence::channel_count;
        state.present =
            state.rxChannel >= 0 && state.rxChannel < Sequence::channel_count
            && used_channels_[static_cast<std::size_t>(port * Sequence::channel_count
                                                       + state.rxChannel)];
        state.map = static_cast<int>(engine_->part_tone_map(index));
        state.lookupBank = engine_->part_lookup_bank(index);

        state.name.clear();
        if (notes_) {
            if (drums) {
                state.name = trimmed(notes_->drums().kit_name(kit));
                if (state.name.empty() && kit >= 0) {
                    /* TRANSLATORS: %d is a GS drum kit number, used when the module gives the
                       kit no name of its own. */
                    state.name = format_text(TS_("Kit %d"), kit);
                }
            } else {
                const auto& directory = notes_->directory();
                const int tone = directory.program_to_tone(
                    part.program, engine_->part_tone_map(index), engine_->part_lookup_bank(index));
                if (tone >= 0) {
                    if (auto record = directory.tone(tone); record && record->is_defined()) {
                        state.name = trimmed(record->name());
                    }
                }
            }
        }
    }
}

Session::ExportPlan Session::plan_export() const
{
    if (!notes_ || song_events_.empty()) {
        throw std::runtime_error(TS_("Load a DLL and a song before exporting."));
    }

    ExportPlan plan;
    // const_cast because a plan borrows the renderer to build a generator over it, which is a
    // non-const use of an object this session otherwise only reads here. The renderer is owned by
    // the session and outlives the plan by contract.
    plan.notes = const_cast<NoteRenderer*>(&*notes_);
    plan.events = song_events_;
    plan.loop = song_loop_;
    plan.options = options();
    plan.total = song_length_ + static_cast<std::int64_t>(tail_seconds * sample_rate);
    return plan;
}

void Session::run_export(const ExportPlan& plan, const std::string& path,
                         const std::function<bool(double)>& progress)
{
    // A second generator over the same note renderer, so exporting disturbs nothing that is playing
    // and costs no second read of the 27 MB of tables.
    //
    // The pseudo-random source is the one thing the two generators genuinely share: it lives on the
    // note renderer because pitch jitter, random pan and the random LFO shapes all draw from it, and
    // its sequence is deterministic *from engine reset*. A generator that has been built, armed, or
    // played has already moved it, so an export starting from wherever it happened to be would
    // differ from a fresh render of the same file -- and from itself, run twice. Resetting is what
    // makes an export reproducible and byte-identical to `tabula-sonora render`.
    //
    // The cost is that exporting while playing perturbs the live random stream, and an export taken
    // during playback interleaves draws with it, so only a quiet transport can promise those exact
    // bytes. Both renditions are correct; they differ in which random pan each note draws.
    plan.notes->noise().reset();

    ToneGenerator engine{*plan.notes, plan.options};

    // No `first_note`, no `skip_lead_in`, no `set_spread_bursts` -- and the omission is the point,
    // so it is written down rather than left to be inferred from three absent calls.
    //
    // An export is data, not a performance. Its length and its alignment are what a comparison
    // against a reference render rests on, and `tabula-sonora render` leaves all three off unless
    // asked for them by flag. Skipping the lead-in here would shorten the file by however much
    // silence it began with; spreading a burst would move the audio by as much as 2.31 dB on a
    // file that has one. Either would take `export-matches-cli` from a byte comparison to a
    // near-enough one, which is the whole of what that test is for.
    //
    // So a song exported from this program does not sound like the same song played by it, on a
    // file with an opening burst. That is the intended difference, and it is upstream's: the
    // players stand in for a cable, and a render answers to the reference.
    SequencePlayer player{engine, smf::Song{plan.events, plan.loop}};

    // One pass, not a chunked loop, and this is not a stylistic choice.
    //
    // The length of a render call is part of the engine's event timing. `SequencePlayer::render`
    // hands the generator every event due within the span it was given, stamped with the offset it
    // falls at, and the engine rounds that stamp to the millisecond -- so the span boundaries decide
    // where events land. Rendering a song a quarter-second at a time and rendering it in one call
    // produce genuinely different audio, and it was the chunked form that disagreed with
    // `tabula-sonora render`. `render_to_end` is the same call the CLI makes, which is what makes
    // an export byte-identical to the reference rather than merely close to it.
    //
    // The cost is that there is nothing to report from inside it: progress is announced at the ends
    // only, and a render already under way cannot be interrupted.
    if (!progress(0.0)) {
        return;
    }

    const RenderResult rendered = player.render_to_end(tail_seconds);

    if (!progress(1.0)) {
        return;
    }

    // Through the library's own writer, so an export from here and a `tabula-sonora render` with
    // the same settings are the same bytes, not merely similar ones.
    wav::write(path, rendered.left, rendered.right, sample_rate);
}

ToneGeneratorOptions Session::options() const
{
    ToneGeneratorOptions options;
    options.map = static_cast<ToneMap>(settings_.map);
    options.polyphony = settings_.polyphony;
    options.ports = settings_.ports;
    options.reverb = settings_.reverb;
    options.chorus = settings_.chorus;
    options.delay = settings_.delay;
    options.efx = settings_.efx;
    options.extended_interpolation = settings_.extendedInterpolation;
    options.flush_before_sysex = settings_.flushBeforeSysex;
    options.output_gain = settings_.outputGain;
    options.channels = &channels_;

    // The module's own timing, and deliberately not settings.
    //
    // Neither of these is an option on the module: `TG_ShortMidiIn` only rings a message and
    // `TG_Process` walks it out four 32-sample chunks later, always, and `tg_output_filter` runs on
    // every chunk it emits. The engine defaults them the other way because its own unit tests send
    // a message and inspect a part on the next line, which is a convenience for testing *laws* and
    // the wrong setting for rendering a *song*.
    //
    // Left unset, this program started every note 128 samples early and skipped a stage the
    // hardware always runs -- a fidelity bug in a program whose whole purpose is the hardware's
    // voice. It also stopped being comparable: `tabula-sonora render` sets both unconditionally as
    // of engine fa3c9a6a, with no flag to ask for the old behaviour, so `export-matches-cli` is a
    // byte comparison against a differently-timed engine unless this matches it.
    //
    // The cost is four milliseconds of latency on live MIDI, which the module also has, against a
    // uniform shift that is inaudible in playback.
    options.event_delay_blocks = 4;
    options.bypass_output_filter = false;

    return options;
}

void Session::rebuild()
{
    const std::int64_t position = player_ ? player_->position() : 0;

    // A rebuild makes fresh parts at their power-on values; capture the outgoing ones first, or a
    // vintage change would silently reset every part to piano.
    std::vector<std::array<int, 7>> previous;
    if (engine_) {
        previous.reserve(static_cast<std::size_t>(engine_->parts()));
        for (int index = 0; index < engine_->parts(); ++index) {
            const Part& part = engine_->part(index);
            previous.push_back({part.bank, part.program, part.volume(), part.pan, part.expression(),
                                part.reverb_send, part.chorus_send});
        }
    }

    // The player holds a pointer into the outgoing engine, so it goes first.
    player_.reset();
    engine_.emplace(*notes_, options());

    if (!previous.empty()) {
        restore_parts(previous);
    }

    if (!song_events_.empty()) {
        arm_player();

        // Put the new generator back where the old one was, so changing vintage mid-song resumes
        // rather than restarting. The seek replays the controllers, which is what makes that sound
        // right.
        if (position > 0) {
            player_->seek(position);
        }
    }
}

void Session::arm_player()
{
    player_.emplace(*engine_, smf::Song{song_events_, song_loop_, song_first_note_});
    player_->set_loop_count(looping_ ? -1 : 1);

    // The two things that make this a player rather than a renderer, both of them upstream's own
    // choice for `apps/audio` and deliberately *not* taken on the export path below.
    //
    // Start where the music does. A file that opens with a bar of bank selects and controllers is
    // silence until its first note, and a listener waiting through it cannot tell that from a
    // broken file or a dead output. Nothing is lost by skipping: `skip_lead_in` goes through
    // `seek`, so every controller, bank select and SysEx in the lead-in is still replayed into the
    // engine, and the only events dropped are notes, of which there are none before the first one.
    player_->skip_lead_in();

    // And hand a dense opening over at a cable's rate. The engine drops whatever runs past its
    // input queue's 2,048 packets in one control tick, faithfully -- that is what the module does
    // to a host that dumps a burst on it -- but a host is not what a player is standing in for. A
    // wire is, and at 31,250 baud `darkness3.mid`'s 660-byte opening takes twenty-one control
    // ticks to arrive and hardware drops none of it. Handing it over in one call is this program's
    // choice, not the file's; upstream measured the difference at 2.31 dB, because without this
    // the parts keep the bulk dump's programs instead of taking the file's own.
    player_->set_spread_bursts(true);
}

void Session::restore_parts(const std::vector<std::array<int, 7>>& previous)
{
    for (int index = 0; index < static_cast<int>(previous.size()); ++index) {
        const auto& [bank, program, volume, pan, expression, reverb, chorus] =
            previous[static_cast<std::size_t>(index)];

        // A part index is not a channel. `port * 16 + channel` has to come apart again before it
        // can go back out as MIDI: a status byte carries four bits of channel and the port travels
        // beside it, so `0xC0 | 16` is not part 17's program change.
        const int port = index / Sequence::channel_count;
        const int channel = index % Sequence::channel_count;

        // Bank before program, as anything selecting a sound must: the program change latches the
        // pair, and it is what brings a drum kit back with it.
        send_control(port, channel, 0, bank);
        engine_->send_channel(port, 0xC0 | channel, program, 0);

        send_control(port, channel, 7, volume);
        send_control(port, channel, 10, pan);
        send_control(port, channel, 11, expression);
        send_control(port, channel, 91, reverb);
        send_control(port, channel, 93, chorus);
    }
}

} // namespace ts::host
