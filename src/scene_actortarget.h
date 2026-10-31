/*
 * This file is part of EasyRPG Player.
 *
 * EasyRPG Player is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * EasyRPG Player is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with EasyRPG Player. If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef EP_SCENE_ACTORTARGET_H
#define EP_SCENE_ACTORTARGET_H

// Headers
#include "scene.h"
#include "window_actortarget.h"
#include "window_help.h"
#include "window_targetstatus.h"

/**
 * Scene ActorTarget class.
 * Manages using of Items and Spells.
 */
class Scene_ActorTarget : public Scene {

public:
	/**
	 * Constructor.
	 *
	 * @param item_id item ID of item to use.
	 */
	Scene_ActorTarget(int item_id);

	/**
	 * Constructor.
	 *
	 * @param skill_id skill ID of skill to use.
	 * @param actor_index index of the spell caster in party.
	 */
	Scene_ActorTarget(int skill_id, int actor_index);

	void Start() override;
	void vUpdate() override;

	/**
	 * Ruina text-play (Phase 3-C): apply the pending item/skill to
	 * the actor at the given party index, through the same code the
	 * decision-key branch runs. For whole-party or self targets the
	 * target is preset by the scene and the index is ignored.
	 * Returns false when the use fails (e.g. no items left, not
	 * enough SP).
	 */
	bool AiDecide(int index);

	/** Ruina text-play: the cancel action (back to the item/skill list). */
	bool AiBack();

	/** Ruina text-play: read-only accessors for state output. */
	Window_ActorTarget* AiTargetWindow() { return target_window.get(); }
	int AiId() const { return id; }
	bool AiUseItem() const { return use_item; }
	int AiActorIndex() const { return actor_index; }

	/**
	 * Update function used when an item will be used.
	 */
	void UpdateItem();

	/**
	 * Update function used when a skill will be used.
	 */
	void UpdateSkill();

private:
	/** Contains the actors of the party. */
	std::unique_ptr<Window_ActorTarget> target_window;
	/** Contains the name of the item/skill that will be used. */
	std::unique_ptr<Window_Help> help_window;
	/** Contains quantity/cost of item/spell. */
	std::unique_ptr<Window_TargetStatus> status_window;

	/** ID of item/skill to use. */
	int id;
	/* Index of spell caster in party (only for skills). */
	int actor_index;
	/** True if item, false if skill. */
	bool use_item;
};

#endif
