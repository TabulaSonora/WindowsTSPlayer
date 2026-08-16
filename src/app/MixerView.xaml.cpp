#include "pch.h"

#include "MixerView.xaml.h"
#if __has_include("MixerView.g.cpp")
#include "MixerView.g.cpp"
#endif

#include "PartRow.xaml.h"

using namespace winrt;
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::UI::Xaml::Controls;

namespace winrt::WindowsTSPlayer::implementation
{
    MixerView::MixerView()
    {
        InitializeComponent();
    }

    void MixerView::Model(WindowsTSPlayer::PlayerModel const& value)
    {
        if (model_ != nullptr) {
            model_.VisibleParts().VectorChanged(vectorToken_);
        }

        model_ = value;

        if (model_ == nullptr) {
            PartList().ItemsSource(nullptr);
            return;
        }

        PartList().ItemsSource(model_.VisibleParts());

        // The empty state follows the list's length, which changes only when the presence diff runs.
        // Watching the vector rather than the tick is what keeps this off the ten-times-a-second
        // path: the message is wrong for a tenth of a second at most, and only when a song is opened
        // or closed.
        vectorToken_ = model_.VisibleParts().VectorChanged([this](auto&&, auto&&) {
            UpdateEmptyState();
        });

        UpdateEmptyState();
    }

    void MixerView::UpdateEmptyState()
    {
        const bool any = model_ != nullptr && model_.VisibleParts().Size() > 0;
        PartList().Visibility(any ? Visibility::Visible : Visibility::Collapsed);
        EmptyText().Visibility(any ? Visibility::Collapsed : Visibility::Visible);
    }

    void MixerView::OnContainerContentChanging(ListViewBase const&,
                                               ContainerContentChangingEventArgs const& args)
    {
        // The row's Part comes from x:Bind in the template; what the template cannot reach is the
        // player, which lives outside the item. Handing it over here rather than binding it means no
        // per-row binding machinery for something that is the same object in all sixteen rows.
        auto row = args.ItemContainer().ContentTemplateRoot().try_as<WindowsTSPlayer::PartRow>();
        if (row == nullptr) {
            return;
        }

        if (args.InRecycleQueue()) {
            // Unbound explicitly, so a recycled row stops listening to a part it is no longer
            // showing. Left subscribed, every recycled row would go on syncing itself from whatever
            // part it last held, ten times a second, for as long as the window lived.
            row.Part(nullptr);
            return;
        }

        row.Model(model_);
    }
}
