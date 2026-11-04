// No bridge commands: compare native shop UI outcomes against a base build.
#include "scene_shop.h"
#include "tests/test_mock_actor.h"
#include "drawable_mgr.h"
#include "bitmap.h"
#include "input.h"
#include <nlohmann/json.hpp>
#include <iostream>
#include <filesystem>

int main() {
    char directory[] = "/tmp/shop-compat-XXXXXX";
    if (!mkdtemp(directory)) return 1;
    setenv("EASYRPG_AI_STATE_DIR", directory, 1);
    Output::SetLogLevel(LogLevel::Error);
    Bitmap::SetFormat(format_R8G8B8A8_a().format());
    MockActor db(Player::EngineRpg2k);
    DrawableList drawables; DrawableMgr::SetLocalList(&drawables);
    auto& party = *Main_Data::game_party;
    party.Clear();
    lcf::Data::items[0].name = "Potion"; lcf::Data::items[0].price = 31;
    party.AddItem(1,3); party.GainGold(1000);
    bool transacted = false;
    auto scene = std::make_shared<Scene_Shop>(std::vector<int>{1}, 0, true, true,
        [&](bool value) { transacted = value; });
    Scene::Push(scene); scene->Start();
    nlohmann::json trace = nlohmann::json::array();
    auto step = [&](int button = -1, int repeat = -1) {
        Input::triggered.reset(); Input::repeated.reset();
        if (button >= 0) Input::triggered[button] = true;
        if (repeat >= 0) Input::repeated[repeat] = true;
        scene->vUpdate();
        trace.push_back({{"gold",party.GetGold()},{"count",party.GetItemCount(1)}});
    };
    step(); step(Input::DECISION); step(Input::DECISION);
    step(-1,Input::RIGHT); step(Input::DECISION); // buy two
    for (int i=0; i<DEFAULT_FPS; ++i) step();
    step(Input::CANCEL); step(Input::DECISION,Input::DOWN); // sell
    step(Input::DECISION); step(Input::DECISION); // sell one
    for (int i=0; i<DEFAULT_FPS; ++i) step();
    step(Input::CANCEL); step(Input::CANCEL);
    scene->Suspend(Scene::Null);
    if (party.GetGold() != 953 || party.GetItemCount(1) != 4 || !transacted) return 2;
    std::cout << nlohmann::json{{"trace",trace},{"transacted",transacted}} << '\n';
    scene.reset(); Scene::instance.reset(); Scene::old_instances.clear();
    std::filesystem::remove_all(directory);
}
