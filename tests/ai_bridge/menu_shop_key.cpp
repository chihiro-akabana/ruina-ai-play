// Native scene/bridge harness. Test data is a small database fixture, not Ruina.
#include BRIDGE_SOURCE
#include "tests/test_mock_actor.h"
#include "drawable_mgr.h"
#include "filefinder.h"
#include "filesystem.h"
#include <lcf/ldb/reader.h>
#include <lcf/lmu/reader.h>
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

static void Shop(const std::string& game) {
    const int start_checks = checks;
    MockActor db(Player::EngineRpg2k);
    std::vector<int> goods{1,2,3};
    int shop_type = 0;
    if (game.empty()) {
        lcf::Data::items[0].name = "Potion"; lcf::Data::items[0].price = 31;
        lcf::Data::items[1].name = "Unsellable"; lcf::Data::items[1].price = 0;
        lcf::Data::items[2].name = "Expensive"; lcf::Data::items[2].price = 999999;
    } else {
        FileFinder::SetGameFilesystem(FileFinder::Root().Create(game));
        auto data = lcf::LDB_Reader::Load(game + "/RPG_RT.ldb", "windows-1252");
        Check(data != nullptr, "real Ruina database loads");
        lcf::Data::data = std::move(*data);
        goods.clear();
        std::vector<std::filesystem::path> maps;
        for (const auto& path : std::filesystem::directory_iterator(game))
            if (path.path().extension() == ".lmu") maps.push_back(path.path());
        std::sort(maps.begin(), maps.end());
        for (const auto& path : maps) {
            auto map = lcf::LMU_Reader::Load(path.string(), "windows-1252");
            if (!map) continue;
            for (const auto& event : map->events) for (const auto& page : event.pages) for (const auto& command : page.event_commands) {
                if (command.code != 10720 || command.parameters.size() <= 4 || command.parameters[0] != 0) continue;
                bool useful = false;
                for (size_t i = 4; i < command.parameters.size(); ++i) {
                    auto* item = lcf::ReaderUtil::GetElement(lcf::Data::items, command.parameters[i]);
                    if (item && item->price > 0 && item->price < 100000) useful = true;
                }
                if (!useful) continue;
                goods.assign(command.parameters.begin() + 4, command.parameters.end());
                shop_type = command.parameters[1];
                std::cout << "Real shop: " << path.filename().string() << ", event " << event.ID << ", goods " << goods.size() << '\n';
                goto found;
            }
        }
        found:
        Check(!goods.empty(), "real buy/sell shop definition found");
    }
    auto& party = *Main_Data::game_party;
    party.Clear();
    party.GainGold(900000);
    auto scene = std::make_shared<Scene_Shop>(goods, shop_type, true, true, nullptr);
    Scene::Push(scene); scene->Start(); UpdateShop(scene.get());
    Check(Latest()["phase"] == "menu" && Latest()["gold"] == party.GetGold(), "shop menu and gold emitted");
    Send("confirm"); UpdateShop(scene.get());
    Check(Ack()["status"] == "rejected", "confirm at menu cannot transact");
    Send("choose", 0); UpdateShop(scene.get());
    auto buy = Latest();
    Check(buy["phase"] == "buy" && buy["items"].size() == goods.size(), "native buy goods list emitted");
    int selected = -1;
    for (size_t i = 0; i < goods.size(); ++i) {
        auto* item = lcf::ReaderUtil::GetElement(lcf::Data::items, goods[i]);
        Check(buy["items"][i]["name"] == std::string(item->name) && buy["items"][i]["price"] == item->price
            && buy["items"][i]["count"] == party.GetItemCount(goods[i]) && buy["items"][i]["equipped"] == party.GetEquippedItemCount(goods[i]),
            "buy name/price/owned/equipped match native database/window values");
        if (selected < 0 && item->price > 0 && item->price < 100000 && party.GetMaxItemCount(goods[i]) >= 2) selected = static_cast<int>(i);
    }
    Check(selected >= 0, "test shop has purchasable item");
    const int id = goods[selected];
    const int price = lcf::ReaderUtil::GetElement(lcf::Data::items, id)->price;
    const int gold = party.GetGold(), owned = party.GetItemCount(id);
    Send("choose", selected); UpdateShop(scene.get());
    Check(Latest()["phase"] == "quantity" && party.GetGold() == gold && party.GetItemCount(id) == owned, "item selection stages quantity without money movement");
    Send("confirm"); UpdateShop(scene.get());
    Check(Ack()["status"] == "rejected" && party.GetGold() == gold, "confirm cannot skip quantity and summary");
    Send("choose", 0); UpdateShop(scene.get());
    Check(Ack()["reason"] == "quantity out of range", "quantity zero rejected");
    Input::triggered[Input::DECISION] = true; scene->UpdateNumberInput(); Input::triggered.reset();
    Send("confirm"); UpdateShop(scene.get());
    Check(Ack()["reason"] == "confirm summary has not been emitted" && party.GetGold() == gold,
        "native decision staging cannot confirm before summary emission");
    Send("back"); UpdateShop(scene.get());
    Send("choose", selected); UpdateShop(scene.get());
    Send("choose", 2); UpdateShop(scene.get());
    auto summary = Latest()["transaction"];
    Check(Latest()["phase"] == "confirm" && summary["count"] == 2 && summary["unit_price"] == price
        && summary["total"] == 2*price && summary["gold_after"] == gold - 2*price, "purchase confirm summary matches exact pending trade");
    Check(party.GetGold() == gold && party.GetItemCount(id) == owned, "confirm summary has no inventory or gold effects");
    Send("choose", 1); UpdateShop(scene.get());
    Check(Ack()["reason"] == "confirm or back required" && party.GetGold() == gold, "choose at confirm cannot buy");
    // Native decision also cannot bypass an explicitly staged summary.
    Input::triggered[Input::DECISION] = true; scene->vUpdate(); Input::triggered.reset();
    Check(party.GetGold() == gold && party.GetItemCount(id) == owned, "native decision cannot execute bridge-staged trade");
    Send("confirm", -1, g_last_id + 1); UpdateShop(scene.get());
    Check(Ack()["reason"] == "stale state_id" && party.GetGold() == gold, "stale shop confirmation rejected");
    Send("confirm"); UpdateShop(scene.get());
    Check(Ack()["status"] == "applied" && party.GetGold() == summary["gold_after"] && party.GetItemCount(id) == owned+2
        && Latest()["phase"] == "bought", "purchase executes exactly the displayed trade through native routine");
    auto log = Read("commands.jsonl"); UpdateShop(scene.get());
    Check(log == Read("commands.jsonl") && party.GetItemCount(id) == owned+2, "duplicate confirm cannot transact twice");
    for (int i=0; i<DEFAULT_FPS; ++i) scene->vUpdate();
    UpdateShop(scene.get()); Check(Latest()["phase"] == "buy", "native bought acknowledgement timer returns to buy");
    Send("choose", selected); UpdateShop(scene.get()); Send("choose", 1); UpdateShop(scene.get());
    int after_purchase = party.GetGold();
    Send("back"); UpdateShop(scene.get());
    Check(Latest()["phase"] == "buy" && party.GetGold() == after_purchase && party.GetItemCount(id) == owned+2, "back from purchase confirmation cancels without effects");
    Send("back"); UpdateShop(scene.get()); Send("choose", 1); UpdateShop(scene.get());
    Check(Latest()["phase"] == "sell", "menu choose sell opens inventory");
    int sell_index = -1;
    auto sell = Latest();
    for (const auto& entry : sell["items"]) if (entry["name"] == std::string(lcf::ReaderUtil::GetElement(lcf::Data::items,id)->name)) sell_index = entry["index"];
    Check(sell_index >= 0 && sell["items"][sell_index]["price"] == price/2 && sell["items"][sell_index]["count"] == owned+2, "sell inventory displays native count and halved sell price");
    Send("choose", sell_index); UpdateShop(scene.get()); Send("choose", 1); UpdateShop(scene.get());
    summary = Latest()["transaction"];
    Check(Latest()["phase"] == "confirm" && summary["total"] == price/2 && summary["gold_after"] == after_purchase + price/2
        && party.GetGold() == after_purchase && party.GetItemCount(id) == owned+2, "sale summary precedes all effects");
    Send("back"); UpdateShop(scene.get());
    Check(party.GetGold() == after_purchase && party.GetItemCount(id) == owned+2, "back cancels sale without effects");
    Send("choose", sell_index); UpdateShop(scene.get()); Send("choose", 1); UpdateShop(scene.get()); Send("confirm"); UpdateShop(scene.get());
    Check(party.GetGold() == after_purchase + price/2 && party.GetItemCount(id) == owned+1 && Latest()["phase"] == "sold", "sale uses native transaction and changes gold/inventory by summary");
    for (int i=0; i<DEFAULT_FPS; ++i) scene->vUpdate();
    UpdateShop(scene.get());
    Send("advance"); UpdateShop(scene.get()); Check(Ack()["status"] == "rejected", "misplaced shop verb rejected");
    Send("future_verb"); UpdateShop(scene.get()); Check(Ack()["reason"] == "unknown command type", "unknown shop verb rejected");
    Send("choose", 999); UpdateShop(scene.get()); Check(Ack()["status"] == "rejected", "invalid inventory index rejected");
    Send("back"); UpdateShop(scene.get()); Send("choose", 0); UpdateShop(scene.get());
    party.LoseGold(party.GetGold());
    Send("choose", selected); UpdateShop(scene.get()); Check(Ack()["reason"] == "insufficient gold", "native insufficient-gold refusal surfaced");
    party.GainGold(900000); party.AddItem(id, party.GetMaxItemCount(id));
    Send("choose", selected); UpdateShop(scene.get()); Check(Ack()["reason"] == "inventory full", "native full-inventory refusal surfaced");
    Scene::Pop(); scene.reset(); Scene::old_instances.clear();
    std::cout << "PASS " << (game.empty() ? "shop fixture" : "Ruina shop") << ": " << checks - start_checks << " checks\n";
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
    Shop(argc > 1 ? argv[1] : "");
}
