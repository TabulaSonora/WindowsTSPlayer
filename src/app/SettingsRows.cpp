#include "pch.h"

#include "SettingsRows.h"

using namespace winrt;
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::UI::Xaml::Controls;
using namespace Microsoft::UI::Xaml::Media;

namespace {

Brush ThemeBrush(const wchar_t* key)
{
    return Application::Current().Resources().Lookup(box_value(key)).as<Brush>();
}

} // namespace

namespace tsgui
{
    FrameworkElement MakeCard(UIElement const& content)
    {
        Border card;
        card.Background(ThemeBrush(L"CardBackgroundFillColorDefaultBrush"));
        card.BorderBrush(ThemeBrush(L"CardStrokeColorDefaultBrush"));
        card.BorderThickness(ThicknessHelper::FromUniformLength(1));
        card.CornerRadius(CornerRadiusHelper::FromUniformRadius(kRowCornerRadius));
        card.Padding(ThicknessHelper::FromLengths(kRowPaddingX, kRowPaddingY,
                                                  kRowPaddingX, kRowPaddingY));
        card.Child(content);
        return card;
    }

    FrameworkElement MakeSettingsRow(std::wstring_view title,
                                     std::wstring_view description,
                                     UIElement const& control)
    {
        TextBlock titleText;
        titleText.Text(hstring{ title });
        titleText.TextWrapping(TextWrapping::Wrap);

        StackPanel labels;
        labels.VerticalAlignment(VerticalAlignment::Center);
        labels.Children().Append(titleText);

        if (!description.empty()) {
            TextBlock descriptionText;
            descriptionText.Text(hstring{ description });
            descriptionText.TextWrapping(TextWrapping::Wrap);
            descriptionText.Opacity(0.65);
            descriptionText.FontSize(12);
            descriptionText.Margin(ThicknessHelper::FromLengths(0, 2, 0, 0));
            labels.Children().Append(descriptionText);
        }

        Grid grid;
        grid.ColumnSpacing(16);

        ColumnDefinition stretch;
        stretch.Width(GridLengthHelper::FromValueAndType(1, GridUnitType::Star));
        ColumnDefinition automatic;
        automatic.Width(GridLengthHelper::Auto());
        grid.ColumnDefinitions().Append(stretch);
        grid.ColumnDefinitions().Append(automatic);

        Grid::SetColumn(labels, 0);
        grid.Children().Append(labels);

        // Grid::SetColumn takes a FrameworkElement, not a UIElement, so the control has to be
        // narrowed before it can be placed. Anything that cannot be is left in column zero rather
        // than dropped, which is the harmless failure; in practice everything here is one.
        if (control != nullptr) {
            if (auto element = control.try_as<FrameworkElement>()) {
                element.VerticalAlignment(VerticalAlignment::Center);
                element.HorizontalAlignment(HorizontalAlignment::Right);
                Grid::SetColumn(element, 1);
            }
            grid.Children().Append(control);
        }

        return MakeCard(grid);
    }

    FrameworkElement MakeSettingsGroup(std::wstring_view title,
                                       std::vector<FrameworkElement> const& rows)
    {
        TextBlock heading;
        heading.Text(hstring{ title });
        heading.Style(Application::Current()
                          .Resources()
                          .Lookup(box_value(L"BodyStrongTextBlockStyle"))
                          .as<Style>());
        heading.Margin(ThicknessHelper::FromLengths(0, 0, 0, 4));

        StackPanel panel;
        panel.Spacing(4);
        panel.Children().Append(heading);
        for (auto const& row : rows) {
            panel.Children().Append(row);
        }
        return panel;
    }
}
