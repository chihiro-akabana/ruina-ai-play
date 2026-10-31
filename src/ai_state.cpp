// See ai_state.h for the purpose and the spoiler policy of this module.

#include "ai_state.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "game_actor.h"
#include "game_battle.h"
#include "game_map.h"
#include "game_party.h"
#include "main_data.h"
#include "pending_message.h"

namespace AiState {
namespace {

bool g_initialized = false;
bool g_enabled = false;
std::string g_dir;
std::string g_last_content;
long g_counter = 0;

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

	state["id"] = ++g_counter;
	std::string line = state.dump();

	{
		std::ofstream log(g_dir + "/states.jsonl", std::ios::app);
		log << line << '\n';
	}
	const std::string tmp_path = g_dir + "/state.json.tmp";
	{
		std::ofstream out(tmp_path, std::ios::trunc);
		out << line << '\n';
	}
	std::error_code ec;
	std::filesystem::rename(tmp_path, g_dir + "/state.json", ec);
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

} // namespace AiState
