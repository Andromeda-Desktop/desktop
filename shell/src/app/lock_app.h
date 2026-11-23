#pragma once

// `noctalia lock`: the lock screen in its own process (.agent/TASK-lock-process.md). It waits unlocked, answers
// org.andromeda.Lock1, logind Lock/Unlock and the compositor sleep activity, and holds ext-session-lock while locked.
int runLockMode(int argc, char* argv[]);
