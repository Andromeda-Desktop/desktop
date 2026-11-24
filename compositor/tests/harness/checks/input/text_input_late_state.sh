#!/usr/bin/env bash
# A text-input-v3 client that sets surrounding text and content type in a commit
# after the one enabling the text input, as Chromium does, must still have both
# relayed to the input method.
set -euo pipefail

source "$ANDROMEDA_HARNESS_LIB"

readonly INPUT_METHOD="${ANDROMEDA_INPUT_METHOD_CLIENT:-./build-debug/tests/input-method-client}"
readonly OBSERVER="${ANDROMEDA_SEAT_LOG_CLIENT:-./build-debug/tests/seat-log-client}"
readonly TEXT_LOG="$ANDROMEDA_RUNTIME_DIR/text-input-window.log"
readonly INPUT_METHOD_LOG="$ANDROMEDA_RUNTIME_DIR/input-method.log"
# zwp_text_input_v3 content_hint.spellcheck and content_purpose.url
readonly HINT_SPELLCHECK=2
readonly PURPOSE_URL=5

"$INPUT_METHOD" > "$INPUT_METHOD_LOG" 2>&1 &
await_log_count "$INPUT_METHOD_LOG" '^grabbed$'
ENABLE_TEXT_INPUT=1 TEXT_INPUT_LATE_STATE=1 "$OBSERVER" text-input-window > "$TEXT_LOG" 2>&1 &
await_log_count "$TEXT_LOG" '^text-input-enter$'

await_log_count "$INPUT_METHOD_LOG" "^surrounding-text text='late text' cursor=9 anchor=9$"
await_log_count "$INPUT_METHOD_LOG" "^content-type hint=$HINT_SPELLCHECK purpose=$PURPOSE_URL$"

echo "surrounding text and content type set after text-input enable reach the input method"
