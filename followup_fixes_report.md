# AI bridge follow-up fixes

## Task A — unknown command rejection

Root cause: `ReadFreshCommand` returned any nonempty type with a fresh sequence.
The scene dispatchers have branches only for the ten supported verbs; unknown
verbs reached no branch and never advanced the consumed sequence or wrote an
acknowledgment. The supported set was checked against every dispatcher:
advance, back, capture, choose, confirm, confirm_name, goto, menu, move, set_name.

Changes:

- `src/ai_state.cpp`: check that closed set after the existing parsing,
  freshness validation, and capture-domain isolation. An unknown verb is
  consumed using the existing sequence/ack writer, with status `rejected` and
  reason `unknown command type`, then withheld from scene dispatch. There is
  no state emission or pending-input change. Known verbs take the same path
  as before; malformed-command handling is unchanged.
- `src/ai_state.h`: document the set, single rejection, and the distinction
  between never-valid verbs and known verbs awaiting an applicable scene.
  Existing stale-state behavior is preserved. Capture still belongs only to
  domain 5; that domain ignores all other verbs, including unknown ones.
- `tests/ai_bridge/commands.cpp`, `run_commands.py`: preserve the
  previous temporary harnesses as reproducible repository artifacts. The
  runner compiles the actual bridge and links native engine objects, replacing
  the SDL entry point. No production test hooks are added. Unknown cases in
  the capture regression now expect the intended new unknown-type rejection;
  all 179 capture checks remain. The previous battle harness was also run
  unchanged from `/tmp/battle_result_regression.cpp` (47 checks).

Verification:

- Clean native baseline (`cdba88b`): configure and full 207-step build passed
  in `/tmp/followup-baseline-{src,build}`.
- Clean candidate before commit: configure and full 207-step build passed in
  `/tmp/followup-a-candidate-{src,build}`.
- Command/capture runner: `python3 tests/ai_bridge/run_commands.py BUILD`.
  All 47 previous battle checks also passed against both clean A builds using
  the unchanged previous battle harness. Command harness results: 179 capture
  checks; five initialization cases with 6 checks each;
  2 disabled-bridge checks; and 881 unknown-command checks passed. Unknown
  tests cover 40 combinations of first non-capture domain, current/stale id,
  and verb (`select`, `unknown`, `Choose`, `future_verb`). Each writes exactly
  one rejected ack/log entry, preserves state files and pending inputs, and
  suppresses duplicates even after whitespace rewrites of command.json.
- Differential traces against `cdba88b`: 100 known-command scenarios and 101
  checks per run, with capture unset, `0`, and `1`, matched exactly, including
  ack/state file bytes, routing, sequence and pending bookkeeping. This
  includes all ten known types, current/stale ids, each possible first scene
  reader, inapplicable reads followed by the applicable reader, and duplicates.
- Clean committed Task A source: a separate full 207-step build and the
  command/capture regression suite passed in `/tmp/followup-a-final-{src,build}`.
  The initial isolated-commit build at `/tmp/followup-a-committed-build` also
  passed all 47 previous battle checks and the full command/capture suite.
- `git diff --check` passed.

All clean builds in this report use a `git archive` source tree, with only the
candidate source patch applied for precommit builds, and a previously absent
build directory. Configuration (substitute the source/build directories):

```sh
cmake -S SOURCE -B BUILD -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH=/tmp/ai-native-deps/prefix \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DPLAYER_WITH_FREETYPE=OFF \
  -DPLAYER_WITH_HARFBUZZ=OFF -DPLAYER_AUDIO_BACKEND=OFF
cmake --build BUILD --target easyrpg-player --parallel 6
```

This is the previous fixes' reduced native Linux configuration (built-in fonts,
audio off, local SDL2/liblcf dependency prefix). Logs are in `/tmp/followup-*`.
No live consumer session or other platform build was performed for Task A.
The differential harness exercises the real reader/consume/emit functions;
its scene dispatch model does not execute gameplay input actions.

Commit location: the workspace's `.git` is mounted read-only. `git add` failed
with `Unable to create .../.git/index.lock: Read-only file system`. The two
requested commits are therefore created in `/tmp/followup-commits`, an isolated
local clone with the same ancestry. Source changes and this report are also
present in the original workspace, whose HEAD/index cannot be updated here.
