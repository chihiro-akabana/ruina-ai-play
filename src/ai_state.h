/*
 * AI state dump for the Ruina text-play project (Phase 1: read-only).
 *
 * Emits the currently displayed message / choice / number-input state as
 * JSON whenever the message window reaches a decision point. Output goes
 * to the directory named by the environment variable EASYRPG_AI_STATE_DIR
 * (unset = disabled):
 *   - state.json   : latest state, replaced atomically
 *   - states.jsonl : append-only log of every emitted state
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

namespace AiState {

/** Called when a message has been fully assembled for display. */
void OnMessageStart(const PendingMessage& pm, const std::string& assembled_text);

/** Called when a choice selection becomes active. */
void OnChoiceStart(const PendingMessage& pm);

/** Called when a number input becomes active. */
void OnNumberInputStart(const PendingMessage& pm);

} // namespace AiState

#endif
