#include "pch.h"

#include "PartRow.xaml.h"
#if __has_include("PartRow.g.cpp")
#include "PartRow.g.cpp"
#endif

#include "PartModel.h"

#include <format>
#include <string>

using namespace winrt;
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::UI::Xaml::Controls;
using namespace Microsoft::UI::Xaml::Controls::Primitives;

namespace {

/// The strip width below which the chips are dropped rather than squeezed.
///
/// Chosen from what is left over: the address, the meter and the two toggles take about 190px of a
/// strip, so at this width the name and the chips have some 230px between them, enough for the
/// longest tone name in the map beside "Drums / SC-8820" without either being cut.
constexpr double tags_minimum_width = 420.0;

/// How wide a fader is allowed to be. Not a floor like the transport's gain slider: the faders are
/// the one part of a strip that must not grow, because everything to their right has to line up down
/// a column of sixteen and the name to their left is what should be taking the spare width.
constexpr double fader_width = 60.0;

/// The strip width below which the two faders are dropped.
///
/// The pair costs two faders plus the column gaps around them, so this is the chip threshold plus
/// that: at this width the name and the chips have back exactly the room they had at 420px with no
/// faders, and neither has to give any of it up.
constexpr double faders_minimum_width = tags_minimum_width + 2 * fader_width + 24.0;

/// How long a fader goes on ignoring the engine after sending a value, waiting to hear it back.
///
/// Real time rather than a count of syncs, because a sync is not a clock: Sync() runs once per
/// changed property, so a tick that moves the name and the voice count as well as the volume runs it
/// three times and a countdown would expire three times as fast.
constexpr std::chrono::milliseconds fader_echo{ 500 };

/// Pan as the module spells it: 1..127 read as L63 through R63, with 64 in the middle.
///
/// Zero is not a position. The engine folds a CC#10 of 0 to 1 because the controller cannot reach the
/// random-pan setting - only the GS panpot SysEx can write that - so the fader does not offer it and
/// a part that has it is described in words instead.
std::string PanText(int pan)
{
    if (pan <= 0) {
        return "Pan random, set by SysEx";
    }

    const int offset = pan - 64;
    if (offset == 0) {
        return "Pan centre";
    }

    // Whole format strings rather than a letter concatenated onto a word: L and R are English
    // initials, and a language that writes them differently, or puts the distance before the side,
    // has nowhere to say so if the string is assembled here.
    return offset < 0 ? std::format("Pan L{}", -offset) : std::format("Pan R{}", offset);
}

} // namespace

namespace winrt::WindowsTSPlayer::implementation
{
    PartRow::PartRow()
    {
        InitializeComponent();

        volume_.slider = VolumeFader();
        pan_.slider = PanFader();

        volume_.slider.ValueChanged([this](auto&&, auto&&) { Send(volume_, 7); });
        pan_.slider.ValueChanged([this](auto&&, auto&&) { Send(pan_, 10); });

        // Decided on the strip's own width, not the window's. The GTK build makes the same choice and
        // for the same reason: a VisualState with an AdaptiveTrigger is window-relative, and what
        // matters here is how much room this row was actually given - which differs from the window
        // by the scrollbar, the padding and whatever else the list decides to take.
        Root().SizeChanged([this](auto&&, SizeChangedEventArgs const& args) {
            ApplyWidth(args.NewSize().Width);
        });
    }

    // -- Binding -----------------------------------------------------------------------------------

    void PartRow::Part(WindowsTSPlayer::PartModel const& value)
    {
        if (part_ != nullptr) {
            part_.PropertyChanged(partToken_);
        }

        part_ = value;

        // Rows are recycled by the list, so a fader can arrive here still waiting to hear a value
        // back from the part it was showing a moment ago. Carried over, that wait would suppress the
        // first sync of the *new* part and leave the strip showing the old one's level.
        volume_.pending = -1;
        pan_.pending = -1;

        if (part_ != nullptr) {
            partToken_ = part_.PropertyChanged([this](auto&&, auto&&) { Sync(); });
            Sync();
        }
    }

    void PartRow::Sync()
    {
        if (part_ == nullptr) {
            return;
        }

        updating_ = true;

        AddressText().Text(part_.Label());
        NameText().Text(part_.Name());

        const hstring tags = part_.Tags();
        TagsText().Text(tags);

        // Re-applied here as well as on resize, because whether a part *has* chips changes with the
        // part and not with the width.
        hasTags_ = !tags.empty();
        ApplyWidth(Root().ActualWidth());

        ToolTipService::SetToolTip(AddressText(), box_value(part_.Address()));
        ToolTipService::SetToolTip(NameText(), box_value(part_.Detail()));

        Meter().Voices(part_.Voices());

        // The faders follow the engine unless they are waiting to hear back from it.
        if (Accepts(volume_, part_.Volume())) {
            volume_.slider.Value(part_.Volume());
        }
        if (Accepts(pan_, part_.Pan())) {
            pan_.slider.Value(part_.Pan());
        }

        // The faders carry no visible number - a strip has no room for one, and a column of sixteen
        // readouts would be unreadable anyway - so the tooltip is where their value is spelled out.
        // Expression comes along because CC#7 alone does not say how loud a part is: the two are
        // multiplied, and a score that leaves volume alone and rides expression would otherwise look
        // as though nothing were moving.
        ToolTipService::SetToolTip(
            volume_.slider,
            box_value(to_hstring(std::format("Volume {} \xC2\xB7 Expression {}", part_.Volume(),
                                             part_.Expression()))));
        ToolTipService::SetToolTip(pan_.slider, box_value(to_hstring(PanText(part_.Pan()))));

        MuteButton().IsChecked(box_value(part_.Muted()).as<Windows::Foundation::IReference<bool>>());
        SoloButton().IsChecked(box_value(part_.Soloed()).as<Windows::Foundation::IReference<bool>>());

        // Dimming says "something else is soloed", which is a property of the mixer rather than of
        // this strip - so it is the one thing here the part cannot tell us on its own.
        Root().Opacity(part_.Dimmed() ? 0.45 : 1.0);

        updating_ = false;
    }

    // -- What fits ---------------------------------------------------------------------------------

    void PartRow::ApplyWidth(double width)
    {
        // Drops the faders, and then the chips, on a strip too narrow to carry them beside the name.
        //
        // The layout cannot be asked to do either. Given less room than its children want, a Grid
        // takes the shortfall out of the starred column, which is the name - leaving a truncated
        // name beside an untouched chip, the wrong one of the two kept whole, since a chip only
        // qualifies a name the reader can still read. Collapsing a child outright gives the name
        // every pixel there is, and the numbers the chip is derived from are in the tooltip anyway.
        //
        // The faders go one threshold earlier than the chips, so the order under a shrinking window
        // is faders, then chips. They are first because they are the only thing here that a narrow
        // strip cannot render *usefully* - a 30px trough is not something anyone sets a level with,
        // where a squeezed name is still a name - and because what they show can be had back by
        // widening the window, where a truncated name is simply gone.
        //
        // Deciding on the strip's own width is also what keeps this from oscillating, and the
        // thresholds are far enough above the row's own minimum for that to hold: at any width a
        // 360px window can produce, the faders are already hidden, so hiding them can never be what
        // makes the room that would bring them back.
        const auto visible = [](bool show) { return show ? Visibility::Visible : Visibility::Collapsed; };

        const bool faders = width >= faders_minimum_width;
        VolumeFader().Visibility(visible(faders));
        PanFader().Visibility(visible(faders));

        TagsText().Visibility(visible(hasTags_ && width >= tags_minimum_width));
    }

    // -- Widgets -> engine -------------------------------------------------------------------------

    bool PartRow::Accepts(Fader& fader, int value)
    {
        // A fader that just followed the snapshot would fight the pointer: the model republishes ten
        // times a second and any of those ticks can land mid-drag carrying the value from before the
        // drag began, so the slider would be dragged forward by the hand and snapped back by the
        // clock. A fader that has just sent something therefore stops taking the engine's word until
        // it hears its own value back.
        //
        // The wait is bounded rather than left to resolve itself, and the bound is doing real work in
        // two cases. A part with its GS volume receive switch off never echoes anything, and a song
        // that sends its own CC7 while a value of ours is outstanding echoes something else - in both
        // the fader has to give up and show what is true, which is what the timeout makes it do.
        if (fader.pending < 0) {
            return true;
        }

        if (value == fader.pending || std::chrono::steady_clock::now() >= fader.deadline) {
            fader.pending = -1;
            return true;
        }

        return false;
    }

    void PartRow::Send(Fader& fader, int controller)
    {
        if (updating_ || part_ == nullptr || model_ == nullptr) {
            return;
        }

        const int value = static_cast<int>(fader.slider.Value());

        fader.pending = value;
        fader.deadline = std::chrono::steady_clock::now() + fader_echo;

        // The receive channel and not the slot: the engine dispatches a controller by walking the
        // parts looking for one that listens on it, so a message addressed to the slot would go to
        // whatever part happens to be on that channel - which after a bulk dump is not the strip
        // under the pointer. The same reason means one fader can move two parts, when a file has
        // pointed both at one channel; that is the module's own behaviour and the other strip will
        // show it within a tick.
        model_.SendControl(part_.Port(), part_.Channel(), controller, value);
    }

    void PartRow::OnMuteClick(IInspectable const& sender, RoutedEventArgs const&)
    {
        if (updating_ || part_ == nullptr || model_ == nullptr) {
            return;
        }
        model_.SetMuted(part_.Index(), sender.as<ToggleButton>().IsChecked().Value());
    }

    void PartRow::OnSoloClick(IInspectable const& sender, RoutedEventArgs const&)
    {
        if (updating_ || part_ == nullptr || model_ == nullptr) {
            return;
        }
        model_.SetSoloed(part_.Index(), sender.as<ToggleButton>().IsChecked().Value());
    }
}
