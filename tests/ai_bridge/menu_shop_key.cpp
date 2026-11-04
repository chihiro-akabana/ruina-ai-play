// Native scene/bridge harness. Test data is a small database fixture, not Ruina.
#include BRIDGE_SOURCE
#include "tests/test_mock_actor.h"
#include "drawable_mgr.h"
#include <iostream>

using namespace AiState;
using nlohmann::json;
static int checks;
static long seq;
static void Check(bool ok, const char* why) {
    ++checks;
    if (!ok) { std::cerr << "FAIL: " << why << '\n'; std::exit(1); }
}
static std::string Read(const char* file) {
    std::ifstream in(g_dir + "/" + file);
    return std::string(std::istreambuf_iterator<char>(in), {});
}
static json Latest() { return json::parse(Read("state.json")); }
static json Ack() { return json::parse(Read("ack.json")); }
static void Send(const char* type, int index = -1, long id = -1) {
    std::ofstream out(g_dir + "/command.json");
    out << json{{"seq", ++seq}, {"state_id", id == -1 ? g_last_id : id}, {"type", type}, {"index", index}};
}
static void Equip() {
    MockActor db(Player::EngineRpg2k3);
    lcf::Data::system.equipment_setting = lcf::rpg::System::EquipmentSetting_class;
    MakeDBActor(1, 1, 50, 100, 30, 20, 21, 22, 23);
    auto* sword = MakeDBEquip(1, lcf::rpg::Item::Type_weapon, 5, 1, 2, 3);
    sword->name = "Sword";
    auto* great = MakeDBEquip(2, lcf::rpg::Item::Type_weapon, 12, 2, 3, 4);
    great->name = "Greatsword"; great->two_handed = true;
    auto* forbidden = MakeDBEquip(3, lcf::rpg::Item::Type_weapon, 99, 99, 99, 99);
    forbidden->name = "Wrong class";
    forbidden->class_set = lcf::DBBitArray(20, false);
    auto* shield = MakeDBEquip(4, lcf::rpg::Item::Type_shield, 0, 8, 0, 0);
    shield->name = "Shield";
    Main_Data::game_party->AddActor(1);
    auto* actor = Main_Data::game_actors->GetActor(1);
    for (int i=1; i<=4; ++i) Main_Data::game_party->AddItem(i, 2);
    actor->ChangeEquipment(1, 1); actor->ChangeEquipment(2, 4);
    auto scene = std::make_shared<Scene_Equip>(std::vector<Game_Actor*>{actor}, 0);
    Scene::Push(scene); scene->Start();
    UpdateEquip(scene.get());
    auto state = Latest();
    Check(state["actor"]["atk"] == 25 && state["actor"]["def"] == 30
        && state["actor"]["spi"] == 24 && state["actor"]["agi"] == 26, "displayed parameters match fixture stats");
    Check(state["equipment"][0]["item"] == "Sword" && state["equipment"][1]["item"] == "Shield"
        && state["equipment"][4]["item"].is_null(), "equipment slots match one-based actor API");
    Send("choose", 0); UpdateEquip(scene.get()); state = Latest();
    Check(Ack()["status"] == "applied" && state["level"] == "item", "choose slot opens native candidate list");
    Check(state["candidates"].size() == 3 && state["candidates"][0]["name"] == "Sword"
        && state["candidates"][1]["name"] == "Greatsword" && state["candidates"][2]["remove"] == true,
        "native candidates omit wrong class and include removal");
    Check(state["preview"] == json({{"atk",25},{"def",30},{"spi",24},{"agi",26}}), "highlighted preview matches screen totals");
    Send("choose", 3); UpdateEquip(scene.get());
    Check(Ack()["status"] == "rejected" && actor->GetEquipment(1) == sword, "out-of-list wrong-class attempt is rejected without equipment change");
    auto log = Read("commands.jsonl"); UpdateEquip(scene.get());
    Check(Read("commands.jsonl") == log, "invalid candidate gets exactly one ack");
    forbidden->class_set[0] = true;
    scene->AiItemWindow()->Refresh();
    forbidden->class_set[0] = false;
    Send("choose", 2); UpdateEquip(scene.get());
    Check(Ack()["reason"] == "candidate is not equippable" && actor->GetEquipment(1) == sword,
        "wrong-class candidate revalidated before native change even if list became stale");
    scene->AiItemWindow()->Refresh();
    // Preview for a highlighted two-handed candidate: screen removes the shield.
    scene->AiItemWindow()->SetIndex(1); UpdateEquip(scene.get());
    Check(Latest()["preview"] == json({{"atk",32},{"def",23},{"spi",25},{"agi",27}}), "two-handed preview uses native other-hand conflict calculation");
    Send("choose", 1); UpdateEquip(scene.get()); state = Latest();
    Check(Ack()["status"] == "applied" && state["level"] == "slot" && state["equipment"][0]["item"] == "Greatsword"
        && state["equipment"][1]["item"].is_null(), "native equipment change and refreshed state resolve two-handed conflict");
    Check(actor->GetAtk() == 32 && Main_Data::game_party->GetItemCount(2) == 1
        && Main_Data::game_party->GetItemCount(4) == 2, "native equipment path updates parameters and inventory");
    Send("choose", 0); UpdateEquip(scene.get());
    Send("back"); UpdateEquip(scene.get());
    Check(Latest()["level"] == "slot", "back from item returns to slot");
    Send("choose", 0); UpdateEquip(scene.get());
    int remove = static_cast<int>(Latest()["candidates"].size()) - 1;
    Send("choose", remove); UpdateEquip(scene.get());
    Check(Latest()["equipment"][0]["item"].is_null() && actor->GetEquipment(1) == nullptr, "remove candidate uses native change path");
    Send("choose", 9); UpdateEquip(scene.get());
    Check(Ack()["status"] == "rejected", "invalid slot rejected");
    Send("choose", 0, g_last_id + 50); UpdateEquip(scene.get());
    Check(Ack()["reason"] == "stale state_id" && !scene->AiIsItemLevel(), "stale equip command rejected");
    Send("confirm"); UpdateEquip(scene.get());
    Check(Ack()["status"] == "rejected", "misplaced known verb rejected in equip");
    Send("future_verb"); UpdateEquip(scene.get());
    Check(Ack()["reason"] == "unknown command type", "unknown equip verb rejected");
    actor->ChangeEquipment(1, 1); sword->cursed = true;
    Send("choose", 0); UpdateEquip(scene.get());
    Check(Ack()["status"] == "rejected" && !scene->AiIsItemLevel(), "native cursed-slot refusal surfaced");
    Scene::Pop(); scene.reset(); Scene::old_instances.clear();
    std::cout << "PASS equip: " << checks << " checks\n";
}
int main(int argc, char** argv) {
    char directory[] = "/tmp/menu-shop-key-XXXXXX";
    Check(mkdtemp(directory) != nullptr, "temporary output");
    setenv("EASYRPG_AI_STATE_DIR", directory, 1);
    Output::SetLogLevel(LogLevel::Error);
    Bitmap::SetFormat(format_R8G8B8A8_a().format());
    EnsureInit();
    DrawableList drawables;
    DrawableMgr::SetLocalList(&drawables);
    Equip();
}
