#include "pch.h"

#include "PartModel.h"
#if __has_include("PartModel.g.cpp")
#include "PartModel.g.cpp"
#endif

#include "ToneMap.h"

#include "tabulasonora/sequence.hpp"

#include <format>
#include <string>
#include <vector>

using namespace winrt;
using namespace Microsoft::UI::Xaml::Data;

namespace {

/// The separator between clauses, matching the other two front ends: U+00B7, not a hyphen.
constexpr const char* separator = " \xC2\xB7 ";

/// The strip's label and its spelled-out form.
///
/// The port comes from the slot and never moves; the channel does not: it is the one the part
/// listens on, and the two part company as soon as anything moves a part.
void FormatAddress(int port, int channel, std::string& label, std::string& spelled)
{
    label = std::format("{}{}", static_cast<char>('A' + port), channel + 1);
    spelled = std::format("Port {}, channel {}", static_cast<char>('A' + port), channel + 1);
}

/// Whether a part is sounding drums is asked of the part, not of the channel number: GS can route
/// any part to the drum path over SysEx and XG does it from bank select alone. The tone map moves
/// per part and per moment too -- a bank LSB names a vintage and an XG System On moves every part at
/// once -- so this is read from the part's own state on every update rather than from the engine's
/// configured map.
std::string TagsFor(const ts::host::PartState& state)
{
    std::string tags;

    if (state.drums) {
        tags = state.kit >= 0 ? std::format("Kit {}", state.kit) : "Drums";
    }

    const char* map = tsgui::ToneMapDisplayName(state.map);
    if (*map != '\0') {
        if (!tags.empty()) {
            tags += separator;
        }
        tags += map;
    }

    return tags;
}

/// The numbers behind the name.
///
/// Programs are counted from one, as every patch chart and every module's own display counts them,
/// while the wire value is zero-based -- so the raw byte is given too rather than leaving anyone
/// comparing against a MIDI capture to work out which convention this is.
///
/// Both halves of the bank select appear because neither identifies anything alone: on this module
/// the MSB carries the variation and the LSB names the vintage.
std::string DetailFor(const ts::host::PartState& state)
{
    // Whole clauses, each formatted in one piece, rather than a sentence grown by +=. The joining is
    // the only thing left to do here because a translator cannot reorder around an append: what
    // reads "Program 1 (PC 0)" in English puts the number first in some languages and last in
    // others, and only a complete format string lets them say so. These become .resw entries in the
    // i18n milestone and the shape has to survive that.
    std::vector<std::string> clauses;

    if (state.drums && state.kit >= 0) {
        clauses.emplace_back(std::format("Drum kit {}", state.kit));
    }

    clauses.emplace_back(std::format("Program {} (PC {})", state.program + 1, state.program));
    clauses.emplace_back(std::format("Bank MSB {}, LSB {}", state.bank, state.bankLsb));

    // Under XG the melodic lookup is not given the bank the part was sent, so saying only what was
    // sent would misdescribe what is sounding.
    if (!state.drums && state.lookupBank != state.bank) {
        clauses.emplace_back(std::format("(resolves against bank {})", state.lookupBank));
    }

    std::string detail;
    for (const std::string& clause : clauses) {
        if (!detail.empty()) {
            detail += separator;
        }
        detail += clause;
    }

    return detail;
}

} // namespace

namespace winrt::WindowsTSPlayer::implementation
{
    PartModel::PartModel(int index)
        : index_(index)
        // The slot's own channel, until an engine says otherwise. At power-on the two agree, so this
        // is the right thing to show before a file has been loaded rather than a placeholder.
        , channel_(index % ts::Sequence::channel_count)
    {
        std::string label;
        std::string spelled;
        FormatAddress(Port(), channel_, label, spelled);
        label_ = to_hstring(label);
        address_ = to_hstring(spelled);
    }

    int32_t PartModel::Port() const noexcept
    {
        return index_ / ts::Sequence::channel_count;
    }

    event_token PartModel::PropertyChanged(PropertyChangedEventHandler const& handler)
    {
        return propertyChanged_.add(handler);
    }

    void PartModel::PropertyChanged(event_token const& token) noexcept
    {
        propertyChanged_.remove(token);
    }

    template <typename T>
    void PartModel::Set(T& field, T value, hstring const& name)
    {
        if (field != value) {
            field = value;
            propertyChanged_(*this, PropertyChangedEventArgs{ name });
        }
    }

    void PartModel::SetText(hstring& field, const std::string& value, hstring const& name)
    {
        // Compared before converting rather than after: this runs sixty-four times per tick, ten
        // times a second, and almost every call finds nothing changed. to_hstring allocates.
        hstring wanted = to_hstring(value);
        if (field != wanted) {
            field = std::move(wanted);
            propertyChanged_(*this, PropertyChangedEventArgs{ name });
        }
    }

    void PartModel::Update(const ts::host::PartState& state, bool dimmed)
    {
        // The channel the part *hears*, which is not its slot. A cleared snapshot reports none, and
        // then the slot's own is the honest answer rather than everything claiming channel 1.
        const int channel =
            state.rxChannel >= 0 ? state.rxChannel : index_ % ts::Sequence::channel_count;

        if (channel != channel_) {
            channel_ = channel;

            std::string label;
            std::string spelled;
            FormatAddress(Port(), channel_, label, spelled);

            SetText(label_, label, L"Label");
            propertyChanged_(*this, PropertyChangedEventArgs{ L"Channel" });

            // Announced like the rest, unlike the GObject build, where the spelled-out form was
            // answered on demand from a tooltip callback. Here it is an ordinary bound property, so
            // staying quiet would leave a screen reader describing the previous channel.
            SetText(address_, spelled, L"Address");
        }

        SetText(name_, state.name.empty() ? "\xE2\x80\x94" : state.name, L"Name");
        SetText(tags_, TagsFor(state), L"Tags");
        SetText(detail_, DetailFor(state), L"Detail");

        Set(voices_, static_cast<int32_t>(state.voices), L"Voices");
        Set(volume_, static_cast<int32_t>(state.volume), L"Volume");
        Set(pan_, static_cast<int32_t>(state.pan), L"Pan");
        Set(expression_, static_cast<int32_t>(state.expression), L"Expression");
        Set(muted_, state.muted, L"Muted");
        Set(soloed_, state.soloed, L"Soloed");
        Set(present_, state.present, L"Present");
        Set(drums_, state.drums, L"Drums");
        Set(dimmed_, dimmed, L"Dimmed");
    }
}
