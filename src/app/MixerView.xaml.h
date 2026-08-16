#pragma once

#include "MixerView.g.h"

namespace winrt::WindowsTSPlayer::implementation
{
    struct MixerView : MixerViewT<MixerView>
    {
        MixerView();

        WindowsTSPlayer::PlayerModel Model() const { return model_; }
        void Model(WindowsTSPlayer::PlayerModel const& value);

        void OnContainerContentChanging(
            Microsoft::UI::Xaml::Controls::ListViewBase const& sender,
            Microsoft::UI::Xaml::Controls::ContainerContentChangingEventArgs const& args);

    private:
        void UpdateEmptyState();

        WindowsTSPlayer::PlayerModel model_{ nullptr };
        winrt::event_token vectorToken_{};
    };
}

namespace winrt::WindowsTSPlayer::factory_implementation
{
    struct MixerView : MixerViewT<MixerView, implementation::MixerView>
    {
    };
}
