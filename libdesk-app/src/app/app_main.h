#pragma once

// The entry of an app library (libandromeda-<app>.so): its one-line executable and andromeda-appd's forked child call it.
extern "C" [[gnu::visibility("default")]] int desk_app_main(int argc, char* argv[]);
