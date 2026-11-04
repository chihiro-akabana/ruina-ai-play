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
