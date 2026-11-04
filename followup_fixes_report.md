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

## Task B — displayed battle-result summaries

Root cause established from the engine flow:

1. `Scene_Battle_Rpg2k::vUpdate` calls `AiState::UpdateBattle` before processing
   scene actions. `CheckBattleEndConditions` can then select `State_Victory`
   or `State_Defeat` and reset `scene_action_substate` to 0 in the same frame.
2. `ProcessSceneActionVictory` / `ProcessSceneActionDefeat`, in `eBegin` (0),
   assemble their result `PendingMessage`. Victory includes whatever reward,
   level-up, and skill text the game actually appends. Both handlers call
   `Game_Message::SetPendingMessage` **before** setting `eEnd` (1).
3. `SetPendingMessage` immediately calls `Window_Message::StartMessageProcessing`.
   That function assembles pages/word wrapping and synchronously calls
   `AiState::OnMessageStart`. At this hook the state is `State_Victory` or
   `State_Defeat`, **substate 0**, not 1. The old substate-1 guard misses it.
   By the following frame's bridge poll, the result text has already been
   emitted as an ordinary message; the bundle is created with no saved lines.

Changes:

- `src/ai_state.cpp`: observe the assembled result message in victory/defeat
  substate 0. Retain its already stripped, displayed lines and outcome with
  the current battle. The existing result observation (poll or `EndBattle`)
  copies those lines into the matching bundle. Message observation does not
  create a bundle early. If the poll already created this battle's matching
  provisional bundle, enrich it before the message emits. New-battle identity
  and Abort clear staged text; outcome matching prevents an escape/other
  authoritative result from inheriting victory text. Pending bundles and the
  existing first-explore delivery/consumption rules are unchanged.
- `src/ai_state.h`: document that the ordinary result message can precede its
  bundle, and that escape keeps an empty summary.
- `tests/ai_bridge/battle_result.cpp`: preserve the original 47 checks, correcting
  victory and defeat scenarios to deliver their ordinary message at substate
  0 before the poll observes the result. The old victory scenario instead
  pre-created a bundle with `UpdateBattle` and manually switched to substate
  1 before calling the message hook; the old defeat scenario also delivered
  at substate 1. Those fixtures masked the real timing bug. Retention, dedupe,
  log/sequence compatibility, multi-page Japanese reward/level/skill text,
  explicit escape/abort, interruption, and disabled-bridge checks remain.
  Thirty additional checks exercise actual result handlers and staged-text
  lifecycle; total **77 checks passed**.
- `tests/ai_bridge/run_battle.py`: standalone native harness runner, independent
  of Task A's command runner; optional `--battle-source` supports running the
  corrected scenario against the old implementation.

The added native-handler cases call actual `ProcessSceneActionVictory` and
`ProcessSceneActionDefeat`, `Game_Message::SetPendingMessage`, and
`Window_Message::StartMessageProcessing`, then the actual `EndBattle` hook.
They prove that the message emits synchronously before the handler returns at
substate 1 and precedes the bundle. Victory uses deterministic fixture rewards
(8 EXP, 8G, guaranteed herb) and compares the summary with the emitted display
pages in order. Defeat compares its summary with the actual displayed defeat
page. Defeat has **not** been observed in live play; it is proved here in the
native-handler harness. The fixture initializes the drawable registry, battle
flag, bitmap format, and escape character normally supplied by Player startup;
initial fixture failures required those additions, not production changes.

Additional checks exclude pre-battle, in-battle, post-result, and post-battle
messages; cover a bundle observed before its message; check escape overrides
stay empty; discard staged text after scene replacement; retain a previous
battle's nonempty bundle while the next result message appears; and create a
summary directly via `EndBattle` without an intervening poll. No structured
rewards, hidden values, or synthetic protocol fields were added. Gameplay,
input dispatch, event execution, result/reward calculations and scene code
were not modified.

Verification:

- The preceding clean committed Task A build is the Task B baseline.
- Clean Task B candidate: configure and full 207-step build passed in
  `/tmp/followup-b-candidate-{src,build}` with the same reduced options above.
- Clean committed Task B source: configure and full 207-step build passed in
  `/tmp/followup-b-committed-{src,build}`. The committed build's version command
  succeeded. Final source files were compared byte-for-byte with the workspace.
- `python3 tests/ai_bridge/run_battle.py BUILD`: **77 checks passed**, retaining
  all 47 previous checks with the timing corrections described above.
- `python3 tests/ai_bridge/run_commands.py BUILD`: **179 capture checks**,
  **881 unknown-command checks**, five gate cases (6 checks each), and the
  disabled bridge (2 checks) passed. All known-command differential traces
  against original HEAD `cdba88b` still matched byte-for-byte: 100 scenarios,
  101 checks per run, capture unset / `0` / `1`.
- Negative regression: running the corrected battle scenario against the
  pre-Task-B bridge fails at `all displayed reward/level/skill pages are cleaned
  and captured` because its summary is empty. This reproduces the observed
  failure rather than relying on the old artificial substate-1 ordering.
- `git diff --check` passed. Configure/build/regression logs use the
  `/tmp/followup-b-*` prefix. Both final clean builds use the same options as
  the baseline, with no build products or unrelated untracked briefs imported.

Limitations: no full game/session replay, live defeat, or non-Linux build was
run. Native-handler checks exercise real assembly and bridge hooks in a
controlled fixture; they do not render/advance every frame of an interactive
battle. The original multi-page fixture covers level/skill text; the real
handler reward fixture uses no actors and therefore no level-ups. Neither
existing result fields nor capture/input semantics were changed beyond Task A's
specified unknown-verb acknowledgment. Commits are in the isolated clone due
to the workspace metadata restriction described under Task A.

A portable `followup_fixes.bundle` in the workspace root contains exactly the
two-commit range after `cdba88b`, for import into a writable checkout. It is an
untracked delivery artifact, not part of either source commit.
