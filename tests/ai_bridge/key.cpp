#include BRIDGE_SOURCE
#include "tests/test_mock_actor.h"
#include "input_source.h"
#include "drawable_mgr.h"
#include <iostream>

// Use the existing input source variable with a deterministic neutral provider;
// no production-only test API or OS key events are involved.
namespace Input { extern std::unique_ptr<Source> source; }
using namespace AiState;
using nlohmann::json;
static int checks;
static void Check(bool ok, const char* why) {
    ++checks;
    if (!ok) { std::cerr << "FAIL: " << why << '\n'; std::exit(1); }
}
class NeutralSource : public Input::Source {
public:
    NeutralSource() : Input::Source(Game_ConfigInput{}, {}) {}
    void Update() override {}
    void UpdateSystem() override {}
    void Hold(Input::InputButton button, bool on) { pressed_buttons[button] = on; }
};
class WaitScene : public Scene {
public:
    WaitScene() { type = Scene::Title; }
    int decisions = 0, cancels = 0;
    void vUpdate() override {
        if (Input::IsTriggered(Input::DECISION)) ++decisions;
        if (Input::IsTriggered(Input::CANCEL)) ++cancels;
    }
};
static std::string Read(const char* name) {
    std::ifstream in(g_dir + "/" + name);
    return std::string(std::istreambuf_iterator<char>(in), {});
}
static json Ack() { return json::parse(Read("ack.json")); }
static json Latest() { return json::parse(Read("state.json")); }
static void Write(json command) {
    std::ofstream out(g_dir + "/command.json"); out << command;
}
static void Key(long seq, json button, json id = "deliberately ignored") {
    Write({{"seq",seq},{"type","key"},{"button",button},{"state_id",id}});
}
static void Gate(bool expected) {
    Check(g_key_enabled == expected, "key gate uses exact initialization value");
    setenv("EASYRPG_AI_KEY", expected ? "0" : "1", 1);
    Key(1, "decision"); Input::Update();
    Check(Ack()["status"] == (expected ? "applied" : "rejected") && Ack()["state_id"] == 0,
        "gate sampled once and key ack uses current zero id");
    if (!expected) Check(Ack()["reason"] == "key disabled", "gate-off reason exactly key disabled");
    Check(Input::IsTriggered(Input::DECISION) == expected, "gate controls logical trigger");
    const auto log = Read("commands.jsonl");
    for (int i=0; i<60; ++i) Input::Update();
    Check(Read("commands.jsonl") == log && !Input::IsPressed(Input::DECISION), "one gate disposition and no lasting input");
    Check(Read("states.jsonl").empty(), "key never manufactures states");
}
static void Pulse() {
    Check(g_key_enabled, "pulse gate enabled");
    auto scene = std::make_shared<WaitScene>(); Scene::Push(scene);
    Key(1, "decision");
    Command command;
    for (int d=0; d<6; ++d) Check(!ReadFreshCommand(command,d), "every other domain leaves key for input poll");
    Check(g_last_seq == 0 && Read("ack.json").empty(), "scene polls cannot consume key");
    Input::UpdateSystem();
    Check(g_last_seq == 0, "system-only update cannot consume a logical pulse");
    Input::Update(); scene->vUpdate();
    Check(scene->decisions == 1 && Input::IsTriggered(Input::DECISION) && Input::IsRepeated(Input::DECISION)
        && Input::IsPressed(Input::DECISION), "decision advances an unhooked title-equivalent wait through native input queries");
    Check(Ack() == json({{"seq",1},{"state_id",0},{"type","key"},{"status","applied"}}), "key ignores nonnumeric state_id and reports current id");
    Check(Input::GetAllRawPressed().none() && Input::GetAllRawTriggered().none(), "pulse never synthesizes raw/OS keys");
    const auto log = Read("commands.jsonl");
    for (int i=0; i<60; ++i) { Input::Update(); scene->vUpdate(); }
    Check(scene->decisions == 1 && !Input::IsTriggered(Input::DECISION) && !Input::IsRepeated(Input::DECISION)
        && !Input::IsPressed(Input::DECISION) && log == Read("commands.jsonl"), "one pulse lasts one input frame without hold/repeat or duplicate ack");
    Key(10, "cancel"); Input::Update(); scene->vUpdate();
    Check(scene->cancels == 1 && Input::IsTriggered(Input::CANCEL), "cancel closes a cancel-only headless wait");
    Input::Update(); scene->vUpdate();
    Check(scene->cancels == 1 && Input::IsReleased(Input::CANCEL) && !Input::IsPressed(Input::CANCEL), "cancel releases next logical frame");
    Key(20,"decision"); Input::Update(); scene->vUpdate();
    Key(21,"decision"); Input::Update(); scene->vUpdate();
    Check(scene->decisions == 3, "consecutive commands each trigger exactly once without requiring a gap frame");
    auto* source = static_cast<NeutralSource*>(Input::GetInputSource());
    source->Hold(Input::DECISION, true);
    Input::Update(); const int physical_time = Input::press_time[Input::DECISION];
    Key(22,"decision"); Input::Update();
    Check(Input::IsTriggered(Input::DECISION) && Input::press_time[Input::DECISION] == physical_time + 1,
        "pulse retriggers even when physical key held without resetting physical hold duration");
    source->Hold(Input::DECISION,false); Input::Update();
    const auto current_id = g_last_id;
    long seq = 30;
    for (auto button : {json("DECISION"),json("left"),json(""),json(1),json(nullptr)}) {
        Key(++seq,button); Input::Update();
        Check(Ack()["reason"] == "invalid key button" && Ack()["state_id"] == current_id
            && !Input::IsTriggered(Input::DECISION) && !Input::IsTriggered(Input::CANCEL), "invalid button consumed once without effect");
        auto rejected = Read("commands.jsonl"); Input::Update();
        Check(rejected == Read("commands.jsonl"), "invalid key gets exactly one rejected ack");
    }
    Write({{"seq",++seq},{"type","key"}}); Input::Update();
    Check(Ack()["reason"] == "invalid key button", "missing key button rejected");
    Write({{"seq",++seq},{"type","key"},{"button","decision"},{"hold",999}});
    Input::Update(); scene->vUpdate(); Input::Update(); scene->vUpdate();
    Check(scene->decisions == 4 && !Input::IsPressed(Input::DECISION), "extra hold data cannot prolong a pulse");
    // Own poll is isolated exactly like capture.
    for (auto type : {"capture","choose","future_verb"}) {
        Write({{"seq",++seq},{"type",type},{"state_id",g_last_id}});
        auto before = Read("commands.jsonl"); Input::Update();
        Check(before == Read("commands.jsonl"), "key poll never consumes another command type");
    }
    Scene::Pop(); scene.reset(); Scene::old_instances.clear();
}
static void NativeCancel() {
    MockActor db;
    DrawableList drawables; DrawableMgr::SetLocalList(&drawables);
    Bitmap::SetFormat(format_R8G8B8A8_a().format());
    Main_Data::game_party->Clear(); Main_Data::game_party->AddActor(1);
    auto* actor = Main_Data::game_actors->GetActor(1);
    auto scene = std::make_shared<Scene_Equip>(std::vector<Game_Actor*>{actor}, 0);
    Scene::Push(scene); scene->Start();
    Check(scene->AiDecide(0) == nullptr && scene->AiIsItemLevel(), "native equipment item screen opened");
    UpdateEquip(scene.get());
    Key(60,"cancel"); Input::Update(); scene->vUpdate(); UpdateEquip(scene.get());
    Check(!scene->AiIsItemLevel() && Latest()["level"] == "slot", "key cancel returns actual native equipment item screen to slots");
    Input::Update(); scene->vUpdate();
    Check(Scene::instance.get() == scene.get(), "one cancel pulse cannot also leave slot screen next frame");
    Key(61,"cancel"); Input::Update(); scene->vUpdate();
    Check(Scene::instance.get() != scene.get(), "second cancel command closes actual native equipment screen");
    scene.reset(); Scene::old_instances.clear();
    DrawableMgr::SetLocalList(nullptr);
}
static void Heartbeat() {
    MockActor db;
    // Common Emit also covers producers that do not call BuildContext.
    for (const char* type : {"message","choice","number_input","name_input","explore",
            "battle_option","battle_command","battle_skill","battle_item","battle_target","battle_target_ally",
            "menu","menu_actor","menu_item","menu_skill","menu_target","menu_save","menu_load","menu_end","menu_equip","menu_status","shop"}) {
        Check(Emit({{"type",type},{"last_seq",-900}}), "state type emitted");
        Check(Latest()["last_seq"] == g_last_seq, "every state type gains uniform current heartbeat in common emitter");
    }
    long seq = 100;
    for (int d=0; d<5; ++d) {
        Write({{"seq",++seq},{"type","choose"},{"state_id",g_last_id}});
        Command cmd;
        Check(ReadFreshCommand(cmd,d), "fresh command reaches each ordinary domain");
        Consume(cmd,d%2 ? "rejected" : "applied",d%2 ? "fixture refusal" : "");
        OnNameInputStart(std::to_string(seq));
        Check(Latest()["last_seq"] == seq && BuildContext()["last_seq"] == seq, "applied/rejected ordinary consumption appears in next actual name state and context");
    }
    Write({{"seq",200},{"type","capture"},{"state_id",-999}}); PollCaptureCommand();
    Check(g_last_seq == 200 && Ack()["type"] == "capture", "capture participates in highest consumed seq");
    Key(300,"cancel",json::object()); Input::Update();
    Check(g_last_seq == 300 && Ack()["state_id"] == g_last_id, "key participates and reports latest emitted id");
    Write({{"seq",400},{"type","unknown"}}); Command cmd; ReadFreshCommand(cmd,0);
    OnNameInputStart("after unknown");
    Check(Latest()["last_seq"] == 400 && Ack()["reason"] == "unknown command type", "unknown rejection participates in heartbeat");
    auto log = Read("commands.jsonl"); Key(399,"decision"); Input::Update();
    Check(Read("commands.jsonl") == log && g_last_seq == 400, "older seq cannot lower heartbeat or pulse");
    auto state_log = Read("states.jsonl");
    Key(500,"cancel"); Input::Update(); OnNameInputStart("after unknown");
    Check(g_last_seq == 500 && Read("states.jsonl") == state_log, "heartbeat alone preserves existing duplicate-state suppression");
    OnNameInputStart("changed visible name"); Check(Latest()["last_seq"] == 500, "next changed state carries latest consumption");
}
int main(int argc,char** argv) {
    char directory[]="/tmp/key-heartbeat-XXXXXX";
    Check(mkdtemp(directory) != nullptr,"temporary output");
    setenv("EASYRPG_AI_STATE_DIR",directory,1);
    const std::string mode = argc > 1 ? argv[1] : "pulse";
    if (mode == "disabled") unsetenv("EASYRPG_AI_STATE_DIR");
    Output::SetLogLevel(LogLevel::Error);
    Input::source = std::make_unique<NeutralSource>(); Input::ResetKeys();
    EnsureInit();
    if (mode == "gate") Gate(argc > 2 && std::string(argv[2]) == "1");
    else if (mode == "disabled") {
        // g_dir is empty, so write only to the fixture's known output directory.
        std::ofstream out(std::string(directory)+"/command.json"); out << "{\"seq\":1,\"type\":\"key\",\"button\":\"decision\"}"; out.close();
        Input::Update(); Check(!g_enabled && g_last_seq == 0 && !Input::IsTriggered(Input::DECISION),"disabled bridge never consumes or injects key");
    } else { Pulse(); NativeCancel(); Heartbeat(); }
    std::cout << "PASS key " << mode << ": " << checks << " checks\n";
    Input::source.reset(); std::filesystem::remove_all(directory);
}
