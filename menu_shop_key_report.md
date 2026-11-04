# Menu, shop, and key follow-up

Base: `c905abf0f0cf287ebb564a61cb8aa3178e6f114e`.
The workspace source matches that base, but its checked-out Git history ends at
`cdba88b`. `/proc/self/mountinfo` confirms workspace `.git` is mounted read-only.
The existing `followup_fixes.bundle` was imported into a writable clone at
`/tmp/menu-shop-key-git`; commits are made there without changing workspace Git
metadata. The final bundle will contain exactly three commits after `c905abf`.

## A — equipment

Implemented in `Scene_Equip::AiDecide`, `SelectSlot`, `EquipSelectedItem`,
`AiPreview` (`src/scene_equip.cpp/.h`) and `AiState::UpdateEquip`
(`src/ai_state.cpp`, documented in `.h`). Choose indices are zero-based slots
at `level: slot`, and zero-based native candidate-list entries at `level: item`.
Removal is an explicit candidate with `name: null`, `remove: true`.
Both keyboard and bridge call the same slot/equipment actions. Class/item,
inventory, fixed/cursed equipment and index validity are checked before bridge
writes; equipment mutation is exclusively the existing `ChangeEquipment` path.
Back at item returns to slot; back at slot retains scene-pop behavior.

State adds actor atk/def/spi/agi, selected slot, candidate names/counts and
preview totals. Preview is the exact original screen calculation, now shared
with the renderer, including two-handed other-hand removal, caps and status
multipliers. Each candidate includes the preview available by highlighting it;
top-level `preview` is the current highlight. Equip equipment names now read
one-based actor slots correctly (the old bridge used slots 0–4). The separate
existing status-state equipment output was kept unchanged for compatibility.

Screen-only deviations: raw item stat modifiers, two_handed and
currently-equipped-elsewhere are omitted. The actual equip windows draw names,
inventory counts, actor totals and preview totals, not those database fields.
Raw modifiers cannot in general be recovered from totals after caps/states and
other-hand conflicts. No hidden IDs or those hidden properties are emitted.
The screen's observable preview totals provide equipment comparison instead.

Verification actually run:

- CMake/Ninja native Release configure and full build passed (audio backend and
  FreeType disabled; dependencies `/tmp/ai-native-deps/prefix`).
- `python3 tests/ai_bridge/run_menu_shop_key.py /tmp/menu-shop-key-build`:
  native scene fixture checks actor values against defined stats, filtered
  wrong-class candidates, explicit wrong-class revalidation, invalid indices,
  two-handed preview/conflict resolution, following-state equipment, inventory,
  removal, back, stale commands, misplaced/unknown verbs and single ack.
- `run_commands.py`: exact differential against `cdba88b`, 100 scenarios ×
  capture unset/0/1, 101 checks each; capture 179, unknown 881, five gate cases
  (6 each) and disabled bridge (2) all passed. This tests the unchanged legacy
  command paths; a further differential against the imported base is recorded
  with the final verification below.
- `run_battle.py`: all 77 checks passed. `git diff --check` passed.

No live equip/session replay was run. Tests use the real native scene/windows
with a deterministic RPG2k3 database fixture, not the Ruina actor setup.
`menu_equip` schema additions and its slot correction intentionally change equip
observations even without equip commands; byte identity is checked on unchanged
legacy paths, excluding this explicitly extended state.

## B — shop

`Scene_Shop::vUpdate` now calls `AiState::UpdateShop`. The hook emits `shop`
with phases `menu`, `buy`, `sell`, `quantity`, `confirm`, `bought`, `sold`
(and `leave` if observed), with gold throughout. Menu text comes from the
shop's localized terms; buy/sell lists retain native ordering, highlighted
indices and disabled entries. Buy entries contain name/price/count/equipped;
sell entries contain name/price/count. There are no item IDs in the protocol.
Quantity/confirm carry transaction action, item name, count, unit price, total,
and resulting gold (including the engine's gold cap).

Command mapping:

- Menu `choose index=0/1/2`: buy/sell/leave.
- Buy/sell `choose index`: zero-based displayed item row.
- Quantity `choose index=N`: literal quantity N, starting at 1. This freezes
  the native quantity panel and emits `phase: confirm`; no money/items move.
- `confirm` against that emitted confirm state calls the extracted native
  `ExecuteTransaction` routine. Confirm before the summary was emitted is
  rejected. Quantities/items are revalidated before execution.
- `back` from quantity/confirm cancels to buy/sell; other backs follow native
  menu/exit behavior. Choose at confirm is rejected. A decision key cannot
  execute a bridge-staged purchase/sale, either before or after its summary.

Keyboard and bridge share the original `SelectBuyItem`, `SelectSellItem` and
`ExecuteTransaction` bodies in `src/scene_shop.cpp/.h`. Accessors in
`window_shop.h` expose the menu's displayed terms/index; `window_shopnumber.*`
exposes pending values and a bounded quantity setter. Staging flags apply only
to bridge-staged trades; native keyboard-only shopping keeps its original flow.
The command vocabulary is unchanged. Unknown/misplaced commands in shop are
consumed once and rejected, including insufficient money/full inventory,
unsellable items, invalid quantities/indices and stale state IDs.

Verification actually run:

- Incremental native Release build passed.
- Native harness: equip 19 checks; shop fixture 30 checks, including purchase,
  sale, cancellation both ways, no effects at confirm, explicit confirm,
  prevention of native-decision bypass and confirmation before emission,
  duplicate confirm, native bought/sold timers, bad/stale/misplaced/unknown
  commands, insufficient gold and inventory full.
- Real Ruina database and real shop definition: Map0003.lmu, event 23, 22
  goods; 50 checks passed before the additional pre-emission test (repeated
  in final verification). Every good's name/value/count matched the loaded
  database and native windows; purchase, sale and cancellation were exercised.
- `run_shop_compat.py /tmp/followup-b-committed-build /tmp/menu-shop-key-build`:
  131-frame native keyboard-only shop gameplay traces were byte-identical to
  the base build, including gold, inventory and transaction continuation.
- `run_battle.py`: all 77 checks passed. `git diff --check` passed.
- Legacy command differential is also run against imported `c905abf` (see
  final verification). Its old bridge is given an inert shop linkage adapter;
  those scenarios never enter a shop. The independent native shop differential
  links the entire actual base build without adapters.

Intentional observation changes: shop now emits states where the base emitted
none. Those new states and A's extended equip state necessarily differ when
visiting those screens without commands; unchanged legacy paths retain byte
identity. The confirm stage freezes the existing quantity screen rather than
adding a new graphical window. Sell includes the screen's disabled unsellable
rows with `enabled: false` so displayed row indices stay faithful.

Headless limitation: the reduced liblcf dependency was built without ICU and
rejects CP932. The real-data harness uses its supported windows-1252 decoder;
it verifies real goods/prices and native transactions but does not verify
Japanese text rendering/decoding. No live co-play, event-interpreter traversal
to the shop, screenshots, or Windows build was run.

## C — logical key pulse and consumption heartbeat

`key` is the sole added verb. Example command:

```json
{"seq":501,"type":"key","button":"decision"}
```

Buttons are the closed set `decision` and `cancel`. `EnsureInit` samples
`EASYRPG_AI_KEY` once; only the exact string `1` enables pulses. Gate-off
commands are consumed once with rejected reason `key disabled`; invalid or
missing buttons are consumed once with `invalid key button` when enabled.
`PollKeyCommand` has its own input domain, isolated from capture and every
ordinary scene poll. It ignores requested state_id (including missing or
nonnumeric values) and acks the current `g_last_id`, including 0 before any
state emission. Capture behavior and its metadata are unchanged.

`Input::Update` calls that intake while computing logical button state. A
successful command makes the button pressed/triggered/repeated for that input
frame; absent physical input it releases next frame. Consecutive commands each
produce a trigger. A physical hold does not suppress a pulse or have its hold
duration reset. There is no queue, hold API, automatic repeat, or OS event
injection. System-only updates do not consume logical pulses. Each command is
one attempt in one logical input frame, following ordinary input semantics.

`BuildContext` adds top-level `last_seq`; common `Emit` also sets it
authoritatively for every producer. It is the highest seq consumed so far,
including applied, rejected, capture, key and unknown commands. As required for
compatibility, that metadata is excluded from the visible-content duplicate
comparison: heartbeat changes alone do not generate new states or change IDs.
The next emitted state always contains the latest value. This is a consumption
marker on emitted observations, not a periodic timer or acknowledgement file.

Files: `src/ai_state.cpp/.h` (`EnsureInit`, command parser/domain routing,
`PollKeyCommand`, `BuildContext`, `Emit`) and `src/input.cpp` (`Input::Update`).
Checks live in `tests/ai_bridge/key.cpp` / `run_key.py`. The legacy command runner
has an explicit `--allow-last-seq` switch: it asserts heartbeat presence and
normalizes only that top-level state field before comparing the full protocol
trace. It does not normalize any other state changes or ack differences.
Old sources get inert linkage adapters for hooks absent at their revision;
legacy differential scenarios never call those hooks.

Verification actually run:

- Native Release build passed; a separate final clean build is recorded below.
- `run_key.py`: five gate cases (unset, 0, true, 01, 1; 7/7/7/7/6 checks),
  pulse/heartbeat/native-cancel suite 97 checks, disabled bridge 2 checks.
  Coverage includes an unhooked title-equivalent wait, decision and cancel,
  60-frame no-repeat check, consecutive commands, held physical input, raw-key
  isolation, capture/other-domain isolation, exactly one gate-off/invalid ack,
  arbitrary state_id, actual native equip cancel (item to slot, one frame later
  still at slot, second command closes the screen), all 22 current state types
  through common Emit, all ordinary domains/capture/key/unknown consumption,
  monotonic seq, actual name-state heartbeat and preserved duplicate suppression.
- Legacy differential against `c905abf`: 100 scenarios × capture unset/0/1,
  101 checks each, byte-identical after removing **only added state last_seq**.
  Capture 179, unknown-command 881, five capture-gate cases (6 each), disabled
  bridge 2 all passed. Both scene-independent polls leave legacy verbs alone.
- Base/current native differential: byte-identical 131-frame shop gameplay
  trace plus 180 input frames (triggered/repeated/released, physical hold time,
  raw pressed/triggered/released, cardinal/diagonal directions).
- Final workspace fixture rerun: equip 19 and shop 30 checks passed. Real
  Ruina shop rerun: 51 checks passed, including the pre-emission confirm guard.
  Battle-result regression: all 77 checks passed.

Limitations: no interactive title/new-game start, notebook/crafting/picture-wait
session, live screenshot comparison, or non-Linux execution. The headless wait
uses the game's actual Input::Update and trigger queries; the actual native
equip screen separately proves cancel delivery and single-frame behavior.
Japanese decoding/rendering remains unverified as described in B.

## Final clean verification and delivery

Separate configure and full 207-step Release build passed from the isolated
source checkout in `/tmp/menu-shop-key-clean-build`. All six suites were then
rerun against that clean build and exited 0:

```sh
python3 tests/ai_bridge/run_menu_shop_key.py BUILD
python3 tests/ai_bridge/run_menu_shop_key.py BUILD /home/pub/ruina_kit/game
python3 tests/ai_bridge/run_key.py BUILD
python3 tests/ai_bridge/run_commands.py BUILD --baseline c905abf --allow-last-seq
python3 tests/ai_bridge/run_shop_compat.py BASE_BUILD BUILD
python3 tests/ai_bridge/run_battle.py BUILD
```

Results: equip 19; fixture shop 30; real shop 51; key gate 34, key pulse/heartbeat
97, bridge-disabled 2; legacy differential 303 checks; capture 179,
unknown-command 881, capture gates 30, capture-disabled bridge 2; native shop
131 frames plus physical input 180 frames; battle 77. The full standalone
engine unit-test target was not run. Logs use `/tmp/menu-shop-key-clean-*`.
`BASE_BUILD` was `/tmp/followup-b-committed-build`, the existing actual base
build. `git diff --check` passed. Sources in the workspace and isolated checkout
were compared; all delivered files match.

Commits are ordered A, B, C after `c905abf`:

1. `e050e43` — Bridge: complete equipment selection and displayed previews
2. `ee33223` — Bridge: add shop states and explicit staged transaction confirmation
3. Bridge: add gated input-frame key pulses and state consumption heartbeat

`menu_shop_key.bundle` carries exactly this three-commit range, with base
`c905abf` as its prerequisite. Bundle verification and an import into a fresh
checkout at that base passed; the imported tree matches the delivery checkout.
The bundle is a delivery artifact, outside the commits it contains. Import into
a writable checkout already at the base with:

```sh
git fetch /path/to/menu_shop_key.bundle master
git merge --ff-only FETCH_HEAD
```

This workspace's read-only Git metadata still describes the older checkout.
A checkout in that same situation first needs the existing
`followup_fixes.bundle` to reach `c905abf`. The source changes and report are
also present directly in the workspace. No workspace Git metadata was forced.
