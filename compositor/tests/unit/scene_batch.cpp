// wlr_scene_update_batch_begin/end defers the visibility pass and output damage of node moves to one pass. A batched
// scene must end every batch with the same output membership (enter/leave, primary output; both follow node
// visibility, so occlusion counts) as a scene that applied the same moves one by one, and with output damage that
// covers the unbatched damage.
#include "check.h"

// clang-format off
// See keybind_parse.cpp: <cmath> must precede the wayland chain.
#include <cmath> // IWYU pragma: keep
#include "wlr.h"
// clang-format on

#include <array>
#include <random>

extern "C" {
#include <wlr/backend/headless.h>
#include <wlr/interfaces/wlr_buffer.h>
}

namespace {

  void testBufferDestroy(wlr_buffer* buffer) { wlr_buffer_finish(buffer); }

  constexpr wlr_buffer_impl kTestBufferImpl = {
      .destroy = testBufferDestroy,
      .get_dmabuf = nullptr,
      .get_shm = nullptr,
      .begin_data_ptr_access = nullptr,
      .end_data_ptr_access = nullptr,
  };

  constexpr int kOutputWidth = 1280;
  constexpr int kOutputHeight = 720;
  constexpr int kWindows = 24;
  constexpr int kBufferSize = 300;

  // One scene with two side-by-side outputs and kWindows trees that each hold one buffer, half of them opaque so
  // they occlude what is below.
  // The outputs a buffer is on, from the enter/leave stream a client observes.
  struct Membership {
    wl_listener enter{};
    wl_listener leave{};
    const std::array<wlr_scene_output*, 2>* outputs = nullptr;
    unsigned mask = 0;

    void watch(wlr_scene_buffer* buffer, const std::array<wlr_scene_output*, 2>* sceneOutputs) {
      outputs = sceneOutputs;
      enter.notify = onEnter;
      leave.notify = onLeave;
      wl_signal_add(&buffer->events.output_enter, &enter);
      wl_signal_add(&buffer->events.output_leave, &leave);
    }

    void stop() {
      wl_list_remove(&enter.link);
      wl_list_remove(&leave.link);
    }

    [[nodiscard]] unsigned bit(const void* output) const {
      return output == (*outputs)[0] ? 1U : output == (*outputs)[1] ? 2U : 0U;
    }

    static void onEnter(wl_listener* listener, void* data) {
      Membership* self = nullptr;
      self = wl_container_of(listener, self, enter);
      self->mask |= self->bit(data);
    }

    static void onLeave(wl_listener* listener, void* data) {
      Membership* self = nullptr;
      self = wl_container_of(listener, self, leave);
      self->mask &= ~self->bit(data);
    }
  };

  struct Scene {
    wlr_scene* scene = nullptr;
    std::array<wlr_scene_output*, 2> outputs{};
    std::array<wlr_scene_tree*, kWindows> windows{};
    std::array<wlr_scene_buffer*, kWindows> buffers{};
    std::array<Membership, kWindows> membership{};

    [[nodiscard]] bool setUp(wlr_backend* backend, wlr_buffer* content) {
      scene = wlr_scene_create();
      if (scene == nullptr) {
        return false;
      }
      for (int i = 0; i < 2; ++i) {
        wlr_output* output = wlr_headless_add_output(backend, kOutputWidth, kOutputHeight);
        if (output == nullptr) {
          return false;
        }
        wlr_output_state state{};
        wlr_output_state_init(&state);
        wlr_output_state_set_enabled(&state, true);
        wlr_output_state_set_custom_mode(&state, kOutputWidth, kOutputHeight, 0);
        const bool committed = wlr_output_commit_state(output, &state);
        wlr_output_state_finish(&state);
        outputs[i] = committed ? wlr_scene_output_create(scene, output) : nullptr;
        if (outputs[i] == nullptr) {
          return false;
        }
        wlr_scene_output_set_position(outputs[i], i * kOutputWidth, 0);
      }
      for (int i = 0; i < kWindows; ++i) {
        windows[i] = wlr_scene_tree_create(&scene->tree);
        buffers[i] = windows[i] != nullptr ? wlr_scene_buffer_create(windows[i], nullptr) : nullptr;
        if (buffers[i] == nullptr) {
          return false;
        }
        membership[i].watch(buffers[i], &outputs);
        wlr_scene_buffer_set_buffer(buffers[i], content);
        if (i % 2 == 0) {
          pixman_region32_t opaque;
          pixman_region32_init_rect(&opaque, 0, 0, kBufferSize, kBufferSize);
          wlr_scene_buffer_set_opaque_region(buffers[i], &opaque);
          pixman_region32_fini(&opaque);
        }
      }
      return true;
    }

    void tearDown() {
      for (Membership& entry : membership) {
        entry.stop();
      }
      wlr_scene_node_destroy(&scene->tree.node);
    }

    void clearDamage() {
      for (wlr_scene_output* output : outputs) {
        pixman_region32_clear(&output->damage_ring.current);
      }
    }

    [[nodiscard]] int primaryIndex(int i) const {
      const wlr_scene_output* primary = buffers[i]->primary_output;
      return primary == outputs[0] ? 0 : primary == outputs[1] ? 1 : -1;
    }
  };

  struct Move {
    int window;
    int x;
    int y;
  };

} // namespace

ANDROMEDA_TEST(batchedMovesMatchSequentialMoves) {
  wl_display* display = wl_display_create();
  CHECK(display != nullptr);
  wlr_backend* backend = wlr_headless_backend_create(wl_display_get_event_loop(display));
  CHECK(backend != nullptr);
  wlr_buffer content{};
  wlr_buffer_init(&content, &kTestBufferImpl, kBufferSize, kBufferSize);

  Scene sequential;
  Scene batched;
  CHECK(sequential.setUp(backend, &content));
  CHECK(batched.setUp(backend, &content));

  std::mt19937 rng(1126);
  std::uniform_int_distribution<int> pickWindow(0, kWindows - 1);
  std::uniform_int_distribution<int> pickX(-kBufferSize, 2 * kOutputWidth);
  std::uniform_int_distribution<int> pickY(-kBufferSize, kOutputHeight);
  std::uniform_int_distribution<int> pickCount(1, 2 * kWindows);

  for (int round = 0; round < 200; ++round) {
    sequential.clearDamage();
    batched.clearDamage();
    // A count above kWindows moves some windows twice in one batch.
    const int count = pickCount(rng);
    for (int m = 0; m < count; ++m) {
      const Move move{pickWindow(rng), pickX(rng), pickY(rng)};
      wlr_scene_node_set_position(&sequential.windows[move.window]->node, move.x, move.y);
      if (m == 0) {
        wlr_scene_update_batch_begin(batched.scene);
        // Nested batches defer to the outermost end.
        wlr_scene_update_batch_begin(batched.scene);
      }
      wlr_scene_node_set_position(&batched.windows[move.window]->node, move.x, move.y);
    }
    wlr_scene_update_batch_end(batched.scene);
    wlr_scene_update_batch_end(batched.scene);

    for (int i = 0; i < kWindows; ++i) {
      CHECK_EQ(batched.membership[i].mask, sequential.membership[i].mask);
      CHECK_EQ(batched.primaryIndex(i), sequential.primaryIndex(i));
    }
    for (int o = 0; o < 2; ++o) {
      pixman_region32_t missed;
      pixman_region32_init(&missed);
      pixman_region32_subtract(
          &missed, &sequential.outputs[o]->damage_ring.current, &batched.outputs[o]->damage_ring.current
      );
      CHECK(!pixman_region32_not_empty(&missed));
      pixman_region32_fini(&missed);
    }
  }

  sequential.tearDown();
  batched.tearDown();
  wlr_buffer_drop(&content);
  wlr_backend_destroy(backend);
  wl_display_destroy(display);
}

int main() { return RUN_TESTS(); }
