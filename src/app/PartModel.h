#pragma once

#include "PartModel.g.h"

#include "host/ts_session.hpp"

#include "tabulasonora/sequence.hpp"

#include <string>

namespace winrt::WindowsTSPlayer::implementation
{
    struct PartModel : PartModelT<PartModel>
    {
        explicit PartModel(int index);

        int32_t Index() const noexcept { return index_; }
        int32_t Port() const noexcept;
        int32_t Channel() const noexcept { return channel_; }

        hstring Label() const { return label_; }
        hstring Address() const { return address_; }
        hstring Name() const { return name_; }
        hstring Tags() const { return tags_; }
        hstring Detail() const { return detail_; }

        int32_t Voices() const noexcept { return voices_; }
        int32_t Volume() const noexcept { return volume_; }
        int32_t Pan() const noexcept { return pan_; }
        int32_t Expression() const noexcept { return expression_; }

        bool Muted() const noexcept { return muted_; }
        bool Soloed() const noexcept { return soloed_; }
        bool Present() const noexcept { return present_; }
        bool Drums() const noexcept { return drums_; }
        bool Dimmed() const noexcept { return dimmed_; }

        winrt::event_token PropertyChanged(
            Microsoft::UI::Xaml::Data::PropertyChangedEventHandler const& handler);
        void PropertyChanged(winrt::event_token const& token) noexcept;

        /// Copies a snapshot's part state in, raising PropertyChanged only for what actually
        /// changed. Not projected: `PartState` is a C++ type and cannot cross an ABI, so the tick
        /// reaches this through winrt::get_self. That is the standard escape hatch and the direct
        /// analogue of what the GObject build does with the same struct.
        ///
        /// `dimmed` is not the part's own state: it means something *else* is soloed, which the
        /// mixer knows and a single part does not.
        void Update(const ts::host::PartState& state, bool dimmed);

    private:
        template <typename T>
        void Set(T& field, T value, hstring const& name);
        void SetText(hstring& field, const std::string& value, hstring const& name);

        const int index_;
        int channel_;

        hstring label_;
        hstring address_;
        hstring name_;
        hstring tags_;
        hstring detail_;

        int32_t voices_{ 0 };

        // The power-on values, not zero: a strip that has never heard from an engine is describing a
        // module at reset, and a silent, hard-left one would be a lie in both directions.
        int32_t volume_{ ts::sequence_builder::default_volume };
        int32_t pan_{ ts::sequence_builder::default_pan };
        int32_t expression_{ ts::sequence_builder::default_expression };

        bool muted_{ false };
        bool soloed_{ false };
        bool present_{ false };
        bool drums_{ false };
        bool dimmed_{ false };

        winrt::event<Microsoft::UI::Xaml::Data::PropertyChangedEventHandler> propertyChanged_;
    };
}
