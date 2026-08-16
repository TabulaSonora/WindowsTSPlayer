#include "pch.h"

#include "SongInfoWindow.h"

#include "PlayerModel.h"
#include "SettingsRows.h"
#include "ToneMap.h"
#include "WindowChrome.h"

#include "host/ts_session.hpp"
#include "host/ts_song_info.hpp"

// Run and Paragraph, which the precompiled header does not carry: nothing else in this program needs
// the document model, and a RichTextBlock's content is built out of it rather than set as a string.
#include <winrt/Microsoft.UI.Xaml.Documents.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <format>
#include <functional>
#include <memory>
#include <string>
#include <vector>

using namespace winrt;
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::UI::Xaml::Controls;
using namespace Microsoft::UI::Xaml::Media;

namespace {

using ts::host::SongInfo;
using ts::host::TextEncoding;

// -- Text --------------------------------------------------------------------------------------

/// Turns a file's bytes into something a TextBlock can hold.
///
/// A Standard MIDI File declares no encoding at all -- FF 01 and its neighbours are "any amount of
/// text", bytes and nothing more -- so `read_song_info` guesses one, and this is where the guess is
/// spent.
///
/// Two passes, and the first exists because the guess was made from a *sample*. A string the
/// detector never looked at can be malformed under the encoding it chose, and MB_ERR_INVALID_CHARS
/// is the only way to find that out: without the flag MultiByteToWideChar substitutes and reports
/// success, so a wrong guess would come back as a row of replacement characters rather than as a
/// signal to try something else. Only UTF-8 is worth the check -- 1252 has no invalid byte and 932's
/// own detection already ran against the whole file.
///
/// The second pass deliberately drops the flag. A file that mixes encodings -- and they exist --
/// should lose the byte it got wrong rather than the whole string, because a track list with one
/// blank row is still a track list and one with seventeen is not. That is what
/// `g_convert_with_fallback` gives the GTK build, and dropping the flag is how the same behaviour is
/// spelled here.
///
/// Nothing is escaped on the way out, unlike the GTK build, where every one of these went through
/// `g_markup_escape_text` before reaching a widget that parses Pango markup -- the first real file
/// this was pointed at was credited to "Hoagy Carmichael & Stuart Gorell", whose ampersand made GTK
/// drop the whole string. A TextBlock's Text is plain text and parses nothing, so that entire class
/// of bug does not exist here.
hstring Decode(const std::string& raw, TextEncoding encoding)
{
    if (raw.empty()) {
        return {};
    }

    const auto convert = [&raw](UINT page, DWORD flags) -> std::wstring {
        const int length = MultiByteToWideChar(page, flags, raw.data(),
                                               static_cast<int>(raw.size()), nullptr, 0);
        if (length <= 0) {
            return {};
        }
        std::wstring text(static_cast<std::size_t>(length), L'\0');
        MultiByteToWideChar(page, flags, raw.data(), static_cast<int>(raw.size()), text.data(),
                            length);
        return text;
    };

    UINT page = ts::host::encoding_code_page(encoding);
    if (page == CP_UTF8 && convert(CP_UTF8, MB_ERR_INVALID_CHARS).empty()) {
        // Windows-1252, which is what the GTK build falls back to for the same reason: it has no
        // undefined byte, so it always produces something, and a Latin-1 copyright sign is far and
        // away the likeliest thing a "UTF-8" file actually holds.
        page = 1252;
    }

    return hstring{ convert(page, 0) };
}

/// Seconds as `m:ss`.
///
/// Truncating rather than rounding, which is what the transport's own FormatTime does. The two read
/// the same length, so rounding here would show a song as a second longer than the clock beside the
/// scrubber says it is -- the same number disagreeing with itself, on two windows of one program.
std::string ClockText(double seconds)
{
    if (!std::isfinite(seconds) || seconds < 0.0) {
        seconds = 0.0;
    }
    const auto total = static_cast<long long>(seconds);
    return std::format("{}:{:02}", total / 60, total % 60);
}

std::string FramesToClock(std::int64_t frames)
{
    return ClockText(static_cast<double>(frames) / ts::host::Session::sample_rate);
}

// -- Rows --------------------------------------------------------------------------------------

Brush ThemeBrush(const wchar_t* key)
{
    return Application::Current().Resources().Lookup(box_value(key)).as<Brush>();
}

Style ThemeStyle(const wchar_t* key)
{
    return Application::Current().Resources().Lookup(box_value(key)).as<Style>();
}

/// The icon face, asked for by name rather than spelled out, so one family answers for every glyph
/// here and the set can be changed without auditing which codepoints live where. The same reasoning
/// the transport's markup follows with SymbolThemeFontFamily.
FontFamily SymbolFont()
{
    return Application::Current()
        .Resources()
        .Lookup(box_value(L"SymbolThemeFontFamily"))
        .as<FontFamily>();
}

/// A read-only row: a title on the left, a selectable value on the right.
///
/// Selectable because everything on this window is something somebody might want to paste somewhere
/// -- a composer's name into a search, a loop point into a sequencer -- and a value that cannot be
/// copied out is a value that has to be retyped.
FrameworkElement ValueRow(std::wstring_view title, hstring const& value,
                          hstring const& description = {})
{
    TextBlock valueText;
    valueText.Text(value);
    valueText.IsTextSelectionEnabled(true);
    valueText.TextWrapping(TextWrapping::Wrap);
    valueText.TextAlignment(TextAlignment::Right);
    valueText.Opacity(0.65);

    // Bounded, so a long copyright line wraps into a column rather than pushing the title off the
    // left edge of a window this narrow.
    valueText.MaxWidth(280);

    return tsgui::MakeSettingsRow(title, std::wstring_view{ description }, valueText);
}

/// The same, with an em dash for a value the file does not state.
///
/// A dash rather than a hidden row. A file that says nothing about itself is the common case -- most
/// of them do -- and a window that shrinks to three rows for one looks broken, where one that says
/// "Copyright --" has answered the question.
FrameworkElement ValueRowOrDash(std::wstring_view title, hstring const& value,
                                hstring const& description = {})
{
    return ValueRow(title, value.empty() ? hstring{ L"—" } : value, description);
}

/// The file's own words, as one read-only block.
///
/// **Monospaced, and that is not a stylistic preference.** This text was written in a fixed-width
/// world and is laid out for one: files pad track names and headings with trailing spaces so columns
/// line up, rule off sections with runs of dashes, and set credits as blocks. Haru-no-umi.mid pads
/// every one of its names to the same width. A proportional face silently discards all of that and
/// turns a laid-out sheet into ragged prose, which looks like the file being untidy rather than like
/// us having reflowed it.
///
/// Consolas, the face the transport's readouts already use, rather than a second monospaced font
/// nothing else here asks for. A Japanese file falls through to the system's own CJK face glyph by
/// glyph, which is what Windows does everywhere and is why this does not have to name a font per
/// script.
///
/// A RichTextBlock, and specifically not a read-only TextBox. A TextBox is an input control wearing
/// a disabled hat: it takes focus as one, shows a caret as one, carries a spell checker and a
/// context menu offering Cut and Paste greyed out, and announces itself to a screen reader as an
/// edit field the reader cannot edit. None of that describes what this is. A RichTextBlock is
/// display text that happens to be selectable, which is exactly the thing -- and it still gives one
/// continuous selection across every line, which is the point a card per line could not manage.
///
/// One paragraph per line rather than one Run holding the newlines, so the breaks are structural
/// rather than dependent on how a Run treats a control character. Margins zeroed, because a
/// paragraph's default spacing would double-space a lyric sheet.
///
/// Wrapping, despite the fixed-width argument above, because the alternative is worse. The window is
/// one narrow column and a scrollbar running the other way inside a page that already scrolls
/// vertically is the more confusing of the two. Short lines -- which is nearly all of them -- keep
/// their leading alignment either way; only an over-long one degrades.
RichTextBlock MakeTextArea(hstring const& body)
{
    RichTextBlock area;
    area.IsTextSelectionEnabled(true);
    area.TextWrapping(TextWrapping::Wrap);
    area.FontFamily(FontFamily{ L"Consolas" });

    std::wstring_view rest{ body };
    for (;;) {
        const auto brk = rest.find(L'\n');
        std::wstring_view line = rest.substr(0, brk);

        // A file written on one platform and read on another arrives with the carriage return still
        // attached, and a stray CR renders as a box in a monospaced face rather than as nothing.
        if (line.ends_with(L'\r')) {
            line.remove_suffix(1);
        }

        Documents::Run run;
        run.Text(hstring{ line });

        Documents::Paragraph paragraph;
        paragraph.Margin(ThicknessHelper::FromUniformLength(0));
        paragraph.Inlines().Append(run);
        area.Blocks().Append(paragraph);

        if (brk == std::wstring_view::npos) {
            break;
        }
        rest = rest.substr(brk + 1);
    }

    return area;
}

/// A marker: what it says, where it falls, and a jump to it.
///
/// Clickable, because a marker whose position is known and cannot be reached is a label rather than
/// a place -- "Chorus" is worth showing mostly because it is worth going to. Seeking only, not
/// seek-and-play: the transport keeps whatever state it had, so this scrubs a paused song and jumps
/// a playing one, which is what a scrubber does and what this row is standing in for.
///
/// A Button wearing the card's colours rather than a card containing a Button. The difference shows
/// under the pointer: the whole row has to light up, and a button inside a card would highlight a
/// rectangle inset from the border people are aiming at. The two brushes are replaced through the
/// control's own resources rather than by setting Background directly, so the pointer-over and
/// pressed states still come from the default template instead of being flattened by a local value.
FrameworkElement MarkerRow(winrt::WindowsTSPlayer::PlayerModel const& model, hstring const& body,
                           std::int64_t frames)
{
    const double seconds = static_cast<double>(frames) / ts::host::Session::sample_rate;

    TextBlock text;
    text.Text(body);
    text.TextWrapping(TextWrapping::Wrap);
    text.VerticalAlignment(VerticalAlignment::Center);

    TextBlock stamp;
    stamp.Text(to_hstring(ClockText(seconds)));
    stamp.Opacity(0.65);
    // The same monospaced face the transport's readouts use, so a column of timestamps lines up on
    // the colon instead of drifting with each row's digits.
    stamp.FontFamily(FontFamily{ L"Consolas" });
    stamp.VerticalAlignment(VerticalAlignment::Center);

    FontIcon chevron;
    chevron.Glyph(L"\uE76C");
    chevron.FontFamily(ThemeBrush(L"SystemControlPageTextBaseHighBrush") != nullptr
                           ? FontFamily{ L"Segoe Fluent Icons" }
                           : FontFamily{ L"Segoe Fluent Icons" });
    chevron.FontSize(12);
    chevron.Opacity(0.5);
    chevron.VerticalAlignment(VerticalAlignment::Center);

    Grid grid;
    grid.ColumnSpacing(12);
    for (const auto width : { GridLengthHelper::FromValueAndType(1, GridUnitType::Star),
                              GridLengthHelper::Auto(), GridLengthHelper::Auto() }) {
        ColumnDefinition column;
        column.Width(width);
        grid.ColumnDefinitions().Append(column);
    }
    Grid::SetColumn(text, 0);
    Grid::SetColumn(stamp, 1);
    Grid::SetColumn(chevron, 2);
    grid.Children().Append(text);
    grid.Children().Append(stamp);
    grid.Children().Append(chevron);

    Button row;
    row.Resources().Insert(box_value(L"ButtonBackground"),
                           ThemeBrush(L"CardBackgroundFillColorDefaultBrush"));
    row.Resources().Insert(box_value(L"ButtonBorderBrush"),
                           ThemeBrush(L"CardStrokeColorDefaultBrush"));
    row.CornerRadius(CornerRadiusHelper::FromUniformRadius(tsgui::kRowCornerRadius));
    row.Padding(ThicknessHelper::FromLengths(tsgui::kRowPaddingX, tsgui::kRowPaddingY,
                                             tsgui::kRowPaddingX, tsgui::kRowPaddingY));
    row.HorizontalAlignment(HorizontalAlignment::Stretch);
    row.HorizontalContentAlignment(HorizontalAlignment::Stretch);
    row.Content(grid);
    row.Click([model, seconds](auto&&, auto&&) { model.Seek(seconds); });
    return row;
}

// -- The pages ---------------------------------------------------------------------------------

std::wstring ChannelList(const std::vector<int>& channels)
{
    std::wstring text;
    for (const int channel : channels) {
        if (!text.empty()) {
            text += L", ";
        }
        // One-based, because that is how every sequencer and this program's own mixer number them.
        text += std::to_wstring(channel + 1);
    }
    return text;
}

std::string ContainerText(SongInfo const& info)
{
    // `to_smf` reports that it converted, not what it converted from, so a foreign container can
    // only be described as one. Saying so still matters: it explains why a .xmi has track names and
    // a format number at all.
    return info.container.empty()
               ? std::format("Standard MIDI File, format {}", info.format)
               : std::format("Converted to Standard MIDI File, format {}", info.format);
}

/// A page's scrolling body. The window is one narrow column on every page, so the horizontal scroll
/// bar is turned off rather than left to appear the moment a track name is long.
ScrollViewer NewPage(UIElement const& content)
{
    ScrollViewer scroller;
    scroller.HorizontalScrollMode(ScrollMode::Disabled);
    scroller.HorizontalScrollBarVisibility(ScrollBarVisibility::Disabled);
    scroller.VerticalScrollBarVisibility(ScrollBarVisibility::Auto);
    scroller.Padding(ThicknessHelper::FromLengths(16, 4, 16, 16));
    scroller.Content(content);
    return scroller;
}

StackPanel NewColumn()
{
    StackPanel column;
    column.Spacing(16);
    return column;
}

/// Something to say when there is nothing to show, rather than a page of em dashes.
FrameworkElement EmptyState(std::wstring_view glyph, std::wstring_view title)
{
    FontIcon icon;
    icon.Glyph(hstring{ glyph });
    icon.FontSize(40);
    icon.Opacity(0.4);

    TextBlock text;
    text.Text(hstring{ title });
    text.Style(ThemeStyle(L"SubtitleTextBlockStyle"));
    text.Opacity(0.6);
    text.HorizontalAlignment(HorizontalAlignment::Center);

    StackPanel panel;
    panel.Spacing(12);
    panel.HorizontalAlignment(HorizontalAlignment::Center);
    panel.VerticalAlignment(VerticalAlignment::Center);
    panel.Children().Append(icon);
    panel.Children().Append(text);
    return panel;
}

/// The song page is a pure function of the file's own metadata plus the module setting. It reads no
/// model property, which is what keeps it from picking up the previous song's values when it is
/// rebuilt from a SongName change -- the properties are refreshed from the render thread's snapshot,
/// which happens *after* the name has already said the file changed.
FrameworkElement BuildSongPage(SongInfo const& info, winrt::WindowsTSPlayer::PlayerModel const& model,
                               int map)
{
    auto column = NewColumn();

    // No "Name" row: the window's own subtitle already carries the file name, and repeating it as
    // the first row of the first group says the same thing twice on one screen.
    column.Children().Append(tsgui::MakeSettingsGroup(
        L"File",
        { ValueRow(L"Container", to_hstring(ContainerText(info))),
          ValueRow(L"Tracks", to_hstring(std::to_string(info.track_count))),
          ValueRow(L"Duration", to_hstring(FramesToClock(info.length))) }));

    // -- What it asks to be played on --
    //
    // The evidence is the "Asks For" row's own description rather than a second row: it is the
    // reason for the value beside it, not a separate fact.
    const std::string wanted = ts::host::vintage_name(info.vintage);
    column.Children().Append(tsgui::MakeSettingsGroup(
        L"Module",
        { ValueRow(L"Asks For",
                   wanted.empty() ? hstring{ L"No preference" } : to_hstring(wanted),
                   to_hstring(info.vintage_evidence)),
          ValueRow(L"Playing On", to_hstring(tsgui::ToneMapDisplayName(map))) }));

    // -- Timing --
    std::string tempo;
    if (info.initial_tempo_bpm > 0.0) {
        const int bpm = static_cast<int>(info.initial_tempo_bpm + 0.5);
        // Two whole phrases rather than a suffix appended to the first: the qualifier lands in a
        // different place in different languages, and appending fixes it at the end.
        tempo = info.tempo_changes > 1 ? std::format("{} bpm, varying", bpm)
                                       : std::format("{} bpm", bpm);
    }
    column.Children().Append(tsgui::MakeSettingsGroup(
        L"Timing",
        { ValueRowOrDash(L"Tempo", to_hstring(tempo)),
          ValueRowOrDash(L"Time", to_hstring(info.time_signature)),
          ValueRowOrDash(L"Key", to_hstring(info.key_signature)) }));

    // -- The loop --
    //
    // Only when there is one. A group whose entire content is "there is no loop" is a row saying
    // nothing, and the great majority of files have none.
    if (info.has_loop) {
        // Named rather than labelled "Loop", because the two behave differently at the jump: a soft
        // loop rewinds cleanly, where a hard one's end is inferred and replays controllers the way a
        // seek does.
        const std::string span = std::format("{} – {}", FramesToClock(info.loop_start),
                                             FramesToClock(info.loop_end));
        column.Children().Append(tsgui::MakeSettingsGroup(
            L"Loop", { ValueRow(info.loop_soft ? L"Soft" : L"Hard", to_hstring(span)) }));
    }

    // -- What it says --
    //
    // One block rather than a row per line, which is the shape the file itself has: the copyright,
    // the text events and the karaoke headings are consecutive lines of one document, and the GTK
    // build's row-per-line -- chosen there because Adw.ActionRow was the vocabulary to hand -- broke
    // that document up into cards and lost whatever the author had lined up between them.
    //
    // Joined here rather than by the reader, so a selection runs across the lot in one go.
    std::wstring said;
    const auto add = [&said](hstring const& line) {
        if (!said.empty()) {
            said += L"\n";
        }
        said += std::wstring_view{ line };
    };
    if (!info.copyright.empty()) {
        add(Decode(info.copyright, info.encoding));
    }
    for (const std::string& line : info.text) {
        add(Decode(line, info.encoding));
    }
    for (const std::string& heading : info.karaoke_headings) {
        add(Decode(heading, info.encoding));
    }

    if (!said.empty()) {
        // On the same card every other row on this page sits on, which is what marks the file's own
        // words off from our labels around them. It brings no scrolling of its own -- a RichTextBlock
        // simply grows -- so the page's scroller stays the only one, and the wheel keeps working
        // wherever the pointer happens to be.
        column.Children().Append(tsgui::MakeSettingsGroup(
            L"Text", { tsgui::MakeCard(MakeTextArea(hstring{ said })) }));
    }

    if (!info.markers.empty()) {
        std::vector<FrameworkElement> rows;
        rows.reserve(info.markers.size());
        for (const auto& marker : info.markers) {
            rows.push_back(MarkerRow(model, Decode(marker.text, info.encoding), marker.position));
        }
        column.Children().Append(tsgui::MakeSettingsGroup(L"Markers", rows));
    }

    return NewPage(column);
}

FrameworkElement BuildTracksPage(SongInfo const& info)
{
    StackPanel column;
    column.Spacing(4);

    for (const auto& track : info.tracks) {
        const hstring name = Decode(track.name, info.encoding);
        const std::wstring heading =
            std::to_wstring(track.number) + L". "
            + (name.empty() ? std::wstring{ L"Untitled" } : std::wstring{ std::wstring_view{ name } });

        // The instrument name and the channels are two different answers to "what is this track",
        // and files supply one, both or neither.
        const hstring instrument = Decode(track.instrument, info.encoding);
        std::wstring subtitle{ std::wstring_view{ instrument } };
        const std::wstring channels = ChannelList(track.channels);
        if (!channels.empty()) {
            if (!subtitle.empty()) {
                subtitle += L" · ";
            }
            // The plural is over how many channels the list names, not over any number in it, so the
            // joined list is substituted into a form chosen by its length.
            subtitle += track.channels.size() == 1 ? L"Channel " : L"Channels ";
            subtitle += channels;
        }

        TextBlock count;
        count.Text(to_hstring(std::format("{} note{}", track.notes, track.notes == 1 ? "" : "s")));
        count.Opacity(0.65);
        count.FontFamily(FontFamily{ L"Consolas" });

        column.Children().Append(tsgui::MakeSettingsRow(heading, subtitle, count));
    }

    return NewPage(column);
}

FrameworkElement BuildLyricsPage(SongInfo const& info)
{
    const hstring text = Decode(info.lyrics, info.encoding);
    if (text.empty()) {
        return EmptyState(L"\uE8A5", L"No lyrics");
    }

    // The same monospaced block as the file's other text, because it is the same bytes out of the
    // same file, decoded the same way. A karaoke sheet is if anything the more laid-out of the two --
    // Soft Karaoke writes its lines to be read in time with the music.
    //
    // On the page's own scroller and not on a card. A card is what separates one group from the next,
    // and there is nothing here to separate it from: the sheet is the whole page.
    return NewPage(MakeTextArea(text));
}

} // namespace

namespace tsgui
{
    Window CreateSongInfoWindow(winrt::WindowsTSPlayer::PlayerModel const& model,
                                SettingsStore& settings)
    {
        // By pointer, taken by value, exactly as the preferences do it: every callback below outlives
        // this function, and capturing the reference parameter would capture this frame's reference
        // slot rather than the store.
        SettingsStore* const store = &settings;

        Window window;
        window.Title(L"Song Information");

        TextBlock caption;
        caption.Text(L"Song Information");
        caption.Style(ThemeStyle(L"CaptionTextBlockStyle"));
        caption.VerticalAlignment(VerticalAlignment::Center);
        caption.Margin(ThicknessHelper::FromLengths(16, 0, 0, 0));
        caption.TextTrimming(TextTrimming::CharacterEllipsis);

        Grid titleBarArea;
        titleBarArea.Children().Append(caption);

        // The three pages, switched by a SelectorBar -- the WinUI answer to the view switcher the
        // GTK build puts in a bottom bar. It goes at the top here instead: a bar pinned to the
        // bottom edge is a GNOME and a phone idiom, and on Windows the tabs of a window belong under
        // its title.
        SelectorBar pages;
        pages.Margin(ThicknessHelper::FromLengths(12, 0, 12, 4));
        for (const auto* label : { L"Song", L"Tracks", L"Lyrics" }) {
            SelectorBarItem item;
            item.Text(label);
            pages.Items().Append(item);
        }

        // Every page stays in the tree and is toggled by Visibility, rather than being navigated to
        // in a Frame. A Frame keeps the page it is not showing measured, which is the pathology the
        // main window's own stack comment describes, and it would also throw away the scroll
        // position each time the reader looked at the tracks and came back.
        Grid host;
        Grid::SetRow(host, 2);

        Grid root;
        for (const auto height : { GridLengthHelper::FromPixels(32), GridLengthHelper::Auto(),
                                   GridLengthHelper::FromValueAndType(1, GridUnitType::Star) }) {
            RowDefinition row;
            row.Height(height);
            root.RowDefinitions().Append(row);
        }
        Grid::SetRow(titleBarArea, 0);
        Grid::SetRow(pages, 1);
        root.Children().Append(titleBarArea);
        root.Children().Append(pages);
        root.Children().Append(host);

        // No Background anywhere up this tree, which is load-bearing rather than an omission: a
        // backdrop is drawn behind the window's content, so an opaque brush here would hide it
        // completely -- and the failure reads as Mica not being applied rather than as something
        // painting over it.
        window.Content(root);

        SetUpWindowChrome(window, titleBarArea);
        SetWindowIcon(window);

        // Remembered separately from the player's, under its own keys. The two are different shapes --
        // this one is a tall narrow column and that one is wide -- so a shared size would be wrong for
        // whichever of them was resized second.
        RestoreWindowGeometry(window, store->song_info_width(), store->song_info_height(),
                              store->song_info_maximized());

        // Shows whichever page the bar has selected. Read back from the bar rather than tracked in a
        // variable of our own, so a rebuild that resets the selection cannot leave the two disagreeing
        // about which page is on screen.
        const auto reveal = [host, pages]() {
            uint32_t selected = 0;
            if (const auto item = pages.SelectedItem()) {
                pages.Items().IndexOf(item, selected);
            }
            const auto children = host.Children();
            for (uint32_t index = 0; index < children.Size(); ++index) {
                children.GetAt(index).Visibility(index == selected ? Visibility::Visible
                                                                   : Visibility::Collapsed);
            }
        };

        // Everything is rebuilt rather than updated in place, which is the opposite of what the mixer
        // does and is right for the opposite reason. The mixer's shape is fixed at sixty-four parts
        // and only its values move, ten times a second. This content's *shape* is a function of the
        // file -- a row per track, a group that exists only when there are markers -- so there is
        // nothing to update in place, and it runs when a file is opened rather than on any tick.
        auto rebuild = std::make_shared<std::function<void()>>();
        *rebuild = [model, store, host, pages, caption, reveal]() {
            host.Children().Clear();

            const hstring song = model.SongName();
            if (song.empty()) {
                caption.Text(L"Song Information");
                pages.Visibility(Visibility::Collapsed);
                host.Children().Append(EmptyState(L"\uE8D6", L"No song loaded"));
                reveal();
                return;
            }

            // Reached through get_self because SongInfo is a plain C++ struct and cannot cross an
            // ABI. This is the standard C++/WinRT escape hatch, and the same one PartState takes.
            const SongInfo& info =
                get_self<winrt::WindowsTSPlayer::implementation::PlayerModel>(model)->SongInfo();

            caption.Text(to_hstring(std::format("Song Information · {}", to_string(song))));
            pages.Visibility(Visibility::Visible);

            host.Children().Append(BuildSongPage(info, model, store->map()));
            host.Children().Append(BuildTracksPage(info));
            host.Children().Append(BuildLyricsPage(info));

            // Back to the first page. A reader who was on Lyrics when a new file was opened is not
            // asking to see that file's lyrics; they are looking at a different song now.
            pages.SelectedItem(pages.Items().GetAt(0));
            reveal();
        };

        pages.SelectionChanged([reveal](auto&&, auto&&) { reveal(); });

        // Follows the player rather than freezing on the file that was open when it appeared, which
        // is the whole reason this is a window that can be left up. SongName is the signal because it
        // is set as part of the load, ahead of the tick that refreshes everything else.
        const auto token = model.PropertyChanged(
            [rebuild](auto&&, Data::PropertyChangedEventArgs const& args) {
                if (args.PropertyName() == L"SongName") {
                    (*rebuild)();
                }
            });

        // The window comes from the event's own sender rather than being captured. Capturing it would
        // hang a reference to the window off an event the window itself owns, and a window that cannot
        // drop its last reference never finishes closing -- which here would keep the model alive too.
        window.Closed([model, token, store](auto&& sender, auto&&) {
            const auto geometry =
                MeasureWindowGeometry(sender.template as<Window>(), store->song_info_width(),
                                      store->song_info_height());
            store->set_song_info_geometry(geometry.width, geometry.height, geometry.maximized);

            // Unsubscribed by hand, and this is not tidiness. The handler holds the shared rebuild,
            // which holds the model, the pages and the caption; left registered, the whole tree would
            // stay reachable from a model that outlives this window, and the parts of it that hold
            // engine state would never be released.
            model.PropertyChanged(token);
        });

        (*rebuild)();
        return window;
    }
}
