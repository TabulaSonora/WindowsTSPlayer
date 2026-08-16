#pragma once

#include "VoiceMeter.g.h"

#include <array>

namespace winrt::WindowsTSPlayer::implementation
{
    struct VoiceMeter : VoiceMeterT<VoiceMeter>
    {
        VoiceMeter();

        int32_t Voices() const noexcept { return voices_; }
        void Voices(int32_t value);

    private:
        void Apply();

        int32_t voices_{ -1 };

        std::array<Microsoft::UI::Xaml::Shapes::Rectangle, 6> bars_{};

        Microsoft::UI::Xaml::Media::Brush lit_{ nullptr };
        Microsoft::UI::Xaml::Media::Brush unlit_{ nullptr };
    };
}

namespace winrt::WindowsTSPlayer::factory_implementation
{
    struct VoiceMeter : VoiceMeterT<VoiceMeter, implementation::VoiceMeter>
    {
    };
}
