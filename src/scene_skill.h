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

#ifndef EP_SCENE_SKILL_H
#define EP_SCENE_SKILL_H

// Headers
#include <vector>
#include "scene.h"
#include "window_help.h"
#include "window_skill.h"
#include "window_skillstatus.h"

/**
 * Scene_Skill class.
 */
class Scene_Skill : public Scene {

public:
	/**
	 * Constructor.
	 */

	Scene_Skill(std::vector<Game_Actor*> actors, int actor_index, int skill_index = 0);

	void Start() override;
	void Continue(SceneType prev_scene) override;
	void vUpdate() override;
	void TransitionOut(SceneType next_scene) override;

	/**
	 * Ruina text-play (Phase 3-C): use the skill at the given list
	 * index, through the same branch a decision-key press would take.
	 * Returns false for invalid or disabled skills.
	 */
	bool AiDecide(int index);

	/** Ruina text-play: the cancel action (back to the menu). */
	bool AiBack();

	/** Ruina text-play: read-only accessors for state output. */
	Window_Skill* AiSkillWindow() { return skill_window.get(); }
	Game_Actor* AiActor() {
		return actor_index >= 0 && actor_index < static_cast<int>(actors.size()) ? actors[actor_index] : nullptr;
	}

private:
	std::vector<Game_Actor*> actors;
	/** Actor in the vector whose skills are displayed. */
	int actor_index;
	/** Skill to select at startup. */
	int skill_index;
	/** Displays available skills. */
	std::unique_ptr<Window_Skill> skill_window;
	/** Displays information about the actor. */
	std::unique_ptr<Window_SkillStatus> skillstatus_window;
	/** Displays description about the selected skill. */
	std::unique_ptr<Window_Help> help_window;
};

#endif
