#include "pch.h"

#include "PrefsDialog.h"

#include "SettingsRows.h"
#include "ToneMap.h"

#include "tabulasonora/patch_directory.hpp"

#include <cmath>
#include <functional>
#include <string>
#include <vector>
#include <utility>

using namespace winrt;
using namespace Windows::Foundation;
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::UI::Xaml::Controls;

namespace {

using tsgui::MakeSettingsGroup;
using tsgui::MakeSettingsRow;

/// A switch row bound to one boolean key.
FrameworkElement Toggle(std::wstring_view title, std::wstring_view description, bool initial,
                        std::function<void(bool)> const& apply)
{
    ToggleSwitch toggle;
    toggle.IsOn(initial);
    toggle.OnContent(nullptr);
    toggle.OffContent(nullptr);
    toggle.Toggled([apply](IInspectable const& sender, RoutedEventArgs const&) {
        apply(sender.as<ToggleSwitch>().IsOn());
    });
    return MakeSettingsRow(title, description, toggle);
}

/// A choice row over a fixed list of (label, value) pairs.
FrameworkElement Choice(std::wstring_view title, std::wstring_view description,
                        std::vector<std::pair<hstring, int>> const& options, int initial,
                        std::function<void(int)> const& apply)
{
    ComboBox combo;
    combo.MinWidth(150);

    int selected = 0;
    for (int i = 0; i < static_cast<int>(options.size()); ++i) {
        combo.Items().Append(box_value(options[static_cast<size_t>(i)].first));
        if (options[static_cast<size_t>(i)].second == initial) {
            selected = i;
        }
    }
    combo.SelectedIndex(selected);

    // Captured by value: the row outlives this function, and the vector is small.
    combo.SelectionChanged([options, apply](IInspectable const& sender, SelectionChangedEventArgs const&) {
        const int index = sender.as<ComboBox>().SelectedIndex();
        if (index >= 0 && index < static_cast<int>(options.size())) {
            apply(options[static_cast<size_t>(index)].second);
        }
    });

    return MakeSettingsRow(title, description, combo);
}

} // namespace

namespace tsgui
{
    IAsyncAction ShowPreferencesAsync(SettingsStore& store, XamlRoot const& root)
    {
        // A pointer captured by value, not a reference to the parameter. Every handler below outlives
        // this function's first suspension, and a lambda capturing `&store` would be capturing the
        // reference slot in the coroutine frame rather than the store itself. The store is owned by
        // the window and outlives the dialog either way, so this is about saying that plainly rather
        // than about a bug that has been observed.
        SettingsStore* const settings = &store;
        // -- Voice --
        //
        // The module list comes straight from the library, so a vintage added upstream appears here
        // without being listed twice. The values are the module's own bank codes, not an ordering of
        // ours, which is why they are stored rather than an index.
        std::vector<std::pair<hstring, int>> maps;
        for (const auto& [name, value] : ts::tone_map_choices()) {
            (void)name;
            maps.emplace_back(to_hstring(tsgui::ToneMapDisplayName(static_cast<int>(value))),
                              static_cast<int>(value));
        }

        std::vector<FrameworkElement> voice;
        voice.push_back(Choice(L"Module",
                               L"Which module's tone map program changes resolve against",
                               maps, settings->map(),
                               [settings](int v) { settings->set_map(v); }));

        // Three separate labels rather than one with a count: they are fixed labels, not a plural
        // over a runtime number, and a language that inflects "port" differently at 2 and at 4 can
        // say so here without a plural form to carry it.
        voice.push_back(Choice(L"Parts", L"",
                               { { L"16 (1 port)", 1 }, { L"32 (2 ports)", 2 }, { L"64 (4 ports)", 4 } },
                               settings->ports(),
                               [settings](int v) { settings->set_ports(v); }));

        voice.push_back(Choice(L"Polyphony", L"",
                               { { L"64 (hardware)", 64 }, { L"128", 128 }, { L"256", 256 } },
                               settings->polyphony(),
                               [settings](int v) { settings->set_polyphony(v); }));

        // The one setting here that is not a choice between two things the module does: it is a
        // choice between the module and the machine the module models. Worded from the side that is
        // off by default, because "on" is simply the engine behaving well and needs no explaining.
        voice.push_back(Toggle(L"Extended interpolation",
                               L"A wide band-limiting resampler with no pitch ceiling. Turn it off "
                               L"to reproduce SCCore.dll exactly, including its aliasing and the "
                               L"glides it stalls.",
                               settings->extended_interpolation(),
                               [settings](bool v) { settings->set_extended_interpolation(v); }));

        // The other departure from the module, worded from the other side: this one is off by
        // default, so "on" is the half that needs explaining. Deliberately not phrased as a fix for
        // anything -- a file whose bulk dump the hardware truncates is being played correctly when it
        // is truncated here too, and this offers the other reading of it rather than a better one.
        voice.push_back(Toggle(L"Deliver dropped SysEx",
                               L"The module discards a bulk dump too large for one control tick, and "
                               L"so does this. Turn it on to hear such a file as it was written "
                               L"instead.",
                               settings->flush_before_sysex(),
                               [settings](bool v) { settings->set_flush_before_sysex(v); }));

        // -- Effects --
        std::vector<FrameworkElement> effects;
        effects.push_back(Toggle(L"Reverb", L"", settings->reverb(),
                                 [settings](bool v) { settings->set_reverb(v); }));
        effects.push_back(Toggle(L"Chorus", L"", settings->chorus(),
                                 [settings](bool v) { settings->set_chorus(v); }));
        effects.push_back(Toggle(L"Delay", L"", settings->delay(),
                                 [settings](bool v) { settings->set_delay(v); }));
        effects.push_back(Toggle(L"Insertion effects", L"", settings->efx(),
                                 [settings](bool v) { settings->set_efx(v); }));

        // -- Latency --
        NumberBox buffer;
        buffer.Minimum(10);
        buffer.Maximum(400);
        buffer.SmallChange(5);
        buffer.LargeChange(20);
        buffer.Value(settings->latency_ms());
        buffer.SpinButtonPlacementMode(NumberBoxSpinButtonPlacementMode::Compact);
        buffer.MinWidth(120);
        buffer.ValueChanged([settings](NumberBox const&, NumberBoxValueChangedEventArgs const& args) {
            // NaN is what a NumberBox reports when its text has been cleared, and rounding that
            // through an int is undefined. Ignored rather than clamped: the box is mid-edit, and
            // writing a value now would fight whatever is being typed.
            if (std::isnan(args.NewValue())) {
                return;
            }
            settings->set_latency_ms(static_cast<int>(args.NewValue()));
        });

        std::vector<FrameworkElement> latency;
        latency.push_back(MakeSettingsRow(
            L"Buffer",
            L"How far ahead the engine renders, in milliseconds. Lower answers a keyboard sooner; "
            L"raise it if the transport reports dropouts.",
            buffer));

        StackPanel page;
        page.Spacing(16);
        page.Children().Append(MakeSettingsGroup(L"Voice", voice));
        page.Children().Append(MakeSettingsGroup(L"Effects", effects));
        page.Children().Append(MakeSettingsGroup(L"Latency", latency));

        ScrollViewer scroller;
        scroller.Content(page);
        scroller.VerticalScrollBarVisibility(ScrollBarVisibility::Auto);
        scroller.MaxHeight(520);

        ContentDialog dialog;
        dialog.Title(box_value(L"Preferences"));
        dialog.Content(scroller);
        dialog.CloseButtonText(L"Close");
        dialog.DefaultButton(ContentDialogButton::Close);

        // Without this the dialog throws rather than appearing: a desktop app's dialog has no
        // implicit XamlRoot to inherit, and the exception names neither the dialog nor the root.
        dialog.XamlRoot(root);

        co_await dialog.ShowAsync();
    }
}
