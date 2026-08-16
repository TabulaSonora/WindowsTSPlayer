#pragma once

#include "PartRow.g.h"

#include <chrono>

namespace winrt::WindowsTSPlayer::implementation
{
    struct PartRow : PartRowT<PartRow>
    {
        PartRow();

        WindowsTSPlayer::PartModel Part() const { return part_; }
        void Part(WindowsTSPlayer::PartModel const& value);

        WindowsTSPlayer::PlayerModel Model() const { return model_; }
        void Model(WindowsTSPlayer::PlayerModel const& value) { model_ = value; }

        void OnMuteClick(IInspectable const& sender, Microsoft::UI::Xaml::RoutedEventArgs const& args);
        void OnSoloClick(IInspectable const& sender, Microsoft::UI::Xaml::RoutedEventArgs const& args);

    private:
        /// One of the two controller faders and the state that keeps it from fighting its own engine.
        struct Fader {
            Microsoft::UI::Xaml::Controls::Slider slider{ nullptr };

            /// The last value this row sent, or -1 when it is simply following the engine.
            int pending{ -1 };

            /// When to stop waiting for `pending` to come back.
            std::chrono::steady_clock::time_point deadline{};
        };

        void Sync();
        void ApplyWidth(double width);
        void Send(Fader& fader, int controller);

        /// Whether the engine's value for a fader may be written into it.
        bool Accepts(Fader& fader, int value);

        WindowsTSPlayer::PartModel part_{ nullptr };
        WindowsTSPlayer::PlayerModel model_{ nullptr };
        winrt::event_token partToken_{};

        Fader volume_;
        Fader pan_;

        /// Whether the bound part has any chips at all. Kept apart from whether they are *shown*,
        /// which is a question of how wide the strip has been allocated.
        bool hasTags_{ false };

        /// Set while the row writes its own controls from the bound part, so the handlers can tell a
        /// rebind from a click and not send the engine a change it just reported.
        bool updating_{ false };
    };
}

namespace winrt::WindowsTSPlayer::factory_implementation
{
    struct PartRow : PartRowT<PartRow, implementation::PartRow>
    {
    };
}
