#pragma once

#include "PlayerModel.g.h"

#include "host/ts_types.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

// Forward-declared for the same reason MainWindow does it: the host layer's headers drag in the
// engine's, and keeping them out of a header the XAML compiler also generates against means the
// generated code, the projection and the engine never have to agree on what windows.h has defined.
// The cost is a destructor declared here and defined where both types are complete.
namespace ts::host {
class Player;
class AudioDevice;
struct SongInfo;
}

namespace winrt::WindowsTSPlayer::implementation
{
    struct PlayerModel : PlayerModelT<PlayerModel>
    {
        PlayerModel();
        ~PlayerModel();

        hstring RomName() const { return romName_; }
        hstring SongName() const { return songName_; }

        bool Playing() const noexcept { return playing_; }
        bool Looping() const noexcept { return looping_; }
        void Looping(bool value);
        bool Complete() const noexcept { return complete_; }
        bool XgMode() const noexcept { return xgMode_; }

        double Position() const noexcept { return position_; }
        double Duration() const noexcept { return duration_; }

        int32_t ActiveVoices() const noexcept { return activeVoices_; }
        int32_t VoiceCapacity() const noexcept { return voiceCapacity_; }
        int64_t Underruns() const noexcept { return underruns_; }

        double OutputGain() const noexcept { return outputGain_; }
        void OutputGain(double value);

        bool Exporting() const noexcept { return exporting_; }

        Windows::Foundation::IAsyncOperation<bool> ExportWavAsync(hstring path);

        hstring LastError() const { return lastError_; }
        hstring BackendDescription() const { return backendDescription_; }

        Windows::Foundation::Collections::IVectorView<WindowsTSPlayer::PartModel> AllParts() const
        {
            return allParts_.GetView();
        }

        Windows::Foundation::Collections::IObservableVector<WindowsTSPlayer::PartModel>
        VisibleParts() const
        {
            return visibleParts_;
        }

        Windows::Foundation::IAsyncOperation<bool> LoadRomAsync(hstring path, bool verifyFully);
        Windows::Foundation::IAsyncOperation<bool> LoadSongAsync(hstring path);
        void UnloadSong();

        void Play();
        void Pause();
        void TogglePlaying();
        void Seek(double seconds);
        void Restart();
        void Panic();

        void SetMuted(int32_t part, bool muted);
        void SetSoloed(int32_t part, bool soloed);
        void ResetChannels();

        winrt::event_token PropertyChanged(
            Microsoft::UI::Xaml::Data::PropertyChangedEventHandler const& handler);
        void PropertyChanged(winrt::event_token const& token) noexcept;

        winrt::event_token PresenceChanged(
            Windows::Foundation::EventHandler<Windows::Foundation::IInspectable> const& handler);
        void PresenceChanged(winrt::event_token const& token) noexcept;

        winrt::event_token RoutingChanged(
            Windows::Foundation::EventHandler<Windows::Foundation::IInspectable> const& handler);
        void RoutingChanged(winrt::event_token const& token) noexcept;

        /// What the loaded file says about itself: names, text, lyrics, loop and the module it asks
        /// for. Not a property and not on the display tick -- it is fixed for the life of a song, so
        /// it is read once when something wants it, reached through winrt::get_self because
        /// `SongInfo` is a C++ type that cannot cross an ABI.
        const ts::host::SongInfo& SongInfo() const;

    private:
        void Refresh();
        void StartTicking();
        void RebuildVisible();

        template <typename T>
        void Set(T& field, T value, hstring const& name);
        void SetText(hstring& field, const std::string& value, hstring const& name);

        std::unique_ptr<ts::host::Player> player_;
        std::unique_ptr<ts::host::AudioDevice> device_;

        /// What the loaded file says about itself, cached at load. Never null after construction, so
        /// SongInfo() has something to return before a song exists.
        std::unique_ptr<ts::host::SongInfo> songInfo_;

        Microsoft::UI::Dispatching::DispatcherQueue dispatcher_{ nullptr };
        Microsoft::UI::Dispatching::DispatcherQueueTimer timer_{ nullptr };

        Windows::Foundation::Collections::IVector<WindowsTSPlayer::PartModel> allParts_{ nullptr };
        Windows::Foundation::Collections::IObservableVector<WindowsTSPlayer::PartModel>
            visibleParts_{ nullptr };

        hstring romName_;
        hstring songName_;
        hstring lastError_;
        hstring backendDescription_;

        bool playing_{ false };
        bool looping_{ false };
        bool complete_{ false };
        bool xgMode_{ false };

        double position_{ 0.0 };
        double duration_{ 0.0 };

        double outputGain_{ 1.0 };
        bool exporting_{ false };

        int32_t activeVoices_{ 0 };
        int32_t voiceCapacity_{ 0 };
        int64_t underruns_{ 0 };

        /// One bit per part, so a change in *which* parts the song addresses can be spotted without
        /// comparing sixty-four structs.
        std::uint64_t presence_{ 0 };

        /// The channel each part listens on, as of the last tick. Six bits per part is too much to
        /// pack into a word the way presence is, and this is compared rather than read.
        std::array<signed char, TS_MAX_PARTS> routing_{};

        winrt::event<Microsoft::UI::Xaml::Data::PropertyChangedEventHandler> propertyChanged_;
        winrt::event<Windows::Foundation::EventHandler<Windows::Foundation::IInspectable>>
            presenceChanged_;
        winrt::event<Windows::Foundation::EventHandler<Windows::Foundation::IInspectable>>
            routingChanged_;
    };
}

namespace winrt::WindowsTSPlayer::factory_implementation
{
    struct PlayerModel : PlayerModelT<PlayerModel, implementation::PlayerModel>
    {
    };
}
