#include "shell/settings/settings_alert.h"

#include "core/deferred_call.h"
#include "design_tokens.h"
#include "render/core/renderer.h"
#include "ui/builders.h"
#include "ui/controls/alert.h"
#include "ui/controls/flex.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <memory>
#include <optional>
#include <utility>

namespace settings {

  namespace {

    struct AlertState {
      SettingsAlertRequest request;
      std::optional<SettingsModalHost::ModalId> id;
      Node* root = nullptr;
      Button* defaultButton = nullptr;
      bool answered = false;
    };

    std::unique_ptr<Node> build(SettingsModalHost& host, const std::shared_ptr<AlertState>& state) {
      const SettingsAlertRequest& request = state->request;
      state->defaultButton = nullptr;
      ui::AlertProps alert{
          .title = request.title,
          .body = request.body,
          .defaultIndex = request.defaultIndex,
          .defaultButton = &state->defaultButton,
          .scale = request.scale,
      };
      for (std::size_t index = 0; index < request.buttons.size(); ++index) {
        alert.buttons.push_back({
            .text = request.buttons[index].text,
            .variant = request.buttons[index].variant,
            .onClick =
                [&host, weak = std::weak_ptr<AlertState>(state), index] {
                  // Deferred: popping destroys this button while its click is still dispatching.
                  DeferredCall::callLater([&host, weak, index] {
                    const auto alive = weak.lock();
                    if (alive == nullptr || !alive->id.has_value()) {
                      return;
                    }
                    auto action = alive->request.buttons[index].onClick;
                    alive->answered = true;
                    (void)host.pop(*alive->id);
                    if (action) {
                      action();
                    }
                  });
                },
        });
      }
      auto root = ui::alertContent(alert);
      state->root = root.get();
      return root;
    }

  } // namespace

  void showSettingsAlert(SettingsModalHost& host, SettingsAlertRequest request) {
    auto state = std::make_shared<AlertState>();
    state->request = std::move(request);
    state->request.defaultIndex = std::min(state->request.defaultIndex, state->request.buttons.size() - 1);
    const float scale = state->request.scale;
    state->id = host.push(
        SettingsModalRequest{
            .build = [&host, state] { return build(host, state); },
            .measure =
                [state, scale](Renderer& renderer, const SettingsModalLayoutSpace& space) {
                  const float width = std::min(
                      (Tokens::componentDialogAlertWidth - 2.0F * Tokens::componentDialogAlertPadding) * scale,
                      space.maxContentWidth
                  );
                  if (state->root == nullptr) {
                    return LayoutSize{.width = width, .height = 1.0F};
                  }
                  state->root->setSize(width, 0.0F);
                  LayoutConstraints constraints;
                  constraints.setExactWidth(width);
                  return LayoutSize{.width = width, .height = state->root->measure(renderer, constraints).height};
                },
            .initialFocusArea = [state] {
              return state->defaultButton != nullptr ? state->defaultButton->inputArea() : nullptr;
            },
            .requestClose =
                [&host, weak = std::weak_ptr<AlertState>(state)] {
                  if (const auto alive = weak.lock(); alive != nullptr && alive->id.has_value()) {
                    (void)host.pop(*alive->id);
                  }
                },
            .onClosed =
                [state] {
                  state->root = nullptr;
                  if (!state->answered && state->request.onDismissed) {
                    state->request.onDismissed();
                  }
                },
            .contentPadding = Tokens::componentDialogAlertPadding * scale,
            .radius = Tokens::componentDialogAlertRadius,
            .windowMargin = Tokens::componentDialogPadding * scale,
            .scale = scale,
        }
    );
  }

} // namespace settings
