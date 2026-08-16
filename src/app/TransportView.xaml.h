#pragma once

#include "TransportView.g.h"

namespace winrt::WindowsTSPlayer::implementation
{
    struct TransportView : TransportViewT<TransportView>
    {
        TransportView();

        WindowsTSPlayer::PlayerModel Model() const { return model_; }
        void Model(WindowsTSPlayer::PlayerModel const& value);

        void OnPlayClick(IInspectable const& sender, Microsoft::UI::Xaml::RoutedEventArgs const& args);
        void OnRestartClick(IInspectable const& sender, Microsoft::UI::Xaml::RoutedEventArgs const& args);
        void OnLoopClick(IInspectable const& sender, Microsoft::UI::Xaml::RoutedEventArgs const& args);
        void OnPanicClick(IInspectable const& sender, Microsoft::UI::Xaml::RoutedEventArgs const& args);

    private:
        void Sync();

        void OnScrubberValueChanged(
            IInspectable const& sender,
            Microsoft::UI::Xaml::Controls::Primitives::RangeBaseValueChangedEventArgs const& args);

        WindowsTSPlayer::PlayerModel model_{ nullptr };
        winrt::event_token modelToken_{};

        /// True from the moment the scrubber is grabbed until it is let go.
        ///
        /// While it is set the engine's reported position is ignored, because otherwise every tick
        /// would yank the handle out from under the pointer. This is the same latch the mixer's
        /// faders need, for the same reason.
        bool scrubbing_{ false };

        /// Set while this control writes its own widgets from the model, so the handlers can tell
        /// that from a click. Without it, syncing the loop button would call back into the model and
        /// syncing the slider would look like a seek.
        bool updating_{ false };
    };
}

namespace winrt::WindowsTSPlayer::factory_implementation
{
    struct TransportView : TransportViewT<TransportView, implementation::TransportView>
    {
    };
}
