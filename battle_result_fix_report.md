# Battle result bridge fix

Implemented all three requested fixes. Changes are limited to bridge output,
bridge bookkeeping, and an observation hook at battle resolution. Gameplay
calculations, rewards, escape rolls, battle counters, scene transitions, and
command handling are unchanged. No commit was created.

Files changed:

- `src/ai_state.cpp`: retain pending results through the first emitted explore
  state; add displayed result `summary_lines`; capture explicit escape results;
  reset per-battle bookkeeping using weak scene ownership; snapshot the pending
  bundle's sequence number so a subsequent battle cannot relabel it.
- `src/ai_state.h`: declare the battle resolution hook and document the additive
  JSON field and result lifetime.
- `src/scene_battle.cpp`: call the bridge hook from `EndBattle`, before popping
  the scene, passing the actual `BattleResult`. The hook only handles RPG2k
  battle scenes and returns immediately when the bridge is disabled.
- `battle_result_fix_report.md`: this report.

1. Result delivery: `Emit` attaches the pending bundle to messages, menus, and
   other emitted states. It clears the pending flag and payload only after
   writing an explore state carrying that bundle. Deduplicated states do not
   consume it. A caller-supplied `last_battle` also does not consume the pending
   bundle. Existing `result`, `battle_seq`, and `log` keys are preserved.

2. Outcome details: the additive `summary_lines` array contains the assembled
   victory/defeat message lines from the existing display hook, across all
   pages, with presentation control codes removed by the existing stripper.
   Capture occurs before emitting the result message, so that message and later
   states carry the enriched bundle. This preserves displayed EXP, gold, item
   names, level-ups, and learned-skill text when present, without parsing or
   inventing values.

   Separate structured EXP/gold/items/level-up fields were not added. Existing
   `Ai*` accessors expose no retained reward records: EXP, money, and randomized
   drops are local variables in `ProcessSceneActionVictory`, and level-up text
   is generated inside `Game_Actor::ChangeExp` / `ChangeLevel`. The brief's
   displayed-message fallback avoids rerolling drops, inferring reward deltas,
   or assuming a localized message template displays a particular value.
   Missing or undisplayed details are not synthesized, including zero rewards.
   Escape has an empty `summary_lines` array; its displayed success narration
   remains available through the existing battle log.

3. Escape versus abort: only an explicit `BattleResult::Escape` delivered by
   `Scene_Battle::EndBattle` creates an escape result. Merely selecting escape,
   reaching its success substate, failing an escape, or removing the scene does
   not create one. `BattleResult::Abort` creates no bundle and discards any
   provisional victory/defeat belonging to that same battle. Load/title paths
   that remove the scene without calling `EndBattle(Escape)` cannot be mistaken
   for escape. Explicit resolution also overrides a different provisional
   result. Weak scene ownership resets bookkeeping for each new battle,
   including after load/title interruption, without inferring an outcome.

Build verification (repository working directory):

The initial native configuration command was:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
```

It failed because SDL2 development files were absent. There was no configured
build directory or native build script in this checkout. Cached dependency
sources elsewhere on this machine were used to build a native dependency
prefix in `/tmp/ai-native-deps/prefix`, with:

```sh
bash /tmp/build-ai-native-deps.sh > /tmp/build-ai-native-deps.log 2>&1
```

Dependency preparation succeeded. No dependency sources or build scripts were
added to the repository. This verification configuration uses SDL2's available
headless video support, built-in font rendering, audio disabled, and liblcf
without ICU/XML support. These are local build options, not source changes.

The successful configuration and exact Player build commands were:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=/tmp/ai-native-deps/prefix -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DPLAYER_WITH_FREETYPE=OFF -DPLAYER_WITH_HARFBUZZ=OFF -DPLAYER_AUDIO_BACKEND=OFF > /tmp/battle-result-configure.log 2>&1
cmake --build build --target easyrpg-player --parallel 6 > /tmp/battle-result-build.log 2>&1
```

Both exited 0. The initial complete native Player build compiled and linked
successfully (207 steps). After the final edge-case adjustments, the same build
command successfully recompiled `ai_state.cpp` and relinked the executable.

```sh
build/easyrpg-player --version
```

Exited 0 and printed:

```text
EasyRPG Player 0.8.1.1 git@5c05a3e-dirty (2026-11-04)
```

Regression verification:

```sh
python3 /tmp/run_battle_result_regression.py > /tmp/battle-result-regression.log 2>&1
git diff --check
```

Both exited 0; the regression harness passed 47 checks. The temporary C++ harness in
`/tmp/battle_result_regression.cpp` compiles the actual bridge implementation
and links it with the native engine objects, replacing the normal entry point.
It uses the repository's mock actor fixture and battle scene subclass to
exercise message capture and the actual `EndBattle` hook. It checks retention
through messages/menus, deduplication, consumption on explore, Japanese
multi-page reward/level-up/skill text, unchanged existing keys, repeated battle
sequences, successful/failed escape, explicit abort, scene removal without
resolution, authoritative resolution, and the disabled bridge.

No live game playthrough was performed; the regression harness supplies result
message text and selects battle substates directly. Native build artifacts are
in the ignored `build/` directory; harness and dependency artifacts are in
`/tmp`. All requested source changes remain uncommitted in the working tree.
