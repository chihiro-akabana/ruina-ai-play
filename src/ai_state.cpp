// See ai_state.h for the purpose, wire format, and spoiler policy.

#include "ai_state.h"

#include <algorithm>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <queue>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include <lcf/reader_util.h>

#include "game_actor.h"
#include "game_battle.h"
#include "game_battler.h"
#include "game_enemyparty.h"
#include "game_event.h"
#include "game_map.h"
#include "game_message.h"
#include "game_party.h"
#include "game_player.h"
#include "main_data.h"
#include "pending_message.h"
#include "scene_battle_rpg2k.h"
#include "scene_name.h"
#include "window_command.h"
#include "window_message.h"

namespace AiState {
namespace {

bool g_initialized = false;
bool g_enabled = false;
std::string g_dir;
std::string g_last_content;
long g_counter = 0;

// Latest emitted state, used to validate incoming commands.
long g_last_id = 0;
std::string g_last_type;
std::vector<bool> g_last_choice_enabled;

struct Command {
	long seq = 0;
	long state_id = 0;
	std::string type;
	int index = -1;
	bool has_name = false;
	std::string name;
	// explore-domain fields
	std::string dir;
	int steps = 0;
	int point = -1;
};

long g_last_seq = 0;
// Command domains: 0 = message window, 1 = name screen, 2 = exploration,
// 3 = battle selections.
std::string g_cmd_raw_seen[4];
bool g_has_pending = false;
Command g_pending;

// Exploration (Phase 3-A) bookkeeping.
struct PointInfo {
	int x = 0;
	int y = 0;
	int trigger = 0; // lcf::rpg::EventPage::Trigger value
	int layer = 0;   // lcf::rpg::EventPage layer: 0 below, 1 same, 2 above
};
std::vector<PointInfo> g_explore_points; // points of the last emitted explore state
std::deque<int> g_move_queue;            // pending step directions (lcf Direction values)
bool g_has_move_cmd = false;
Command g_move_cmd;
bool g_goto_confirm = false;             // after the queue: face the point and confirm
bool g_goto_face = true;                 // false for below-layer points: confirm standing on them
int g_goto_tx = -1;
int g_goto_ty = -1;
int g_idle_frames = 0;
int g_last_hx = -1;
int g_last_hy = -1;

// Battle (Phase 3-B) bookkeeping.
std::vector<std::string> g_battle_prev_lines;
std::vector<std::string> g_battle_log;
bool g_battle_result_sent = false;
// The finished battle's result, attached to the next emitted state
// (whatever it is) as "last_battle" instead of being emitted as a
// state of its own: a standalone result state would displace a
// still-pending victory/defeat message state and orphan it.
bool g_battle_pending = false;
std::string g_battle_pending_result;
std::vector<std::string> g_battle_pending_log;
bool g_battle_active = false;
long g_battle_seq = 0;

/**
 * Removes presentation-only control codes (\c[..], \s[..], \>, \!, ...)
 * from a message line. Text-inserting commands (\v[..], \n[..]) have
 * already been substituted by PendingMessage before this point.
 * The game text is UTF-8; the escape character and brackets are ASCII,
 * so byte-wise scanning never splits a multi-byte character.
 */
std::string StripControlCodes(const std::string& line) {
	std::string out;
	out.reserve(line.size());
	for (size_t i = 0; i < line.size();) {
		char c = line[i];
		if (c == '\f') { // page-break marker, handled by page splitting
			++i;
			continue;
		}
		if (c == '\\' && i + 1 < line.size()) {
			char d = line[i + 1];
			if (d == '\\') { // escaped backslash is displayed as-is
				out.push_back('\\');
				i += 2;
				continue;
			}
			if (i + 2 < line.size() && line[i + 2] == '[') {
				// \x[...] form: drop through the closing bracket
				size_t close = line.find(']', i + 3);
				i = (close == std::string::npos) ? line.size() : close + 1;
				continue;
			}
			// single-letter code (\>, \<, \!, \^, \., \|, \_, \$): drop both bytes
			i += 2;
			continue;
		}
		out.push_back(c);
		++i;
	}
	return out;
}

std::vector<std::string> StripLines(const std::vector<std::string>& lines) {
	std::vector<std::string> out;
	for (const auto& line : lines) {
		out.push_back(StripControlCodes(line));
	}
	while (!out.empty() && out.back().empty()) {
		out.pop_back();
	}
	return out;
}

std::string JoinLines(const std::vector<std::string>& lines) {
	std::string out;
	for (size_t i = 0; i < lines.size(); ++i) {
		if (i > 0) {
			out.push_back('\n');
		}
		out += lines[i];
	}
	return out;
}

nlohmann::json BuildContext() {
	nlohmann::json ctx;
	ctx["scene"] = Game_Battle::IsBattleRunning() ? "battle" : "map";
	ctx["location"] = std::string(Game_Map::GetMapName(Game_Map::GetMapId()));

	nlohmann::json party = nlohmann::json::array();
	if (Main_Data::game_party) {
		ctx["gold"] = Main_Data::game_party->GetGold();
		for (const Game_Actor* actor : Main_Data::game_party->GetActors()) {
			if (actor == nullptr) {
				continue;
			}
			nlohmann::json a;
			a["name"] = std::string(actor->GetName());
			a["level"] = actor->GetLevel();
			a["hp"] = actor->GetHp();
			a["max_hp"] = actor->GetMaxHp();
			a["sp"] = actor->GetSp();
			a["max_sp"] = actor->GetMaxSp();
			party.push_back(std::move(a));
		}
	} else {
		ctx["gold"] = nullptr;
	}
	ctx["party"] = std::move(party);
	return ctx;
}

void EnsureInit() {
	if (g_initialized) {
		return;
	}
	g_initialized = true;
	const char* dir = std::getenv("EASYRPG_AI_STATE_DIR");
	if (dir == nullptr || dir[0] == '\0') {
		g_enabled = false;
		return;
	}
	g_dir = dir;
	std::error_code ec;
	std::filesystem::create_directories(g_dir, ec);
	g_enabled = !ec;
}

void WriteFileAtomic(const std::string& final_path, const std::string& content) {
	const std::string tmp_path = final_path + ".tmp";
	{
		std::ofstream out(tmp_path, std::ios::trunc);
		out << content;
	}
	std::error_code ec;
	std::filesystem::rename(tmp_path, final_path, ec);
}

void AppendLine(const std::string& path, const std::string& line) {
	std::ofstream log(path, std::ios::app);
	log << line << '\n';
}

void WriteAck(const Command& cmd, const char* status, const char* reason) {
	nlohmann::json ack;
	ack["seq"] = cmd.seq;
	ack["state_id"] = cmd.state_id;
	ack["type"] = cmd.type;
	ack["status"] = status;
	if (reason != nullptr && reason[0] != '\0') {
		ack["reason"] = reason;
	}
	std::string line = ack.dump();
	AppendLine(g_dir + "/commands.jsonl", line);
	WriteFileAtomic(g_dir + "/ack.json", line + "\n");
}

void DropPending(const char* reason) {
	if (!g_has_pending) {
		return;
	}
	WriteAck(g_pending, "dropped", reason);
	g_has_pending = false;
}

bool Emit(nlohmann::json state) {
	EnsureInit();
	if (!g_enabled) {
		return false;
	}
	if (g_battle_pending && !state.contains("last_battle")) {
		nlohmann::json bundle;
		bundle["result"] = g_battle_pending_result;
		bundle["battle_seq"] = g_battle_seq;
		bundle["log"] = g_battle_pending_log;
		state["last_battle"] = std::move(bundle);
	}
	std::string content = state.dump();
	if (content == g_last_content) {
		return false; // unchanged state: suppress (token-cost rule from the plan)
	}
	g_last_content = content;
	if (state.contains("last_battle")) {
		g_battle_pending = false;
		g_battle_pending_log.clear();
	}

	DropPending("state changed");

	state["id"] = ++g_counter;
	g_last_id = g_counter;
	g_last_type = state.value("type", "");
	g_last_choice_enabled.clear();
	if (state.contains("choices")) {
		for (const auto& c : state["choices"]) {
			g_last_choice_enabled.push_back(c.value("enabled", true));
		}
	}
	std::string line = state.dump();

	AppendLine(g_dir + "/states.jsonl", line);
	WriteFileAtomic(g_dir + "/state.json", line + "\n");
	return true;
}

/**
 * Reads command.json when its content changed. Returns true when a
 * fresh, not-yet-handled command is available. Commands from the other
 * input domain (message window vs. name screen) are left untouched:
 * each poll only consumes commands of its own domain, except that any
 * poll may reject a command addressed to a stale state.
 */
bool ReadFreshCommand(Command& out, int domain) {
	std::ifstream in(g_dir + "/command.json");
	if (!in) {
		return false;
	}
	std::stringstream ss;
	ss << in.rdbuf();
	std::string raw = ss.str();
	// Each domain tracks the file content it has already seen, so a
	// command left for another domain is still picked up there.
	std::string& seen = g_cmd_raw_seen[domain];
	if (raw.empty() || raw == seen) {
		return false;
	}
	seen = raw;

	auto parsed = nlohmann::json::parse(raw, nullptr, false);
	if (parsed.is_discarded() || !parsed.is_object()) {
		return false;
	}
	Command cmd;
	cmd.seq = parsed.value("seq", 0L);
	cmd.state_id = parsed.value("state_id", 0L);
	cmd.type = parsed.value("type", "");
	cmd.index = parsed.value("index", -1);
	if (parsed.contains("name") && parsed["name"].is_string()) {
		cmd.has_name = true;
		cmd.name = parsed["name"].get<std::string>();
	}
	cmd.dir = parsed.value("dir", "");
	cmd.steps = parsed.value("steps", 0);
	cmd.point = parsed.value("point", -1);
	if (cmd.seq <= 0 || cmd.seq <= g_last_seq || cmd.type.empty()) {
		return false; // duplicate write or malformed: not a fresh command
	}
	out = cmd;
	return true;
}

void Consume(const Command& cmd, const char* status, const char* reason) {
	g_last_seq = cmd.seq;
	WriteAck(cmd, status, reason);
}

} // namespace

void OnMessageStart(const PendingMessage& pm, const std::string& assembled_text) {
	EnsureInit();
	if (!g_enabled || pm.HasChoices() || pm.HasNumberInput()) {
		// Choice / number-input states are emitted from their own hooks,
		// at the moment the selection actually becomes active.
		return;
	}

	// Split the assembled text into pages (\f) and lines (\n), exactly as
	// the window will show them.
	nlohmann::json pages = nlohmann::json::array();
	std::vector<std::string> all_lines;
	size_t page_begin = 0;
	while (page_begin <= assembled_text.size()) {
		size_t page_end = assembled_text.find('\f', page_begin);
		if (page_end == std::string::npos) {
			page_end = assembled_text.size();
		}
		std::vector<std::string> raw_lines;
		size_t line_begin = page_begin;
		while (line_begin < page_end) {
			size_t line_end = assembled_text.find('\n', line_begin);
			if (line_end == std::string::npos || line_end > page_end) {
				line_end = page_end;
			}
			raw_lines.push_back(assembled_text.substr(line_begin, line_end - line_begin));
			line_begin = line_end + 1;
		}
		if (page_begin == page_end && page_begin != 0) {
			// trailing empty segment after the final \f
		} else {
			auto lines = StripLines(raw_lines);
			if (!lines.empty()) {
				nlohmann::json page = nlohmann::json::array();
				for (const auto& l : lines) {
					page.push_back(l);
					all_lines.push_back(l);
				}
				pages.push_back(std::move(page));
			}
		}
		if (page_end == assembled_text.size()) {
			break;
		}
		page_begin = page_end + 1;
	}
	if (all_lines.empty()) {
		return;
	}

	nlohmann::json state = BuildContext();
	state["type"] = "message";
	state["text"] = JoinLines(all_lines);
	state["pages"] = std::move(pages);
	Emit(std::move(state));
}

void OnChoiceStart(const PendingMessage& pm) {
	EnsureInit();
	if (!g_enabled || !pm.HasChoices()) {
		return;
	}
	const auto& lines = pm.GetLines();
	const int start = pm.GetChoiceStartLine();

	std::vector<std::string> context_lines;
	for (int i = 0; i < start && i < static_cast<int>(lines.size()); ++i) {
		context_lines.push_back(lines[i]);
	}
	context_lines = StripLines(context_lines);

	nlohmann::json choices = nlohmann::json::array();
	for (int i = start; i < static_cast<int>(lines.size()); ++i) {
		nlohmann::json choice;
		choice["index"] = i - start;
		choice["text"] = StripControlCodes(lines[i]);
		choice["enabled"] = pm.IsChoiceEnabled(i - start);
		choices.push_back(std::move(choice));
	}

	nlohmann::json state = BuildContext();
	state["type"] = "choice";
	state["text"] = JoinLines(context_lines);
	state["choices"] = std::move(choices);
	state["cancel_type"] = pm.GetChoiceCancelType();
	Emit(std::move(state));
}

void OnNumberInputStart(const PendingMessage& pm) {
	EnsureInit();
	if (!g_enabled || !pm.HasNumberInput()) {
		return;
	}
	const auto& lines = pm.GetLines();
	std::vector<std::string> context_lines;
	for (const auto& line : lines) {
		context_lines.push_back(line);
	}
	context_lines = StripLines(context_lines);

	nlohmann::json state = BuildContext();
	state["type"] = "number_input";
	state["text"] = JoinLines(context_lines);
	state["digits"] = pm.GetNumberInputDigits();
	Emit(std::move(state));
}

void OnNameInputStart(const std::string& current_name) {
	EnsureInit();
	if (!g_enabled) {
		return;
	}
	nlohmann::json state = BuildContext();
	state["type"] = "name_input";
	state["text"] = "";
	state["current_name"] = current_name;
	Emit(std::move(state));
}

void PollMessageCommand(Window_Message* window) {
	EnsureInit();
	if (!g_enabled || window == nullptr) {
		return;
	}

	Command cmd;
	if (ReadFreshCommand(cmd, 0)) {
		if (cmd.state_id != g_last_id) {
			Consume(cmd, "rejected", "stale state_id");
		} else if (cmd.type == "advance") {
			if (g_last_type == "message") {
				DropPending("superseded");
				g_pending = cmd;
				g_has_pending = true;
				Consume(cmd, "accepted", "");
			} else {
				Consume(cmd, "rejected", "state is not a message");
			}
		} else if (cmd.type == "choose") {
			if (g_last_type != "choice") {
				Consume(cmd, "rejected", "state is not a choice");
			} else if (cmd.index < 0 || cmd.index >= static_cast<int>(g_last_choice_enabled.size())) {
				Consume(cmd, "rejected", "choice index out of range");
			} else if (!g_last_choice_enabled[cmd.index]) {
				Consume(cmd, "rejected", "choice is disabled");
			} else {
				DropPending("superseded");
				g_pending = cmd;
				g_has_pending = true;
				Consume(cmd, "accepted", "");
			}
		}
		// Commands of the name-input domain are left for PollNameCommand.
	}

	if (g_has_pending) {
		bool applied = false;
		if (g_pending.type == "advance") {
			applied = window->AiAdvance();
		} else if (g_pending.type == "choose") {
			applied = window->AiChoose(g_pending.index);
		}
		if (applied) {
			WriteAck(g_pending, "applied", "");
			g_has_pending = false;
		}
	}
}

bool PollNameCommand(Scene_Name* scene) {
	EnsureInit();
	if (!g_enabled || scene == nullptr) {
		return false;
	}

	Command cmd;
	if (!ReadFreshCommand(cmd, 1)) {
		return false;
	}
	if (cmd.state_id != g_last_id) {
		Consume(cmd, "rejected", "stale state_id");
		return false;
	}
	if (cmd.type != "set_name" && cmd.type != "confirm_name") {
		return false; // not this domain
	}
	if (g_last_type != "name_input") {
		Consume(cmd, "rejected", "state is not a name_input");
		return false;
	}
	const std::string* name = nullptr;
	if (cmd.type == "set_name") {
		if (!cmd.has_name) {
			Consume(cmd, "rejected", "set_name without a name");
			return false;
		}
		name = &cmd.name;
	}
	bool applied = scene->AiConfirmName(name);
	if (applied) {
		Consume(cmd, "applied", "");
	} else {
		Consume(cmd, "rejected", "name confirm failed");
	}
	return applied;
}

namespace {

const char* DirName(int dir) {
	switch (dir) {
		case 0: return "up";
		case 1: return "right";
		case 2: return "down";
		case 3: return "left";
		default: return "unknown";
	}
}

int DirFromName(const std::string& name) {
	if (name == "up") return 0;
	if (name == "right") return 1;
	if (name == "down") return 2;
	if (name == "left") return 3;
	return -1;
}

// The currently displayed interaction points: events whose active page
// shows a sprite and uses an action/touch trigger. Identified by tile
// position only; event names, ids, and page conditions stay internal.
std::vector<PointInfo> ComputePoints() {
	std::vector<PointInfo> pts;
	for (const Game_Event& ev : Game_Map::GetEvents()) {
		const auto* page = ev.GetActivePage();
		if (page == nullptr) {
			continue;
		}
		if (ev.GetSpriteName().empty()) {
			continue;
		}
		const int trig = static_cast<int>(ev.GetTrigger());
		if (trig != static_cast<int>(lcf::rpg::EventPage::Trigger_action)
				&& trig != static_cast<int>(lcf::rpg::EventPage::Trigger_touched)
				&& trig != static_cast<int>(lcf::rpg::EventPage::Trigger_collision)) {
			continue;
		}
		pts.push_back(PointInfo{ev.GetX(), ev.GetY(), trig, static_cast<int>(page->layer)});
	}
	std::sort(pts.begin(), pts.end(), [](const PointInfo& a, const PointInfo& b) {
		return a.y != b.y ? a.y < b.y : a.x < b.x;
	});
	return pts;
}

// True when the engine would accept player input right now.
bool ExploreCanAct() {
	auto* player = Main_Data::game_player.get();
	if (player == nullptr || player->IsPendingTeleport()) {
		return false;
	}
	if (Game_Message::IsMessageActive() || Game_Message::IsMessagePending()) {
		return false;
	}
	if (Game_Map::GetInterpreter().IsRunning() || Game_Map::IsAnyEventStarting()) {
		return false;
	}
	return true;
}

// Breadth-first path over engine passability (Game_Character::MakeWay,
// the same test movement itself uses). Fills out_dirs with the step
// directions from (sx, sy) to any of the target tiles.
bool FindPath(int sx, int sy, const std::vector<std::pair<int, int>>& targets, std::deque<int>& out_dirs) {
	auto* player = Main_Data::game_player.get();
	if (player == nullptr) {
		return false;
	}
	std::map<std::pair<int, int>, bool> target_set;
	for (const auto& t : targets) {
		target_set[t] = true;
	}
	const int w = Game_Map::GetTilesX();
	const int h = Game_Map::GetTilesY();
	std::map<std::pair<int, int>, std::pair<std::pair<int, int>, int>> parent;
	std::queue<std::pair<int, int>> q;
	parent[{sx, sy}] = {{-1, -1}, -1};
	q.push({sx, sy});
	std::pair<int, int> goal{-1, -1};
	while (!q.empty()) {
		auto cur = q.front();
		q.pop();
		if (target_set.count(cur) > 0) {
			goal = cur;
			break;
		}
		for (int dir = 0; dir < 4; ++dir) {
			int nx = cur.first + Game_Character::GetDxFromDirection(dir);
			int ny = cur.second + Game_Character::GetDyFromDirection(dir);
			if (nx < 0 || ny < 0 || nx >= w || ny >= h) {
				continue;
			}
			if (parent.count({nx, ny}) > 0) {
				continue;
			}
			if (!player->MakeWay(cur.first, cur.second, nx, ny)) {
				continue;
			}
			parent[{nx, ny}] = {cur, dir};
			q.push({nx, ny});
		}
	}
	if (goal.first < 0) {
		return false;
	}
	std::deque<int> reversed;
	auto cur = goal;
	while (cur.first != sx || cur.second != sy) {
		auto it = parent.find(cur);
		if (it == parent.end()) {
			return false;
		}
		reversed.push_front(it->second.second);
		cur = it->second.first;
	}
	out_dirs = reversed;
	return true;
}

void ClearMoveCmd(const char* status, const char* reason) {
	if (status != nullptr) {
		WriteAck(g_move_cmd, status, reason);
	}
	g_move_queue.clear();
	g_has_move_cmd = false;
	g_goto_confirm = false;
	g_goto_face = true;
}

} // namespace

void UpdateExplore() {
	EnsureInit();
	if (!g_enabled) {
		return;
	}
	auto* player = Main_Data::game_player.get();
	if (player == nullptr || player->IsPendingTeleport()) {
		return;
	}

	// 1) Execute / finish / drop an in-flight movement command.
	if (g_has_move_cmd) {
		const bool interrupted = Game_Map::GetInterpreter().IsRunning()
				|| Game_Message::IsMessageActive() || Game_Message::IsMessagePending();
		if (interrupted && !g_move_queue.empty()) {
			ClearMoveCmd("dropped", "interrupted by an event");
		} else if (!g_move_queue.empty() && ExploreCanAct() && !player->IsMoving()) {
			int dir = g_move_queue.front();
			g_move_queue.pop_front();
			if (!player->Move(dir)) {
				ClearMoveCmd("dropped", "path blocked");
			}
		}
		if (g_has_move_cmd && g_move_queue.empty() && !player->IsMoving() && ExploreCanAct()) {
			if (g_goto_confirm) {
				if (g_goto_face) {
					int dx = g_goto_tx - player->GetX();
					int dy = g_goto_ty - player->GetY();
					int dir = (dx == 1) ? 1 : (dx == -1) ? 3 : (dy == 1) ? 2 : (dy == -1) ? 0 : -1;
					if (dir >= 0) {
						player->SetDirection(dir);
					}
				}
				player->AiCheckActionEvent();
			}
			ClearMoveCmd("applied", "");
		}
	}

	// 2) Take in a fresh explore-domain command.
	Command cmd;
	if (ReadFreshCommand(cmd, 2)) {
		if (cmd.state_id != g_last_id) {
			Consume(cmd, "rejected", "stale state_id");
		} else if (cmd.type == "move" || cmd.type == "confirm" || cmd.type == "goto") {
			if (g_last_type != "explore") {
				Consume(cmd, "rejected", "state is not an explore state");
			} else if (g_has_move_cmd) {
				Consume(cmd, "rejected", "movement in progress");
			} else if (cmd.type == "confirm") {
				bool hit = ExploreCanAct() && !player->IsMoving() && player->AiCheckActionEvent();
				Consume(cmd, hit ? "applied" : "rejected", hit ? "" : "nothing triggered");
			} else if (cmd.type == "move") {
				int dir = DirFromName(cmd.dir);
				if (dir < 0 || cmd.steps < 1 || cmd.steps > 60) {
					Consume(cmd, "rejected", "invalid move (dir or steps)");
				} else {
					for (int i = 0; i < cmd.steps; ++i) {
						g_move_queue.push_back(dir);
					}
					g_move_cmd = cmd;
					g_has_move_cmd = true;
					g_goto_confirm = false;
					Consume(cmd, "accepted", "");
				}
			} else { // goto
				if (cmd.point < 0 || cmd.point >= static_cast<int>(g_explore_points.size())) {
					Consume(cmd, "rejected", "point index out of range");
				} else {
					const PointInfo& pt = g_explore_points[cmd.point];
					std::vector<std::pair<int, int>> targets;
					const bool is_action = pt.trigger == static_cast<int>(lcf::rpg::EventPage::Trigger_action);
					// Below-layer action events (Ruina's points) are
					// triggered standing ON them; same/above-layer ones
					// by facing them from an adjacent tile.
					const bool step_on = !is_action || pt.layer == 0;
					if (is_action && !step_on) {
						for (int dir = 0; dir < 4; ++dir) {
							targets.push_back({pt.x + Game_Character::GetDxFromDirection(dir),
									pt.y + Game_Character::GetDyFromDirection(dir)});
						}
					} else {
						targets.push_back({pt.x, pt.y});
					}
					std::deque<int> path;
					if (!FindPath(player->GetX(), player->GetY(), targets, path)) {
						Consume(cmd, "rejected", "point unreachable");
					} else {
						g_move_queue = path;
						g_move_cmd = cmd;
						g_has_move_cmd = true;
						g_goto_confirm = is_action;
						g_goto_face = !step_on;
						g_goto_tx = pt.x;
						g_goto_ty = pt.y;
						Consume(cmd, "accepted", "");
					}
				}
			}
		}
		// Commands of other domains are left for their own polls.
	}

	// 3) Emit an explore state once the hero has settled.
	const int hx = player->GetX();
	const int hy = player->GetY();
	if (hx == g_last_hx && hy == g_last_hy && !player->IsMoving()
			&& g_move_queue.empty() && !g_has_move_cmd && ExploreCanAct()) {
		++g_idle_frames;
	} else {
		g_idle_frames = 0;
		g_last_hx = hx;
		g_last_hy = hy;
	}
	if (g_idle_frames >= 15) {
		g_idle_frames = 0;
		auto pts = ComputePoints();
		nlohmann::json state = BuildContext();
		state["type"] = "explore";
		nlohmann::json hero;
		hero["x"] = hx;
		hero["y"] = hy;
		hero["facing"] = DirName(player->GetDirection());
		state["hero"] = std::move(hero);
		nlohmann::json arr = nlohmann::json::array();
		for (size_t i = 0; i < pts.size(); ++i) {
			nlohmann::json p;
			p["index"] = static_cast<int>(i);
			p["x"] = pts[i].x;
			p["y"] = pts[i].y;
			p["dx"] = pts[i].x - hx;
			p["dy"] = pts[i].y - hy;
			p["trigger"] = pts[i].trigger == static_cast<int>(lcf::rpg::EventPage::Trigger_action) ? "action" : "touch";
			p["layer"] = pts[i].layer == 0 ? "below" : pts[i].layer == 2 ? "above" : "same";
			arr.push_back(std::move(p));
		}
		state["points"] = std::move(arr);
		if (Emit(std::move(state))) {
			g_explore_points = std::move(pts);
		}
	}
}

namespace {

bool IsBattleDecisionType(const std::string& t) {
	return t == "battle_option" || t == "battle_command" || t == "battle_skill"
			|| t == "battle_item" || t == "battle_target" || t == "battle_target_ally";
}

// Accumulate the battle narration lines (damage, results, dice-like
// outcomes) shown since the last snapshot of the battle message
// window. The window holds one page of lines and is cleared between
// phases; a cleared window restarts the accumulation.
void CaptureBattleLog(Scene_Battle_Rpg2k* scene) {
	auto* win = scene->AiBattleMessageWindow();
	if (win == nullptr) {
		return;
	}
	std::vector<std::string> clean;
	for (const std::string& line : win->GetLines()) {
		std::string c = StripControlCodes(line);
		if (!c.empty()) {
			clean.push_back(c);
		}
	}
	if (clean == g_battle_prev_lines) {
		return;
	}
	const bool prefix = clean.size() > g_battle_prev_lines.size()
			&& std::equal(g_battle_prev_lines.begin(), g_battle_prev_lines.end(), clean.begin());
	if (prefix) {
		for (size_t i = g_battle_prev_lines.size(); i < clean.size(); ++i) {
			g_battle_log.push_back(clean[i]);
		}
	} else {
		for (const auto& l : clean) {
			g_battle_log.push_back(l);
		}
	}
	while (g_battle_log.size() > 60) {
		g_battle_log.erase(g_battle_log.begin());
	}
	g_battle_prev_lines = std::move(clean);
}

nlohmann::json WindowChoices(Window_Command* win) {
	nlohmann::json arr = nlohmann::json::array();
	if (win == nullptr) {
		return arr;
	}
	for (size_t i = 0; i < win->AiCommands().size(); ++i) {
		nlohmann::json c;
		c["index"] = static_cast<int>(i);
		c["text"] = win->AiCommands()[i];
		c["enabled"] = i >= win->AiCommandsEnabled().size() || win->AiCommandsEnabled()[i];
		arr.push_back(std::move(c));
	}
	return arr;
}

nlohmann::json BattleEnemies() {
	nlohmann::json arr = nlohmann::json::array();
	std::vector<Game_Battler*> battlers;
	Main_Data::game_enemyparty->GetActiveBattlers(battlers);
	for (auto* b : battlers) {
		nlohmann::json e;
		e["name"] = b->GetName();
		arr.push_back(std::move(e));
	}
	return arr;
}

} // namespace

void UpdateBattle(Scene_Battle_Rpg2k* scene) {
	EnsureInit();
	if (!g_enabled || scene == nullptr) {
		return;
	}
	const int st = scene->AiBattleState();
	const int sub = scene->AiBattleSubState();
	const int kStart = static_cast<int>(Scene_Battle::State_Start);
	const int kVictory = static_cast<int>(Scene_Battle::State_Victory);
	const int kDefeat = static_cast<int>(Scene_Battle::State_Defeat);

	if (st == kStart && !g_battle_active) {
		g_battle_active = true;
		++g_battle_seq;
		g_battle_result_sent = false;
		g_battle_log.clear();
		g_battle_prev_lines.clear();
	}

	CaptureBattleLog(scene);

	// At victory / defeat, stash the result for the next emitted
	// state (see g_battle_pending). Emitting a standalone result
	// state here would orphan the victory/defeat message state that
	// was emitted just before, while it still waits for "advance".
	// (Escape resolves through the following states instead: a failed
	// escape returns to the option state, a successful one ends the
	// scene and the map states resume.)
	if ((st == kVictory || st == kDefeat) && !g_battle_result_sent) {
		g_battle_result_sent = true;
		g_battle_pending = true;
		g_battle_pending_result = st == kVictory ? "victory" : "defeat";
		g_battle_pending_log = g_battle_log;
		g_battle_log.clear();
	}

	// Decision states: built every frame while the scene waits for
	// input; Emit's content comparison suppresses the repeats, so a
	// state goes out exactly when something changed (party status,
	// choices, or fresh narration in the log). The log is cleared
	// only when the state was actually emitted.
	std::string kind;
	nlohmann::json choices;
	Game_Actor* actor = scene->AiActiveActor();
	if (st == static_cast<int>(Scene_Battle::State_SelectOption) && sub == 3) {
		kind = "battle_option";
		choices = WindowChoices(scene->AiOptionsWindow());
	} else if (st == static_cast<int>(Scene_Battle::State_SelectCommand) && sub == 1) {
		kind = "battle_command";
		choices = WindowChoices(scene->AiCommandWindow());
	} else if (st == static_cast<int>(Scene_Battle::State_SelectSkill) && sub == 1) {
		kind = "battle_skill";
		choices = nlohmann::json::array();
		auto* win = scene->AiSkillWindow();
		if (win != nullptr && actor != nullptr) {
			for (size_t i = 0; i < win->AiData().size(); ++i) {
				const int skill_id = win->AiData()[i];
				const auto* skill = lcf::ReaderUtil::GetElement(lcf::Data::skills, skill_id);
				nlohmann::json c;
				c["index"] = static_cast<int>(i);
				c["text"] = skill != nullptr ? skill->name : "";
				c["cost"] = actor->CalculateSkillCost(skill_id);
				c["enabled"] = win->CheckEnable(skill_id);
				choices.push_back(std::move(c));
			}
		}
	} else if (st == static_cast<int>(Scene_Battle::State_SelectItem) && sub == 1) {
		kind = "battle_item";
		choices = nlohmann::json::array();
		auto* win = scene->AiItemWindow();
		if (win != nullptr) {
			for (size_t i = 0; i < win->AiData().size(); ++i) {
				const int item_id = win->AiData()[i];
				const auto* item = lcf::ReaderUtil::GetElement(lcf::Data::items, item_id);
				nlohmann::json c;
				c["index"] = static_cast<int>(i);
				c["text"] = item != nullptr ? item->name : "";
				c["count"] = item_id > 0 ? Main_Data::game_party->GetItemCount(item_id) : 0;
				c["enabled"] = item_id > 0 && win->CheckEnable(item_id);
				choices.push_back(std::move(c));
			}
		}
	} else if (st == static_cast<int>(Scene_Battle::State_SelectEnemyTarget) && sub == 1) {
		kind = "battle_target";
		choices = WindowChoices(scene->AiTargetWindow());
	} else if (st == static_cast<int>(Scene_Battle::State_SelectAllyTarget) && sub == 1) {
		kind = "battle_target_ally";
		choices = nlohmann::json::array();
		int idx = 0;
		for (auto* member : Main_Data::game_party->GetActors()) {
			nlohmann::json c;
			c["index"] = idx++;
			c["text"] = member->GetName();
			c["hp"] = member->GetHp();
			c["max_hp"] = member->GetMaxHp();
			c["enabled"] = true;
			choices.push_back(std::move(c));
		}
	}

	if (!kind.empty()) {
		nlohmann::json state = BuildContext();
		state["type"] = kind;
		state["choices"] = choices;
		state["enemies"] = BattleEnemies();
		if (actor != nullptr && (kind == "battle_command" || kind == "battle_skill" || kind == "battle_item")) {
			nlohmann::json a;
			a["name"] = actor->GetName();
			a["hp"] = actor->GetHp();
			a["max_hp"] = actor->GetMaxHp();
			a["sp"] = actor->GetSp();
			a["max_sp"] = actor->GetMaxSp();
			state["actor"] = std::move(a);
		}
		if (!g_battle_log.empty()) {
			state["log"] = g_battle_log;
		}
		if (Emit(std::move(state))) {
			g_battle_log.clear();
		}
	}

	// Command intake (domain 3). "choose" is shared with the message
	// domain: only consume it when the latest state is a battle
	// decision state, otherwise leave it for the message window.
	Command cmd;
	if (ReadFreshCommand(cmd, 3)) {
		if (cmd.state_id != g_last_id) {
			Consume(cmd, "rejected", "stale state_id");
		} else if (cmd.type == "choose" && IsBattleDecisionType(g_last_type)) {
			bool ok = cmd.index >= 0 && scene->AiDecideSelection(cmd.index);
			Consume(cmd, ok ? "applied" : "rejected", ok ? "" : "selection failed");
		} else if (cmd.type == "back") {
			if (IsBattleDecisionType(g_last_type)) {
				bool ok = scene->AiBack();
				Consume(cmd, ok ? "applied" : "rejected", ok ? "" : "cannot go back here");
			} else {
				Consume(cmd, "rejected", "state is not a battle selection");
			}
		}
		// Other command types belong to other domains.
	}
}

} // namespace AiState
