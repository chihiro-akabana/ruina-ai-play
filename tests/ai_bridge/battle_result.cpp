#include BRIDGE_SOURCE
#include "tests/test_mock_actor.h"
#include "game_enemy.h"
#include "drawable_mgr.h"
#include <iostream>

using nlohmann::json;
using namespace AiState;

static int checks = 0;
static void Check(bool condition, const char* description) {
	++checks;
	if (!condition) {
		std::cerr << "FAIL: " << description << '\n';
		std::exit(1);
	}
}

class TestBattle : public Scene_Battle_Rpg2k {
public:
	TestBattle() : Scene_Battle_Rpg2k(BattleArgs{}) {}
	void Select(State s, int sub) {
		SetState(s);
		SetSceneActionSubState(sub);
	}
	void Resolve(BattleResult result) { EndBattle(result); }
	void ResultUi() { CreateUi(); }
	void ResultStep() { ProcessSceneAction(); }
};

static json Latest() {
	std::ifstream input(g_dir + "/state.json");
	return json::parse(input);
}

static std::shared_ptr<TestBattle> NewBattle() {
	auto battle = std::make_shared<TestBattle>();
	Scene::Push(battle);
	UpdateBattle(battle.get());
	return battle;
}

static void Explore() {
	g_last_content.clear();
	Check(Emit(json{{"type", "explore"}}), "explore is emitted");
}

// Exercise the actual result handlers, message assembly, and synchronous
// bridge hook. No synthetic substate-1 delivery is used here.
static void NativeResults(const PendingMessage& ordinary) {
    Game_Battle::battle_running = true;
    // Normally set by Player startup after selecting the text encoding.
    Player::escape_symbol = "\\";
    Player::escape_char = '\\';
    Main_Data::game_party->Clear(); // reward fixture; multi-page level-ups are checked above
    Bitmap::SetFormat(format_R8G8B8A8_a().format());
    lcf::Data::terms.victory = "戦いに勝った！";
    lcf::Data::terms.defeat = "全滅した……";
    lcf::Data::terms.exp_received = "の経験値を獲得！";
    lcf::Data::terms.gold = "G";
    lcf::Data::terms.gold_recieved_b = "を手に入れた！";
    lcf::Data::terms.item_recieved = "を手に入れた！";
    lcf::Data::items[0].name = "薬草";
    auto& enemy = lcf::Data::enemies[0];
    enemy.exp = 8; enemy.gold = 8; enemy.drop_id = 1; enemy.drop_prob = 100;
    auto& troop = lcf::Data::troops[0];
    troop.members.resize(1); troop.members[0].enemy_id = 1;
    Main_Data::game_enemyparty->ResetBattle(1);
    Main_Data::game_enemyparty->GetEnemy(0)->SetHp(0);

    auto victory = NewBattle();
    victory->ResultUi();
    OnMessageStart(ordinary, "pre-battle event\n");
    victory->Select(Scene_Battle::State_Battle, 2);
    OnMessageStart(ordinary, "in-battle narration\n");
    victory->Select(Scene_Battle::State_Victory, 0);
    const auto previous = g_counter;
    victory->ResultStep();
    Check(victory->AiBattleSubState() == 1 && g_counter == previous+1,
        "actual victory handler synchronously emits its message before returning at substate 1");
    auto message = Latest();
    Check(message.at("type") == "message" && !message.contains("last_battle") && !g_battle_pending,
        "actual victory message is a normal state before result observation");
    json lines = json::array();
    for(const auto& page : message.at("pages")) for(const auto& line : page) lines.push_back(line);
    Check(lines == json::array({"戦いに勝った！", "8の経験値を獲得！", " 8Gを手に入れた！", "薬草を手に入れた！"}),
        "real reward assembly supplies displayed victory, EXP, gold and item text");
    UpdateBattle(victory.get());
    OnMessageStart(ordinary, "post-result narration\n");
    auto bundle = Latest().at("last_battle");
    Check(bundle.at("result") == "victory" && bundle.at("summary_lines") == lines,
        "real victory summary exactly equals displayed pages, excluding other narration");
    victory->ResultStep(); // eEnd: actual EndBattle hook
    OnMessageStart(ordinary, "post-battle event\n");
    Check(Latest().at("last_battle") == bundle, "actual victory resolution retains summary");
    Explore();
    Check(Latest().at("last_battle") == bundle && !g_battle_pending, "real victory retained through explore");
    Explore();

    auto defeat = NewBattle();
    defeat->ResultUi();
    OnMessageStart(ordinary, "another pre-battle message\n");
    defeat->Select(Scene_Battle::State_Defeat, 0);
    const auto defeat_previous = g_counter;
    defeat->ResultStep();
    Check(defeat->AiBattleSubState() == 1 && g_counter == defeat_previous+1,
        "actual defeat handler synchronously emits its message before returning at substate 1");
    message = Latest();
    Check(message.at("type") == "message" && message.at("pages") == json::array({json::array({"全滅した……"})})
        && !message.contains("last_battle") && !g_battle_pending,
        "actual defeat message precedes bundle, with no victory text carried from previous battle");
    UpdateBattle(defeat.get());
    OnMessageStart(ordinary, "later defeat narration\n");
    bundle = Latest().at("last_battle");
    Check(bundle.at("result") == "defeat" && bundle.at("summary_lines") == message.at("pages").at(0),
        "actual defeat summary contains exactly its displayed result line");
    defeat->ResultStep();
    OnMessageStart(ordinary, "defeat event dialogue\n");
    Check(Latest().at("last_battle") == bundle, "actual defeat resolution retains summary");
    Explore();
    Check(Latest().at("last_battle") == bundle && !g_battle_pending, "real defeat retained through explore");
    Explore();

    // The poll can also observe the result state before message assembly
    // (for example, debug victory). Enrich its existing provisional bundle.
    auto observed = NewBattle();
    observed->Select(Scene_Battle::State_Victory, 0);
    UpdateBattle(observed.get());
    Check(g_battle_pending && g_battle_pending_summary_lines.empty(), "poll-first victory starts with no fabricated text");
    OnMessageStart(ordinary, "observed result\n");
    Check(Latest().at("last_battle").at("summary_lines") == json::array({"observed result"}),
        "poll-first bundle enriched before result message emission");
    observed->Select(Scene_Battle::State_Victory, 1);
    OnMessageStart(ordinary, "unrelated wait-phase message\n");
    Check(Latest().at("last_battle").at("summary_lines") == json::array({"observed result"}),
        "wait-phase messages cannot overwrite summary");
    observed->Resolve(BattleResult::Escape);
    Check(g_battle_pending_result == "escape" && g_battle_pending_summary_lines.empty(),
        "escape override cannot inherit staged victory text");
    Explore(); Explore();

    auto interrupted = NewBattle();
    interrupted->Select(Scene_Battle::State_Victory, 0);
    OnMessageStart(ordinary, "interrupted result\n");
    Scene::Pop();
    auto no_message = NewBattle();
    no_message->Resolve(BattleResult::Defeat);
    Check(g_battle_pending_summary_lines.empty(), "scene replacement discards staged text even before a bundle existed");
    Explore(); Explore();
    auto retained = NewBattle();
    retained->Select(Scene_Battle::State_Victory, 0);
    OnMessageStart(ordinary, "retained victory\n");
    retained->Select(Scene_Battle::State_Victory, 1);
    retained->Resolve(BattleResult::Victory); // resolution can also create the bundle directly
    OnMessageStart(ordinary, "retained battle event\n");
    const auto retained_bundle = Latest().at("last_battle");
    Check(retained_bundle.at("summary_lines") == json::array({"retained victory"}),
        "EndBattle preserves staged summary without an intervening poll");
    auto following = NewBattle();
    OnMessageStart(ordinary, "following pre-battle event\n");
    Check(Latest().at("last_battle") == retained_bundle, "new battle preserves previous nonempty pending summary");
    following->Select(Scene_Battle::State_Victory, 0);
    OnMessageStart(ordinary, "following victory\n");
    Check(Latest().at("last_battle") == retained_bundle, "new result message cannot relabel or overwrite previous pending bundle");
    following->Select(Scene_Battle::State_Victory, 1);
    following->Resolve(BattleResult::Victory);
    OnMessageStart(ordinary, "following event\n");
    Check(Latest().at("last_battle").at("summary_lines") == json::array({"following victory"})
        && Latest().at("last_battle").at("battle_seq") != retained_bundle.at("battle_seq"),
        "following resolution uses only its own staged summary and sequence");
    Explore(); Explore();
    Game_Battle::battle_running = false;
}

int main() {
	char directory[] = "/tmp/battle-result-regression-XXXXXX";
	Check(mkdtemp(directory) != nullptr, "temporary output directory");
	setenv("EASYRPG_AI_STATE_DIR", directory, 1);
	Output::SetLogLevel(LogLevel::Error);
	MockActor fixture(Player::EngineRpg2k);
	// Normally installed by Graphics::UpdateSceneCallback before UI creation.
	DrawableList drawables;
	DrawableMgr::SetLocalList(&drawables);
	PendingMessage pm(PendingMessage::DefaultCommandInserter);
	pm.PushLine("displayed text");

	auto victory = NewBattle();
	Check(g_battle_seq == 1, "first battle sequence");
	g_battle_log = {"final hit"};
	victory->Select(Scene_Battle::State_Victory, 0);
	// In vUpdate, the outcome and its eBegin message occur after the bridge
	// poll. SetPendingMessage synchronously calls OnMessageStart at substate 0.
	OnMessageStart(pm, "勝利！\\|\n1の経験値を獲得！\\.\n1Gを手に入れた！\n薬草を手に入れた！\n\fフランはレベル 3 になった！\\!\n技を覚えた！\n\f");
	Check(Latest().at("type") == "message" && !Latest().contains("last_battle") && !g_battle_pending,
		"real victory ordering: ordinary result message precedes bundle");
	victory->Select(Scene_Battle::State_Victory, 1);
	UpdateBattle(victory.get());
	OnMessageStart(pm, "later battle narration\n");
	const json summary = {"勝利！", "1の経験値を獲得！", "1Gを手に入れた！", "薬草を手に入れた！", "フランはレベル 3 になった！", "技を覚えた！"};
	auto bundle = Latest().at("last_battle");
	Check(bundle.at("result") == "victory", "victory result compatibility");
	Check(bundle.at("battle_seq") == 1, "victory sequence compatibility");
	Check(bundle.at("log") == json::array({"final hit"}), "narration log compatibility");
	Check(bundle.at("summary_lines") == summary, "all displayed reward/level/skill pages are cleaned and captured");
	Check(g_battle_pending, "message does not consume result");
	const auto message_counter = g_counter;
	Check(!Emit(json{{"scene", Latest().at("scene")}, {"location", Latest().at("location")}, {"gold", Latest().at("gold")}, {"party", Latest().at("party")}, {"type", "message"}, {"text", Latest().at("text")}, {"pages", Latest().at("pages")}}), "identical message suppressed");
	Check(g_counter == message_counter && g_battle_pending, "suppression preserves result");
	victory->Resolve(BattleResult::Victory);
	OnMessageStart(pm, "post-battle dialogue\n");
	Check(Latest().at("last_battle") == bundle, "post-battle dialogue keeps complete result");
	Check(Emit(json{{"type", "menu"}}), "intervening menu emitted");
	Check(Latest().at("last_battle") == bundle && g_battle_pending, "menu keeps result");
	Check(Emit(json{{"type", "explore"}, {"last_battle", json{{"result", "external"}}}}), "state with its own bundle is emitted");
	Check(g_battle_pending, "another bundle does not consume the pending result");

	// A suppressed explore attempt must not consume the pending bundle.
	g_last_content = json{{"type", "explore"}, {"last_battle", bundle}}.dump();
	Check(!Emit(json{{"type", "explore"}}) && g_battle_pending, "suppressed explore retains bundle");
	g_last_content.clear();
	Explore();
	Check(Latest().at("last_battle") == bundle && !g_battle_pending, "first emitted explore delivers and clears result");
	Explore();
	Check(!Latest().contains("last_battle"), "following explore has no result");

	auto failed = NewBattle();
	Check(g_battle_seq == 2 && !g_battle_result_sent, "second battle resets sequence/result bookkeeping");
	failed->Select(Scene_Battle::State_Escape, 2);
	UpdateBattle(failed.get());
	Check(!g_battle_pending, "failed escape has no result");
	failed->Resolve(BattleResult::Abort);
	Explore();
	Check(!Latest().contains("last_battle"), "explicit abort has no result");

	auto escape = NewBattle();
	escape->Select(Scene_Battle::State_Escape, 1);
	UpdateBattle(escape.get());
	Check(!g_battle_pending, "success substate alone does not imply resolved escape");
	g_battle_log = {"escape succeeded"};
	escape->Resolve(BattleResult::Escape);
	Check(g_battle_pending && g_battle_pending_result == "escape", "actual EndBattle Escape creates result");
	OnMessageStart(pm, "escaped map dialogue\n");
	bundle = Latest().at("last_battle");
	Check(bundle.at("result") == "escape" && bundle.at("battle_seq") == 3, "escape result and sequence");
	Check(bundle.at("log") == json::array({"escape succeeded"}) && bundle.at("summary_lines").empty(), "escape log retained without invented rewards");

	// Starting another battle before exploration must not relabel the old result.
	auto interrupted = NewBattle();
	Check(g_battle_seq == 4, "new battle starts while old result is pending");
	OnMessageStart(pm, "new battle event\n");
	Check(Latest().at("last_battle") == bundle, "pending battle sequence and summary stay stable");
	Scene::Pop(); // Equivalent disappearance route used by load/title; no EndBattle result.
	Explore();
	Check(Latest().at("last_battle") == bundle, "disappearance does not replace previous result with escape");
	Explore();
	Check(!Latest().contains("last_battle"), "previous result consumed");
	auto title_abort = NewBattle();
	title_abort->Select(Scene_Battle::State_Escape, 1);
	UpdateBattle(title_abort.get());
	Scene::Pop();
	Explore();
	Check(!Latest().contains("last_battle"), "load/title disappearance while escaping produces no bundle");

	auto abort_provisional = NewBattle();
	abort_provisional->Select(Scene_Battle::State_Defeat, 0);
	UpdateBattle(abort_provisional.get());
	Check(g_battle_pending, "provisional defeat exists");
	abort_provisional->Resolve(BattleResult::Abort);
	Check(!g_battle_pending, "abort clears this battle's provisional result");
	auto changed_result = NewBattle();
	changed_result->Select(Scene_Battle::State_Victory, 0);
	UpdateBattle(changed_result.get());
	changed_result->Resolve(BattleResult::Escape);
	Check(g_battle_pending_result == "escape", "explicit resolution overrides a provisional result");
	Explore();
	Explore();

	auto defeat = NewBattle();
	defeat->Select(Scene_Battle::State_Defeat, 0);
	// Defeat uses the same synchronous eBegin delivery as victory.
	OnMessageStart(pm, "全滅した……\n");
	defeat->Select(Scene_Battle::State_Defeat, 1);
	UpdateBattle(defeat.get());
	OnMessageStart(pm, "later defeat narration\n");
	Check(Latest().at("last_battle").at("result") == "defeat", "message-first defeat retained for later bundle");
	Check(Latest().at("last_battle").at("summary_lines") == json::array({"全滅した……"}), "defeat summary has no fabricated reward values");
	defeat->Resolve(BattleResult::Defeat);
	Explore();
	Check(Latest().at("last_battle").at("result") == "defeat", "defeat reaches exploration");
	Explore();
	NativeResults(pm);
	g_enabled = false;
	const auto last_counter = g_counter;
	OnBattleEnd(defeat.get(), BattleResult::Escape);
	Check(!Emit(json{{"type", "explore"}}) && !g_battle_pending && g_counter == last_counter, "disabled bridge has no output or result changes");

	Scene::old_instances.clear();
	std::cout << "PASS: " << checks << " checks; output " << directory << '\n';
}
