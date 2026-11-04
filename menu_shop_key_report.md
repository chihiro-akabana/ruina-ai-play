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
