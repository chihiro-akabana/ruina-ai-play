# On-demand capture bridge change

Implemented on HEAD `e7d7a6e2edee1344ccb925bdac1628f46b5520fa`. No commit
was created. Changes observe rendered pixels and bridge files only; no input,
event, battle, or other gameplay code was changed.

Files changed:

- `src/ai_state.cpp`: read the capture gate once at bridge initialization;
  isolate capture command reads; stage and publish native PNG/JSON files;
  acknowledge success or rejection. Capture rejection adds `detail` and also
  retains the existing `reason` field. Existing commands' ack fields are unchanged.
- `src/ai_state.h`: declare the poll and document the state-id exception,
  output files, and opt-in gate.
- `src/player.cpp`: include the bridge header and call its poll immediately
  after `Player::Draw()`, with a marked observation-only hook.
- `capture_fix_report.md`: this report.

The existing ostream screenshot overload uses the configured screenshot scale.
The capture implementation therefore calls the same display surface's
`WritePNG` directly, through a binary stream in the bridge directory, without
changing configuration or using the save-directory screenshot overload.

## Protocol and domain isolation

The command remains `{"seq": n, "state_id": n, "type": "capture"}` in
`command.json`. It follows the existing positive, increasing sequence-number
and duplicate-suppression rules. Capture has its own raw-content cache (domain
5). `ReadFreshCommand` filters capture versus non-capture immediately after
JSON parsing, before the existing state-id checks or command dispatch:

- Domains 0–4 never return capture, even with a stale requested state id.
- Domain 5 never returns another command type. It cannot acknowledge or advance
  the consumed sequence for those commands; their own raw-content caches remain
  available to their scene polls.
- Non-capture parsing, stale rejection eligibility, scene dispatch, pending
  commands, and state emission retain their previous behavior. No pre-existing
  domain routing rules were changed.

Capture deliberately ignores the requested `state_id`, because title and
picture-based waits can have absent or stale text state. Both capture metadata
and its ack use the current last-emitted state id, or 0 before any emission.
The scene name is the lowercase current `Scene::instance` type, rather than a
possibly stale text-state type. Capture does not emit a new state, change state
deduplication, drop a pending message input, or change queued movement.

Only the exact environment value `EASYRPG_AI_CAPTURE=1`, sampled alongside
`EASYRPG_AI_STATE_DIR` at initialization, enables PNG writing. Unset, `0`,
`true`, and `01` are disabled. With a working bridge directory, disabled capture
is consumed and acknowledged as `rejected`, with `detail: "capture disabled"`;
it does not produce capture files. Without a configured bridge, polling is inert.

On success, `capture.png.tmp` and `capture.json.tmp` are completely written and
closed, then renamed to `capture.png` and `capture.json`, before acknowledging
`applied`. Metadata includes `seq`, `state_id`, `scene`, `width`, and `height`.
No display, encode, stream, or rename failure is treated as success. Failures
are acknowledged as `rejected` with a short detail, and temporary-file cleanup
is attempted. A previously complete capture may remain after rejection.

Each rename is atomic; the two files are not one filesystem transaction. If
metadata publication fails after PNG publication, the PNG is complete but the
metadata may be old or absent, and the command is rejected. Consumers should
use an `applied` ack and matching metadata sequence to identify a successful
capture. No failed attempt exposes a partially written final PNG. As with the
existing protocol, an unwritable ack/log destination cannot itself deliver an
ack; this change does not redesign the existing ack writer.

## Clean native build

A fresh source directory `/tmp/capture-clean-src` was populated with
`git archive HEAD`, then only the three source-file changes above were applied
with `git apply`. It contains no other working-tree files or old build products.
The final three source files were checked byte-for-byte against the workspace.
A separate, initially absent build directory was used. Commands:

```sh
cmake -S /tmp/capture-clean-src -B /tmp/capture-clean-build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=/tmp/ai-native-deps/prefix -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DPLAYER_WITH_FREETYPE=OFF -DPLAYER_WITH_HARFBUZZ=OFF -DPLAYER_AUDIO_BACKEND=OFF > /tmp/capture-configure.log 2>&1
cmake --build /tmp/capture-clean-build --target easyrpg-player --parallel 6 > /tmp/capture-build.log 2>&1
/tmp/capture-clean-build/easyrpg-player --version
```

Configuration and the full 207-step compile/link both exited 0. Version output:
`EasyRPG Player 0.8.1.1 (2026-11-04)`. This uses the same native dependency
prefix and reduced options as the previous fix: built-in fonts, audio off,
SDL2 headless support, and liblcf without ICU/XML. Windows, Web, and other
platform builds were not run.

## Regression harness

The previous temporary-harness approach was mirrored. The harness includes the
actual bridge implementation and links the clean native engine objects,
replacing the normal entry point. No test-only production hooks were added.
Harness sources/scripts and logs are in `/tmp`, outside the delivered source
changes:

```sh
python3 /tmp/run_capture_regression.py > /tmp/capture-regression.log 2>&1
python3 /tmp/run_capture_battle_regression.py > /tmp/capture-battle-regression.log 2>&1
git diff --check
```

All exited 0. Results:

- Capture-specific harness: **179 checks passed**. Tests cover all five legacy
  reader domains rejecting stale capture commands; interleaved capture and
  non-capture commands; preservation of stale foreign-command eligibility;
  absence of new fields in legacy acks; duplicate suppression; title id 0;
  stale requests with a later current id; current scene metadata; unchanged
  state files/dedupe and pending message/movement bookkeeping; full PNG decode
  at 320×240 with screenshot scaling set to 4; missing UI/surface; disabled
  capture; and subsequent recovery.
- File errors were injected using temporary-name directories, `/dev/full`
  symlinks for PNG and metadata writes, and directories blocking final renames.
  Checks verified rejected acks, temporary cleanup, preservation of earlier
  complete files before publication, and a complete PNG even when metadata
  publication failed.
- Differential compatibility harness: compiled both unmodified HEAD's bridge
  and the new bridge. With capture never sent, **100 command scenarios per
  run, 101 checks each**, produced exactly equal command, ack, state, and
  bookkeeping traces for capture unset, `0`, and `1`. Scenarios vary the first
  polling domain, ten legacy/unknown command types, current/stale ids, and
  duplicate reads. These exercise actual reader, consume/ack, and emission
  functions; scene input actions are not executed by this compatibility trace.
- Separate initialization cases checked unset, `0`, `true`, `01`, and `1`:
  **6 checks each**. Changing the environment after initialization did not
  change the sampled gate. Disabled bridge: **2 checks passed**.
- Previous battle-result harness, adapted only to use the new clean build's
  objects: **all 47 checks passed**.

The C++ capture harness is `/tmp/capture_regression.cpp`; its baseline source
is `/tmp/capture-baseline-ai_state.cpp` from `git show HEAD:src/ai_state.cpp`.
An initial capture-harness run lacked the bitmap format initialization normally
performed by the UI; initializing that test fixture corrected the harness.
No production change was needed for that fixture issue.

## Real headless frames and limitations

```sh
python3 /tmp/run_capture_live.py > /tmp/capture-live.log 2>&1
```

Exited 0. The clean Player executable ran with SDL's dummy video driver against
a temporary copy of the locally available game. All save/config/log/bridge
outputs were redirected into `/tmp/capture-live-7xjl6xhv`; no external game or
consumer files were modified. The script waited five seconds before sending
one capture with requested state id -999. Screenshot scale was configured as 4.

| Startup | Captured scene | Ack state id | PNG dimensions | Result |
| --- | --- | --- | --- | --- |
| Normal | title | 0 | 320×240 | applied |
| `--new-game` | map | 1 | 320×240 | applied |
| `--battle-test 1` | battle | 2 | 320×240 | applied |
| Normal, gate unset | — | 0 | no PNG | rejected, capture disabled |

No capture existed before the request. Text-state files were unchanged during
each capture/refusal. Metadata matched the ack, and no temporary capture files
remained. I inspected the title and battle PNGs: they contain the rendered menu
and battle HUD, but this local reduced setup also displays missing-image
checkerboards and incomplete/garbled Japanese text. Thus real frame capture
and post-draw polling were exercised; correct visual rendering of the real
game's full assets/text was not established here. No full playthrough, specific
facility HUD, NPC-position scene, or picture-wait sequence was tested.

No ordinary scene exclusion was found at this poll point: it is in the common
main loop immediately after drawing, before the null-scene exit check. Capture
waits for that loop to render; it cannot service requests while the process is
blocked before drawing, stopped, or inside a platform modal dialog. It captures
the engine display surface at native resolution, including whatever fade or
transition is currently drawn, rather than OS window decorations or platform
UI overlays. Only the native SDL2 configuration was exercised live.
