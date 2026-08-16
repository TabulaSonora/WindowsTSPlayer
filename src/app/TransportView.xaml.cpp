#include "pch.h"

#include "TransportView.xaml.h"
#if __has_include("TransportView.g.cpp")
#include "TransportView.g.cpp"
#endif

#include <cmath>
#include <format>
#include <string>

using namespace winrt;
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::UI::Xaml::Controls;
using namespace Microsoft::UI::Xaml::Controls::Primitives;
using namespace Microsoft::UI::Xaml::Input;

namespace {

/// m:ss, which is what the Apple build shows and what fits a two-digit-minute song without
/// re-laying-out the row.
std::string FormatTime(double seconds)
{
    if (!std::isfinite(seconds) || seconds < 0.0) {
        seconds = 0.0;
    }
    const auto total = static_cast<long long>(seconds);
    return std::format("{}:{:02}", total / 60, total % 60);
}

} // namespace

namespace winrt::WindowsTSPlayer::implementation
{
    TransportView::TransportView()
    {
        InitializeComponent();

        // AddHandler with handledEventsToo, not the ordinary event. Slider marks pointer events
        // handled as part of its own dragging, so a plain PointerPressed subscription never fires
        // and the latch below would never arm -- which looks exactly like the latch not working.
        PointerEventHandler press{ [this](IInspectable const&, PointerRoutedEventArgs const&) {
            scrubbing_ = true;
        } };

        PointerEventHandler release{ [this](IInspectable const&, PointerRoutedEventArgs const&) {
            if (!scrubbing_) {
                return;
            }
            scrubbing_ = false;
            if (model_ != nullptr) {
                model_.Seek(Scrubber().Value());
            }
        } };

        Scrubber().AddHandler(UIElement::PointerPressedEvent(), box_value(press), true);
        Scrubber().AddHandler(UIElement::PointerReleasedEvent(), box_value(release), true);

        // Capture loss as well as release: dragging off the control and letting go there delivers
        // no PointerReleased to the slider, and the latch would stay armed forever -- the transport
        // would stop following the engine and never say why.
        Scrubber().AddHandler(UIElement::PointerCaptureLostEvent(), box_value(release), true);

        Scrubber().ValueChanged({ this, &TransportView::OnScrubberValueChanged });

        GainSlider().ValueChanged([this](IInspectable const&,
                                         RangeBaseValueChangedEventArgs const& args) {
            const double gain = args.NewValue();

            // The readout follows the slider rather than the engine, and is a percentage although
            // the setting is a linear multiplier: "120%" says what moving the handle did, where
            // "1.20" only says what it is called. The engine's own unity is 100%.
            GainReadout().Text(to_hstring(std::format("{}%", static_cast<int>(gain * 100.0 + 0.5))));

            if (updating_ || model_ == nullptr) {
                return;
            }
            model_.OutputGain(gain);
        });
    }

    void TransportView::Model(WindowsTSPlayer::PlayerModel const& value)
    {
        if (model_ != nullptr) {
            model_.PropertyChanged(modelToken_);
        }

        model_ = value;

        if (model_ != nullptr) {
            modelToken_ = model_.PropertyChanged([this](auto&&, auto&&) { Sync(); });
        }

        Sync();
    }

    // -- Model -> widgets --------------------------------------------------------------------------

    void TransportView::Sync()
    {
        if (model_ == nullptr) {
            return;
        }

        updating_ = true;

        const hstring song = model_.SongName();
        const hstring rom = model_.RomName();
        const bool hasSong = !song.empty();

        TitleText().Text(hasSong ? song : hstring{ L"No file open" });
        SubtitleText().Text(rom.empty()
                                ? hstring{}
                                : to_hstring(std::format("Sound Canvas voice \xC2\xB7 {}",
                                                         to_string(rom))));

        const double duration = model_.Duration();

        // A zero-length range makes a Slider misbehave, so an empty transport gets a nominal one.
        Scrubber().Maximum(duration > 0.0 ? duration : 1.0);
        if (!scrubbing_) {
            Scrubber().Value(model_.Position());
        }
        Scrubber().IsEnabled(hasSong);

        ElapsedText().Text(to_hstring(
            FormatTime(scrubbing_ ? Scrubber().Value() : model_.Position())));
        TotalText().Text(to_hstring(FormatTime(duration)));

        // Escapes rather than the literal characters. These are private-use codepoints, and a wide
        // literal holding them is only correct while every tool in the chain agrees on the source
        // encoding. /utf-8 is set in the project for the same reason; this does not depend on it.
        // E102 is Play and E103 is Pause.
        PlayGlyph().Glyph(model_.Playing() ? L"\uE103" : L"\uE102");
        ToolTipService::SetToolTip(PlayButton(),
                                   box_value(model_.Playing() ? L"Pause" : L"Play"));

        PlayButton().IsEnabled(hasSong);
        RestartButton().IsEnabled(hasSong);
        PanicButton().IsEnabled(hasSong);
        LoopButton().IsEnabled(hasSong);
        // IsChecked is IReference<bool> because a ToggleButton can be indeterminate, and this
        // projection makes that type's constructor private -- so it is reached by boxing and
        // querying rather than by construction.
        LoopButton().IsChecked(
            box_value(model_.Looping()).as<Windows::Foundation::IReference<bool>>());

        GainSlider().Value(model_.OutputGain());

        // One form rather than a plural: the noun agrees with the ceiling and not with the first
        // number, so "1/64 voice" would be wrong in English and wrong in the same way elsewhere.
        VoicesText().Text(to_hstring(
            std::format("{}/{} voices", model_.ActiveVoices(), model_.VoiceCapacity())));

        XgBadge().Visibility(model_.XgMode() ? Visibility::Visible : Visibility::Collapsed);

        const int64_t underruns = model_.Underruns();
        DropoutsText().Visibility(underruns > 0 ? Visibility::Visible : Visibility::Collapsed);
        if (underruns > 0) {
            DropoutsText().Text(to_hstring(
                std::format("{} dropout{}", underruns, underruns == 1 ? "" : "s")));
        }

        // Visibility only. The bar is indeterminate because there is no fraction to show; see the
        // comment on it in the markup.
        ExportRow().Visibility(model_.Exporting() ? Visibility::Visible : Visibility::Collapsed);

        updating_ = false;
    }

    // -- Widgets -> model --------------------------------------------------------------------------

    void TransportView::OnScrubberValueChanged(IInspectable const&,
                                               RangeBaseValueChangedEventArgs const& args)
    {
        if (updating_ || model_ == nullptr) {
            return;
        }

        if (scrubbing_) {
            // Keep the elapsed readout under the pointer while dragging, without seeking until
            // release. Seeking on every motion event would have the engine replaying controllers
            // continuously, which is audible and, on a long song, slow.
            ElapsedText().Text(to_hstring(FormatTime(args.NewValue())));
            return;
        }

        // Not dragging, so this is the keyboard: arrow keys and Page Up/Down move the value with no
        // pointer involved and therefore no release to seek on. Those are discrete and infrequent,
        // so seeking immediately is right here where it would be wrong above.
        model_.Seek(args.NewValue());
    }

    void TransportView::OnPlayClick(IInspectable const&, RoutedEventArgs const&)
    {
        model_.TogglePlaying();
    }

    void TransportView::OnRestartClick(IInspectable const&, RoutedEventArgs const&)
    {
        model_.Restart();
    }

    void TransportView::OnLoopClick(IInspectable const& sender, RoutedEventArgs const&)
    {
        if (updating_) {
            return;
        }
        model_.Looping(sender.as<ToggleButton>().IsChecked().Value());
    }

    void TransportView::OnPanicClick(IInspectable const&, RoutedEventArgs const&)
    {
        model_.Panic();
    }

}
