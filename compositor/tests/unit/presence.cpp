#include "check.h"
#include "view/presence.h"

using namespace andromeda;

// Every combination of the inputs, against the design rules: the lock keeps a capture from drawing a window, while a
// background_render game keeps its frames everywhere; a window the scene does not draw is Hidden.
ANDROMEDA_TEST(everyInputCombinationFollowsThePresenceRules) {
  for (unsigned bits = 0; bits < 64; ++bits) {
    const PresenceInputs in{
        .presented = (bits & 1U) != 0,
        .sessionLocked = (bits & 2U) != 0,
        .outputsOff = (bits & 4U) != 0,
        .covered = (bits & 32U) != 0,
        .drawnOffscreen = (bits & 8U) != 0,
        .backgroundRender = (bits & 16U) != 0,
    };
    const PresenceFacts facts = presenceOf(in);
    const bool drawable = in.presented && !in.sessionLocked && !in.outputsOff;
    CHECK((facts.presence == Presence::Hidden) == !in.presented);
    CHECK((facts.presence == Presence::Shown) == (drawable && !in.covered));
    CHECK((facts.presence == Presence::Occluded) == (drawable && in.covered));
    if (in.sessionLocked && !in.backgroundRender) {
      CHECK(!facts.drawsFrames);
    }
    if (in.backgroundRender || (in.drawnOffscreen && !in.sessionLocked)) {
      CHECK(facts.drawsFrames);
    }
    if (!in.backgroundRender && !in.drawnOffscreen) {
      CHECK(facts.drawsFrames == (facts.presence == Presence::Shown));
    }
  }
}

int main() { return RUN_TESTS(); }
