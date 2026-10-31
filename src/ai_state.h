/*
 * AI state dump / command input for the Ruina text-play project.
 *
 * Phase 1 (read-only): emits the currently displayed message / choice /
 * number-input state as JSON whenever the message window reaches a
 * decision point.
 * Phase 2 (input): accepts commands from a file and applies them to the
 * engine's internal state directly (no simulated key presses):
 *   advance     - release the message pause (next page / close)
 *   choose      - decide the active choice by index
 *   set_name    - set and confirm the name on the name-entry screen
 *   confirm_name- confirm the name-entry screen as-is
 *
 * Everything is enabled only when the environment variable
 * EASYRPG_AI_STATE_DIR names a directory:
 *   state.json    : latest state, replaced atomically
 *   states.jsonl  : append-only log of every emitted state
 *   command.json  : written by the bridge (tmp + rename); polled per frame
 *   ack.json      : latest command disposition, replaced atomically
 *   commands.jsonl: append-only log of every command disposition
 *
 * Commands carry the state id they answer and a sequence number; a
 * command whose state id is not the latest emitted state is rejected
 * as stale and never applied.
 *
 * Only information that is on screen (or inspectable through normal
 * menus) is emitted: displayed text, displayed choices, party status,
 * gold, and the current map's name. No hidden events, switch/variable
 * internals, or future content.
 */

#ifndef EP_AI_STATE_H
#define EP_AI_STATE_H

#include <string>

class PendingMessage;
class Window_Message;
class Scene_Battle_Rpg2k;
class Scene_Menu;
class Scene_Item;
class Scene_Skill;
class Scene_ActorTarget;
class Scene_File;
class Scene_End;
class Scene_Equip;
class Scene_Status;
class Scene_Name;

namespace AiState {

/** Called when a message has been fully assembled for display. */
void OnMessageStart(const PendingMessage& pm, const std::string& assembled_text);

/** Called when a choice selection becomes active. */
void OnChoiceStart(const PendingMessage& pm);

/** Called when a number input becomes active. */
void OnNumberInputStart(const PendingMessage& pm);

/** Called when the name-entry screen opens. current_name is the text
 *  shown in the name box (the default name when pre-filled). */
void OnNameInputStart(const std::string& current_name);

/** Per-frame command polling for the message window (advance / choose). */
void PollMessageCommand(Window_Message* window);

/** Per-frame command polling for the name-entry screen.
 *  Returns true when a command was applied this frame. */
bool PollNameCommand(Scene_Name* scene);

/**
 * Phase 3-A: per-frame exploration hook, called from the map scene.
 * Polls explore-domain commands (move / confirm / goto), executes any
 * queued movement through the player's own movement entry points, and
 * emits an "explore" state once the hero has settled: hero position
 * and facing, plus the currently displayed interaction points (events
 * whose active page shows a sprite and uses an action/touch trigger),
 * referenced by index and tile position only.
 */
void UpdateExplore();

/**
 * Phase 3-B: per-frame battle hook, called from the RPG2k battle
 * scene. Emits battle decision states (option / command / skill /
 * item / enemy or ally target) when the scene waits for input, with
 * the battle narration since the previous decision attached as a
 * compact log. On victory/defeat the result is not emitted as its
 * own state (that would orphan the still-pending result message);
 * it is attached to the next emitted state as "last_battle".
 * Applies "choose" and "back" commands through the scene's own
 * selection dispatch.
 */
void UpdateBattle(Scene_Battle_Rpg2k* scene);

/**
 * Ruina text-play (Phase 3-C): called every frame from the menu
 * family scenes' vUpdate. Each emits its screen as a state (menu /
 * menu_actor, menu_item, menu_skill, menu_target, menu_save,
 * menu_load, menu_end, menu_equip, menu_status) and consumes
 * choose/back commands (domain 4) through the scene's own selection
 * code. The explore state also accepts a "menu" command that opens
 * the menu (Game_Player::SetMenuCalling), like the menu key.
 * Equip/status are state output + back only for now.
 */
void UpdateMenu(Scene_Menu* scene);
void UpdateMenuItem(Scene_Item* scene);
void UpdateMenuSkill(Scene_Skill* scene);
void UpdateActorTarget(Scene_ActorTarget* scene);
void UpdateFile(Scene_File* scene);
void UpdateEnd(Scene_End* scene);
void UpdateEquip(Scene_Equip* scene);
void UpdateStatus(Scene_Status* scene);

} // namespace AiState

#endif
