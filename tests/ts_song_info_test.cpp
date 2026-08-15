// What a file says about itself, over files built here rather than files on disk.
//
// Needs no ROM and no corpus: every case is a Standard MIDI File assembled byte by byte below, so
// the test states its own inputs and a checkout with neither can still run it. That matters more
// here than elsewhere -- the cases worth pinning are the malformed and the unusual, and those are
// exactly the files nobody can be relied on to have.

#include "host/ts_song_info.hpp"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

using namespace ts::host;

namespace {

int failures = 0;

void check(bool condition, const std::string& what)
{
    if (!condition) {
        std::printf("FAIL: %s\n", what.c_str());
        ++failures;
    }
}

template <typename T>
void check_equal(const T& got, const T& wanted, const std::string& what)
{
    if (!(got == wanted)) {
        std::printf("FAIL: %s\n", what.c_str());
        ++failures;
    }
}

// -- Building files --------------------------------------------------------------------------------

using Bytes = std::vector<std::uint8_t>;

void append(Bytes& into, const Bytes& tail)
{
    into.insert(into.end(), tail.begin(), tail.end());
}

void append_text(Bytes& into, const std::string& text)
{
    into.insert(into.end(), text.begin(), text.end());
}

Bytes big_endian(std::uint32_t value, int width)
{
    Bytes out;
    for (int shift = (width - 1) * 8; shift >= 0; shift -= 8) {
        out.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFU));
    }
    return out;
}

/// A meta event at delta zero.
Bytes meta(std::uint8_t type, const std::string& payload)
{
    Bytes out{0x00, 0xFF, type, static_cast<std::uint8_t>(payload.size())};
    append_text(out, payload);
    return out;
}

Bytes meta_raw(std::uint8_t type, const Bytes& payload)
{
    Bytes out{0x00, 0xFF, type, static_cast<std::uint8_t>(payload.size())};
    append(out, payload);
    return out;
}

/// A system-exclusive message at delta zero, in the F0 form. `body` excludes the leading F0.
Bytes sysex(const Bytes& body)
{
    Bytes out{0x00, 0xF0, static_cast<std::uint8_t>(body.size())};
    append(out, body);
    return out;
}

/// The same message in the F7 form, which the engine's own reader discards.
Bytes sysex_f7(const Bytes& body)
{
    Bytes out{0x00, 0xF7, static_cast<std::uint8_t>(body.size() + 1), 0xF0};
    append(out, body);
    return out;
}

Bytes note_on(int channel, int note, int velocity)
{
    return {0x00, static_cast<std::uint8_t>(0x90 | channel), static_cast<std::uint8_t>(note),
            static_cast<std::uint8_t>(velocity)};
}

Bytes bank_lsb(int channel, int value)
{
    return {0x00, static_cast<std::uint8_t>(0xB0 | channel), 0x20,
            static_cast<std::uint8_t>(value)};
}

Bytes track(const Bytes& events)
{
    Bytes body = events;
    append(body, Bytes{0x00, 0xFF, 0x2F, 0x00}); // End of Track

    Bytes out;
    append_text(out, "MTrk");
    append(out, big_endian(static_cast<std::uint32_t>(body.size()), 4));
    append(out, body);
    return out;
}

Bytes file(int format, const std::vector<Bytes>& tracks, int division = 480)
{
    Bytes out;
    append_text(out, "MThd");
    append(out, big_endian(6, 4));
    append(out, big_endian(static_cast<std::uint32_t>(format), 2));
    append(out, big_endian(static_cast<std::uint32_t>(tracks.size()), 2));
    append(out, big_endian(static_cast<std::uint32_t>(division), 2));
    for (const Bytes& one : tracks) {
        append(out, one);
    }
    return out;
}

SongInfo read(const Bytes& bytes, const std::string& name = "test.mid")
{
    return read_song_info(bytes, name);
}

std::vector<std::string> marker_texts(const SongInfo& info)
{
    std::vector<std::string> texts;
    texts.reserve(info.markers.size());
    for (const auto& marker : info.markers) {
        texts.push_back(marker.text);
    }
    return texts;
}

/// A delta time as a variable-length quantity: seven bits per byte, high bit set on all but the
/// last. Anything from 128 up needs more than one byte, and 480 ticks -- a quarter note at the
/// usual division -- is already over it.
Bytes variable_length(int value)
{
    Bytes reversed;
    reversed.push_back(static_cast<std::uint8_t>(value & 0x7F));
    value >>= 7;
    while (value > 0) {
        reversed.push_back(static_cast<std::uint8_t>((value & 0x7F) | 0x80));
        value >>= 7;
    }
    return Bytes{reversed.rbegin(), reversed.rend()};
}

/// A meta event at a given delta, for the cases where *when* is the thing under test.
Bytes meta_at(int delta, std::uint8_t type, const std::string& payload)
{
    Bytes out = variable_length(delta);
    append(out, Bytes{0xFF, type, static_cast<std::uint8_t>(payload.size())});
    append_text(out, payload);
    return out;
}

// -- The cases ---------------------------------------------------------------------------------

void test_names_and_text()
{
    Bytes events;
    append(events, meta(0x03, "Lead Guitar"));
    append(events, meta(0x04, "Overdrive"));
    append(events, meta(0x02, "Copyright 1994 Somebody"));
    append(events, meta(0x01, "Arranged by nobody in particular"));
    append(events, note_on(3, 60, 100));
    append(events, note_on(3, 64, 100));
    append(events, note_on(3, 67, 0)); // A note-off in disguise; must not be counted.

    const SongInfo info = read(file(1, {track(events)}));

    check(info.valid, "a well-formed file is valid");
    check_equal(info.track_count, 1, "one track counted");
    check_equal(info.format, 1, "format read from the header");
    check_equal(info.division, 480, "division read from the header");
    check_equal(info.tracks.at(0).name, std::string{"Lead Guitar"}, "FF 03 track name");
    check_equal(info.tracks.at(0).instrument, std::string{"Overdrive"}, "FF 04 instrument name");
    check_equal(info.copyright, std::string{"Copyright 1994 Somebody"}, "FF 02 copyright");
    check_equal(info.text.size(), std::size_t{1}, "one text line");
    check_equal(info.text.at(0), std::string{"Arranged by nobody in particular"}, "FF 01 text");
    check_equal(info.tracks.at(0).notes, 2, "a zero-velocity note-on is not a note");
    check_equal(info.tracks.at(0).channels, std::vector<int>{3}, "the channel the track plays on");
}

void test_text_is_deduplicated()
{
    // Files commonly stamp the same credit on every track. Seventeen copies is not information.
    const Bytes one = track([] {
        Bytes events;
        append(events, meta(0x01, "www.example.com"));
        return events;
    }());
    const SongInfo info = read(file(1, {one, one, one}));

    check_equal(info.text.size(), std::size_t{1}, "repeated text collapses to one line");
    check_equal(info.track_count, 3, "but the tracks are all still counted");
}

void test_running_status()
{
    // Three note-ons sharing one status byte, which is how most files in the wild are written. A
    // parser that mishandles this reads the data bytes as statuses and produces garbage.
    Bytes events{0x00, 0x92, 0x3C, 0x64, // note on, channel 2, explicit status
                 0x00, 0x3E, 0x64,       // running status
                 0x00, 0x40, 0x64};      // running status
    append(events, meta(0x03, "Runner"));

    const SongInfo info = read(file(0, {track(events)}));

    check_equal(info.tracks.at(0).notes, 3, "three notes under running status");
    check_equal(info.tracks.at(0).channels, std::vector<int>{2}, "channel survives running status");
    check_equal(info.tracks.at(0).name, std::string{"Runner"}, "the event after them still parses");
}

void test_running_status_survives_a_meta_event()
{
    // The spec says a system message clears the running status. Sequencers disagree, and the
    // engine's own reader keeps it for exactly that reason -- clearing costs those files most of
    // their notes. This pins the same behaviour here, so the two readers agree about what a track
    // contains.
    Bytes events{0x00, 0x91, 0x3C, 0x64};
    append(events, meta(0x06, "Verse 1"));
    append(events, Bytes{0x00, 0x3E, 0x64}); // still running status, after the meta
    append(events, Bytes{0x00, 0x40, 0x64});

    const SongInfo info = read(file(1, {track(events)}));

    check_equal(info.tracks.at(0).notes, 3, "running status resumes across a meta event");
    check_equal(marker_texts(info), (std::vector<std::string>{"Verse 1"}),
                "and the meta was still read");
}

void test_meta_status_never_becomes_running()
{
    // The other half: a meta event must not *become* the running status. If it did, the note after
    // it would be read as another meta and its length byte swallowed as data, losing the rest.
    Bytes events;
    append(events, meta(0x03, "Named"));
    append(events, Bytes{0x00, 0x93, 0x3C, 0x64});
    append(events, Bytes{0x00, 0x3E, 0x64});

    const SongInfo info = read(file(1, {track(events)}));

    check_equal(info.tracks.at(0).notes, 2, "a meta event did not become the running status");
    check_equal(info.tracks.at(0).channels, std::vector<int>{3}, "the channel is the note's own");
}

void test_tempo_and_signatures()
{
    Bytes events;
    append(events, meta_raw(0x51, {0x07, 0xA1, 0x20})); // 500000 us -> 120 bpm
    append(events, meta_raw(0x58, {0x06, 0x03, 0x18, 0x08}));
    append(events, meta_raw(0x59, {0xFD, 0x01})); // three flats, minor -> C minor
    append(events, meta_raw(0x51, {0x05, 0x16, 0x15})); // a later change

    const SongInfo info = read(file(1, {track(events)}));

    check(info.initial_tempo_bpm > 119.9 && info.initial_tempo_bpm < 120.1, "120 bpm");
    check_equal(info.tempo_changes, 2, "both tempo events counted");
    check_equal(info.time_signature, std::string{"6/8"}, "time signature");
    check_equal(info.key_signature, std::string{"C minor"}, "key signature");
}

void test_lyrics_from_lyric_meta()
{
    Bytes events;
    append(events, meta(0x05, "Hap"));
    append(events, meta(0x05, "py "));
    append(events, meta(0x05, "birth"));
    append(events, meta(0x05, "day"));
    append(events, meta(0x05, "/to you"));

    const SongInfo info = read(file(1, {track(events)}));

    check_equal(info.lyrics, std::string{"Happy birthday\nto you"}, "FF 05 syllables join up");
}

void test_soft_karaoke()
{
    // The other dialect: lyrics in FF 01 Text, marked out by the @ headers rather than by the type.
    Bytes header;
    append(header, meta(0x03, "Soft Karaoke"));
    append(header, meta(0x01, "@KMIDI KARAOKE FILE"));
    append(header, meta(0x01, "@V0100"));

    Bytes words;
    append(words, meta(0x03, "Words"));
    append(words, meta(0x01, "@LENGL"));
    append(words, meta(0x01, "@TMy Song"));
    append(words, meta(0x01, "@TSome Author"));
    append(words, meta(0x01, "\\Once"));
    append(words, meta(0x01, " upon"));
    append(words, meta(0x01, " a time"));
    append(words, meta(0x01, "/and then"));

    const SongInfo info = read(file(1, {track(header), track(words)}));

    check_equal(info.lyrics, std::string{"Once upon a time\nand then"}, "karaoke syllables join up");
    check_equal(info.karaoke_headings.size(), std::size_t{2}, "the @T headings are kept");
    check_equal(info.karaoke_headings.at(0), std::string{"My Song"}, "first @T heading");
    check(info.text.empty(), "karaoke text is not also listed as prose");
}

void test_markers_exclude_loop_keywords()
{
    Bytes events;
    append(events, meta(0x06, "Verse 1"));
    append(events, meta(0x06, "loopStart"));
    append(events, meta(0x06, "Chorus"));
    append(events, meta(0x06, "loopEnd"));
    append(events, meta(0x06, "Verse 1")); // repeated

    const SongInfo info = read(file(1, {track(events)}));

    check_equal(marker_texts(info), (std::vector<std::string>{"Verse 1", "Chorus"}),
                "loop markers are the loop, not markers");
}

void test_lyrics_written_as_markers()
{
    // A third karaoke dialect: the words in FF 06 Marker, one syllable at a time, keeping Soft
    // Karaoke's line breaks. Read as section markers it is hundreds of one-word rows and a lyrics
    // page claiming the file has none.
    Bytes events;
    for (const char* piece : {"/For", " all", " those", " times", "/you", " stood", " by", " me",
                              "/and", " all", " the", " truth", "/that", " you", " made", " me",
                              " see"}) {
        append(events, meta_at(60, 0x06, piece));
    }

    const SongInfo info = read(file(1, {track(events)}, 480));

    check_equal(info.lyrics,
                std::string{"For all those times\nyou stood by me\nand all the truth\nthat you "
                            "made me see"},
                "markers carrying the words become the lyric sheet");
    check(info.markers.empty(), "and are not also listed as places to jump to");
}

void test_section_markers_are_not_mistaken_for_lyrics()
{
    // The other side of that test, and the one with more at stake: a real section list must survive
    // however long it is. Nothing about a marker's length or wording is evidence -- only the line
    // break characters are.
    Bytes events;
    for (const char* piece : {"Prelude", "Verse 1", "Interlude", "Verse 2", "CHORUS 1", "Verse 3",
                              "Interlude 2", "CHORUS 2", "Bridge", "Verse 4", "CHORUS 3", "Coda"}) {
        append(events, meta_at(60, 0x06, piece));
    }

    const SongInfo info = read(file(1, {track(events)}, 480));

    check_equal(info.markers.size(), std::size_t{12}, "a long section list stays a section list");
    check(info.lyrics.empty(), "and is not read as a lyric sheet");
}

void test_marker_positions()
{
    // 480 ticks per quarter at 120 bpm is half a second a quarter note, so a marker two quarters in
    // falls at one second: 32,000 frames.
    Bytes events;
    append(events, meta_raw(0x51, {0x07, 0xA1, 0x20})); // 500000 us -> 120 bpm
    append(events, meta_at(0, 0x06, "Top"));
    append(events, meta_at(120, 0x06, "Quarter"));   // +0.125 s
    append(events, meta_at(840, 0x06, "Two Bars"));  // +0.875 s, so 1.0 s in total

    const SongInfo info = read(file(1, {track(events)}, 480));

    check_equal(info.markers.size(), std::size_t{3}, "three markers");
    check_equal(info.markers.at(0).position, std::int64_t{0}, "the first is at the top");
    check_equal(info.markers.at(1).position, std::int64_t{4000}, "an eighth of a second in");
    check_equal(info.markers.at(2).position, std::int64_t{32000}, "one second in");
}

void test_marker_positions_follow_a_tempo_change()
{
    // The reason ticks cannot simply be scaled: the file halves its tempo partway, so the second
    // marker is twice as far away in time as its tick distance suggests. 480 ticks at 120 bpm is
    // 0.5 s; the next 480 at 60 bpm is 1.0 s.
    Bytes events;
    append(events, meta_raw(0x51, {0x07, 0xA1, 0x20}));       // 120 bpm
    append(events, meta_at(480, 0x51, std::string("\x0f\x42\x40", 3))); // 1000000 us -> 60 bpm
    append(events, meta_at(0, 0x06, "Halved"));
    append(events, meta_at(480, 0x06, "After"));

    const SongInfo info = read(file(1, {track(events)}, 480));

    check_equal(info.markers.at(0).position, std::int64_t{16000}, "half a second at 120 bpm");
    check_equal(info.markers.at(1).position, std::int64_t{48000}, "a further second at 60 bpm");
}

void test_markers_from_several_tracks_merge_in_order()
{
    // A conductor track carrying the tempo map, stored *after* the track carrying the markers --
    // which is why the conversion cannot happen as the walk goes.
    Bytes marks;
    append(marks, meta_at(480, 0x06, "Second"));
    append(marks, meta_at(480, 0x06, "Third"));

    Bytes conductor;
    append(conductor, meta_raw(0x51, {0x07, 0xA1, 0x20}));
    append(conductor, meta_at(0, 0x06, "First"));

    const SongInfo info = read(file(1, {track(marks), track(conductor)}, 480));

    check_equal(marker_texts(info), (std::vector<std::string>{"First", "Second", "Third"}),
                "markers from every track, ordered by where they fall");
    check_equal(info.markers.at(1).position, std::int64_t{16000}, "against the later tempo map");
}

void test_repeated_marker_text_is_kept_when_it_moves()
{
    // Two tracks stamping the same marker on the same tick is one place; the same words later is
    // another, and collapsing by text alone would lose it.
    Bytes one;
    append(one, meta_at(0, 0x06, "Chorus"));
    append(one, meta_at(480, 0x06, "Bridge"));
    append(one, meta_at(480, 0x06, "Chorus"));

    Bytes two;
    append(two, meta_at(0, 0x06, "Chorus")); // the same place, from another track

    const SongInfo info = read(file(1, {track(one), track(two)}, 480));

    check_equal(marker_texts(info), (std::vector<std::string>{"Chorus", "Bridge", "Chorus"}),
                "an exact repeat is dropped, a later repeat is not");
}

// -- Vintage -------------------------------------------------------------------------------------

const Bytes gs_reset{0x41, 0x10, 0x42, 0x12, 0x40, 0x00, 0x7F, 0x00, 0x41, 0xF7};
const Bytes system_mode_set{0x41, 0x10, 0x42, 0x12, 0x00, 0x00, 0x7F, 0x00, 0x01, 0xF7};
const Bytes xg_on{0x43, 0x10, 0x4C, 0x00, 0x00, 0x7E, 0x00, 0xF7};
const Bytes gm_on{0x7E, 0x7F, 0x09, 0x01, 0xF7};
const Bytes gm2_on{0x7E, 0x7F, 0x09, 0x03, 0xF7};

void test_vintage()
{
    {
        const SongInfo info = read(file(1, {track(sysex(gs_reset))}));
        check_equal(info.vintage, SongVintage::sc55, "a GS Reset alone is an SC-55");
        check(!info.vintage_evidence.empty(), "and it says why");
    }
    {
        // The SC-88 demo disks: System Mode Set, which no SC-55 has, and no map selector at all.
        const SongInfo info = read(file(1, {track(sysex(system_mode_set))}));
        check_equal(info.vintage, SongVintage::sc88, "System Mode Set floors at the SC-88");
    }
    {
        Bytes events;
        append(events, sysex(gs_reset));
        append(events, bank_lsb(0, 2));
        const SongInfo info = read(file(1, {track(events)}));
        check_equal(info.vintage, SongVintage::sc88, "a bank LSB of 2 raises a GS file to SC-88");
    }
    {
        Bytes events;
        append(events, sysex(system_mode_set));
        append(events, bank_lsb(0, 4));
        const SongInfo info = read(file(1, {track(events)}));
        check_equal(info.vintage, SongVintage::sc8820, "a bank LSB of 4 raises it further");
    }
    {
        // The floor never drops: an SC-88's mode set with an SC-55 map selected is still an SC-88
        // file, because the SC-55 could not have sent the mode set.
        Bytes events;
        append(events, sysex(system_mode_set));
        append(events, bank_lsb(0, 1));
        const SongInfo info = read(file(1, {track(events)}));
        check_equal(info.vintage, SongVintage::sc88, "a lower map selector does not lower the floor");
    }
    {
        const SongInfo info = read(file(1, {track(sysex(xg_on))}));
        check_equal(info.vintage, SongVintage::xg, "XG System On is XG");
    }
    {
        const SongInfo info = read(file(1, {track(sysex(gm_on))}));
        check_equal(info.vintage, SongVintage::gm, "GM System On names no vintage");
    }
    {
        const SongInfo info = read(file(1, {track(sysex(gm2_on))}));
        check_equal(info.vintage, SongVintage::gm2, "GM2 System On");
    }
    {
        // The case the engine documents itself as refusing to guess at: a bank LSB outside 1-4, in
        // a file that declared nothing. Reading it as a vintage would break every file that means
        // something else by it -- and the corpus has plenty using 46 and 127.
        Bytes events;
        append(events, bank_lsb(0, 46));
        append(events, bank_lsb(1, 127));
        const SongInfo info = read(file(1, {track(events)}));
        check_equal(info.vintage, SongVintage::unstated, "a bank LSB alone states nothing");
        check(info.vintage_evidence.empty(), "and there is no evidence to show");
    }
    {
        // A bank LSB in range but with no reset is still not a declaration.
        const SongInfo info = read(file(1, {track(bank_lsb(0, 3))}));
        check_equal(info.vintage, SongVintage::unstated, "in-range LSB without a reset states nothing");
    }
    {
        // XG wins over a Roland message: the file was authored for XG and the GS reset is the
        // customary "put a GM/GS receiver somewhere sane first" preamble.
        Bytes events;
        append(events, sysex(gs_reset));
        append(events, sysex(xg_on));
        const SongInfo info = read(file(1, {track(events)}));
        check_equal(info.vintage, SongVintage::xg, "XG outranks a GS preamble");
    }
    {
        // The F7 form, which the engine's reader drops on the floor. Recovering it is one of the
        // reasons this walk exists at all.
        const SongInfo info = read(file(1, {track(sysex_f7(xg_on))}));
        check_equal(info.vintage, SongVintage::xg, "an F7-form declaration still counts");
    }
}

// -- Encoding ------------------------------------------------------------------------------------

void test_encoding()
{
    {
        const SongInfo info = read(file(1, {track(meta(0x03, "Plain ASCII"))}));
        check_equal(info.encoding, TextEncoding::ascii, "ascii is recognised as such");
    }
    {
        const SongInfo info = read(file(1, {track(meta(0x03, "Caf\xc3\xa9 du Monde"))}));
        check_equal(info.encoding, TextEncoding::utf8, "valid multi-byte UTF-8");
    }
    {
        // Shift-JIS "ソング" -- lead bytes in the kanji range, which Latin-1 text does not produce.
        const SongInfo info = read(file(1, {track(meta(0x03, "\x83\x5c\x83\x93\x83\x4f"))}));
        check_equal(info.encoding, TextEncoding::shift_jis, "Shift-JIS with real kana");
    }
    {
        // A lone Latin-1 copyright sign: not valid UTF-8, no CJK, so the fallback.
        const SongInfo info = read(file(1, {track(meta(0x02, "Copyright \xa9 1995"))}));
        check_equal(info.encoding, TextEncoding::cp1252, "high bytes that are not UTF-8 or CJK");
    }
}

// -- Malformed files -------------------------------------------------------------------------------

void test_malformed()
{
    {
        const SongInfo info = read(Bytes{});
        check(!info.valid, "an empty file is not valid");
    }
    {
        const SongInfo info = read(Bytes{'N', 'o', 't', 'a', 'm', 'i', 'd', 'i', 0, 0, 0, 0, 0, 0});
        check(!info.valid, "a file that is not a MIDI file is not valid");
    }
    {
        // A track whose declared length runs past the end of the file. Common in the wild, and a
        // reader that trusts the length walks off the buffer. Four bytes takes the End of Track
        // with it and leaves the name whole, which is the case worth pinning: what was read before
        // the cut is still reported.
        Bytes bytes = file(1, {track(meta(0x03, "Truncated"))});
        bytes.resize(bytes.size() - 4);
        const SongInfo info = read(bytes);
        check(info.valid, "a truncated track still yields what was read before the cut");
        check_equal(info.tracks.at(0).name, std::string{"Truncated"}, "the name survived");
    }
    {
        // Running status with none ever set: the track is unreadable from its first byte, and
        // guessing produces plausible nonsense. It must stop, not spin.
        const SongInfo info = read(file(1, {track(Bytes{0x00, 0x3C, 0x64})}));
        check(info.valid, "a file with one bad track is still a file");
        check_equal(info.tracks.at(0).notes, 0, "nothing was invented from it");
    }
    {
        // SMPTE timing, which has no ticks per quarter note to report.
        const SongInfo info = read(file(1, {track(meta(0x03, "Film"))}, 0xE728));
        check_equal(info.division, 0, "SMPTE timing reports no division");
    }
}

} // namespace

int main()
{
    test_names_and_text();
    test_text_is_deduplicated();
    test_running_status();
    test_running_status_survives_a_meta_event();
    test_meta_status_never_becomes_running();
    test_tempo_and_signatures();
    test_lyrics_from_lyric_meta();
    test_soft_karaoke();
    test_markers_exclude_loop_keywords();
    test_lyrics_written_as_markers();
    test_section_markers_are_not_mistaken_for_lyrics();
    test_marker_positions();
    test_marker_positions_follow_a_tempo_change();
    test_markers_from_several_tracks_merge_in_order();
    test_repeated_marker_text_is_kept_when_it_moves();
    test_vintage();
    test_encoding();
    test_malformed();

    if (failures != 0) {
        std::printf("%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("song info: all checks passed\n");
    return 0;
}
