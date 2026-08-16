#include "pch.h"

#include "VoiceMeter.xaml.h"
#if __has_include("VoiceMeter.g.cpp")
#include "VoiceMeter.g.cpp"
#endif

using namespace winrt;
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::UI::Xaml::Media;

namespace {

/// How dim an unsounding bar is. Low enough to read as a track rather than a value, high enough that
/// the meter still has a shape when a part is silent.
constexpr double empty_opacity = 0.18;

} // namespace

namespace winrt::WindowsTSPlayer::implementation
{
    VoiceMeter::VoiceMeter()
    {
        InitializeComponent();

        bars_ = { Bar0(), Bar1(), Bar2(), Bar3(), Bar4(), Bar5() };

        // Looked up once, not per bar per update. This runs sixty-four times a second across a full
        // mixer, and a theme lookup that allocates a brush each time would be the most expensive
        // thing on the tick by a wide margin.
        lit_ = Application::Current().Resources().Lookup(box_value(L"AccentFillColorDefaultBrush"))
                   .as<Brush>();

        auto dim = Application::Current()
                       .Resources()
                       .Lookup(box_value(L"TextFillColorPrimaryBrush"))
                       .as<SolidColorBrush>();

        // A copy rather than the theme's own brush with its opacity changed: brushes from the
        // resource dictionary are shared, and mutating one would dim every piece of text in the
        // window that happens to use it.
        SolidColorBrush faded;
        faded.Color(dim.Color());
        faded.Opacity(empty_opacity);
        unlit_ = faded;

        Voices(0);
    }

    void VoiceMeter::Voices(int32_t value)
    {
        if (value < 0) {
            value = 0;
        }
        if (voices_ == value) {
            return;
        }
        voices_ = value;
        Apply();
    }

    void VoiceMeter::Apply()
    {
        for (int i = 0; i < static_cast<int>(bars_.size()); ++i) {
            bars_[static_cast<size_t>(i)].Fill(i < voices_ ? lit_ : unlit_);
        }
    }
}
