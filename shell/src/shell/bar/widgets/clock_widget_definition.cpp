#include "shell/bar/widgets/clock_widget_definition.h"

const noctalia::bar::WidgetDefinition<ClockWidget::Options>& clockWidgetDefinition() {
  using noctalia::bar::field;
  using Options = ClockWidget::Options;

  static const noctalia::bar::WidgetDefinition<Options> definition{
      .type = "clock",
      .fields = {
          field<&Options::shows>({
              .key = "display",
              .choices =
                  {
                      {
                          .value = ClockWidget::Shows::Time,
                          .configValue = "time",
                          .labelKey = "settings.widgets.options.time",
                      },
                      {
                          .value = ClockWidget::Shows::Date,
                          .configValue = "date",
                          .labelKey = "settings.widgets.options.date",
                      },
                  },
              .presentation = settings::WidgetSettingPresentation{
                  .segmented = true,
              },
          }),
          field<&Options::format>({
              .key = "format",
          }),
          field<&Options::verticalFormat>({
              .key = "vertical_format",
          }),
          field<&Options::tooltipFormat>({
              .key = "tooltip_format",
          }),
          field<&Options::timezone>({
              .key = "timezone",
          }),
          field<&Options::stackStyle>({
              .key = "stack_style",
          }),
      },
  };
  return definition;
}
