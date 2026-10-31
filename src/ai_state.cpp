// See ai_state.h for the purpose, wire format, and spoiler policy.

#include "ai_state.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "game_actor.h"
#include "game_battle.h"
#include "game_map.h"
#include "game_party.h"
#include "main_data.h"
#include "pending_message.h"
#include "scene_name.h"
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
};

long g_last_seq = 0;
std::string g_cmd_raw_seen_msg;
std::string g_cmd_raw_seen_name;
bool g_has_pending = false;
Command g_pending;

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

void Emit(nlohmann::json state) {
	EnsureInit();
	if (!g_enabled) {
		return;
	}
	std::string content = state.dump();
	if (content == g_last_content) {
		return; // unchanged state: suppress (token-cost rule from the plan)
	}
	g_last_content = content;

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
}

/**
 * Reads command.json when its content changed. Returns true when a
 * fresh, not-yet-handled command is available. Commands from the other
 * input domain (message window vs. name screen) are left untouched:
 * each poll only consumes commands of its own domain, except that any
 * poll may reject a command addressed to a stale state.
 */
bool ReadFreshCommand(Command& out, bool name_domain) {
	std::ifstream in(g_dir + "/command.json");
	if (!in) {
		return false;
	}
	std::stringstream ss;
	ss << in.rdbuf();
	std::string raw = ss.str();
	// Each domain tracks the file content it has already seen, so a
	// command left for the other domain is still picked up there.
	std::string& seen = name_domain ? g_cmd_raw_seen_name : g_cmd_raw_seen_msg;
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
	if (ReadFreshCommand(cmd, false)) {
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
	if (!ReadFreshCommand(cmd, true)) {
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

} // namespace AiState
