#include BRIDGE_SOURCE
// Old bridge sources do not define hooks added after their revision. These
// adapters only satisfy linkage; legacy command traces never enter a shop.
#ifdef BASELINE_NO_SHOP
namespace AiState { void UpdateShop(Scene_Shop*) {} }
#endif
#ifdef BASELINE_NO_KEY
namespace AiState { int PollKeyCommand() { return -1; } }
#endif
#include "baseui.h"
#include "bitmap.h"
#include "tests/test_mock_actor.h"
#include <iostream>

using namespace AiState;
using nlohmann::json;
namespace fs = std::filesystem;
static int checks = 0;
static void Check(bool ok, const char* name) {
    ++checks;
    if (!ok) { std::cerr << "FAIL: " << name << '\n'; std::exit(1); }
}
static std::string Read(const std::string& name) {
    std::ifstream in(g_dir + "/" + name, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
}
static json Ack() { return json::parse(Read("ack.json")); }
static void Send(long seq, const std::string& type, long id = -500) {
    std::ofstream out(g_dir + "/command.json");
    out << json{{"seq",seq},{"state_id",id},{"type",type},{"index",1},{"name","test"},{"dir","left"},{"steps",2},{"point",0}};
}
static json Protocol() {
    json out;
    for (auto name : {"state.json", "states.jsonl", "ack.json", "commands.jsonl", "command.json"}) out[name] = Read(name);
    out["last_seq"] = g_last_seq;
    out["last_id"] = g_last_id;
    out["last_type"] = g_last_type;
    out["last_content"] = g_last_content;
    out["pending"] = g_has_pending;
    out["move_pending"] = g_has_move_cmd;
    for (int d=0; d<5; ++d) out["seen"].push_back(g_cmd_raw_seen[d]);
    return out;
}
static bool Owns(int d, const std::string& t) {
    if (d==0) return t=="advance" || t=="choose";
    if (d==1) return t=="set_name" || t=="confirm_name";
    if (d==2) return t=="move" || t=="confirm" || t=="goto" || t=="menu";
    return t=="choose" || t=="back";
}
static void Compat() {
    MockActor actor(Player::EngineRpg2k);
    PendingMessage pm(PendingMessage::DefaultCommandInserter);
    pm.PushLine("displayed text");
    OnMessageStart(pm, "visible text\n");
    json trace = json::array();
    long seq=0;
    for (int first=0; first<5; ++first) {
        for (auto type : {"advance", "choose", "set_name", "confirm_name", "move", "confirm", "goto", "menu", "back"}) {
            for (bool stale : {false,true}) {
                Send(++seq, type, stale ? g_last_id-1 : g_last_id);
                auto before = Protocol();
                PollCaptureCommand();
                PollKeyCommand();
                Check(Protocol()==before, "capture poll cannot mutate legacy protocol");
                for (int n=0;n<5;++n) {
                    int d=(first+n)%5;
                    Command cmd;
                    bool read=ReadFreshCommand(cmd,d);
                    trace.push_back({d,read});
                    if (read && (cmd.state_id != g_last_id || Owns(d,cmd.type))) {
                        Consume(cmd, cmd.state_id != g_last_id ? "rejected" : "applied", cmd.state_id != g_last_id ? "stale state_id" : "");
                    }
                }
                // A duplicate payload must stay suppressed for each legacy domain.
                for(int d=0;d<5;++d) { Command cmd; trace.push_back(ReadFreshCommand(cmd,d)); }
                Emit(json{{"type",first==2?"explore":"menu"},{"visible",seq%3}});
                trace.push_back(Protocol());
            }
        }
    }
    for(int first=0;first<5;++first) {
        for(bool stale : {false,true}) {
            Send(++seq,"capture",stale ? g_last_id-1 : g_last_id);
            auto before=Protocol();
            for(int n=0;n<5;++n) { Command cmd; trace.push_back(ReadFreshCommand(cmd,(first+n)%5)); }
            Check(before["states.jsonl"]==Read("states.jsonl") && before["ack.json"]==Read("ack.json") && before["last_seq"]==g_last_seq,"scene polls cannot consume capture");
            PollCaptureCommand();
            PollCaptureCommand();
            trace.push_back(Protocol());
        }
    }
    std::cout << json{{"trace",trace},{"checks",checks}}.dump() << '\n';
}
#ifndef BASELINE
static void Unknown() {
    EnsureInit();
    Emit(json{{"type","message"},{"text","visible fixture"}});
    g_has_pending=true; g_pending.seq=900; g_pending.type="advance";
    g_has_move_cmd=true; g_move_cmd.seq=901; g_move_queue={2,4};
    long seq=0;
    for(int first=0;first<5;++first) {
        for(auto type : {"select","unknown","Choose","future_verb"}) {
            for(bool stale : {false,true}) {
                Send(++seq,type,stale ? g_last_id-1 : g_last_id);
                auto before=Protocol();
                PollCaptureCommand();
                Check(before==Protocol(),"capture domain cannot consume unknown type");
                const auto log=Read("commands.jsonl");
                Command cmd;
                Check(!ReadFreshCommand(cmd,first),"unknown command never reaches scene dispatch");
                json expected={{"seq",seq},{"state_id",stale?g_last_id-1:g_last_id},{"type",type},{"status","rejected"},{"reason","unknown command type"}};
                Check(Ack()==expected && Read("commands.jsonl")==log+expected.dump()+"\n","exactly one loud unknown rejection");
                const auto rejected=Read("commands.jsonl");
                for(int round=0;round<2;++round) {
                    // Changed raw content with the same seq must also stay suppressed.
                    if(round==1) { std::ofstream out(g_dir+"/command.json",std::ios::app); out << " "; }
                    for(int n=0;n<5;++n) Check(!ReadFreshCommand(cmd,(first+n)%5),"later domains suppress already consumed seq");
                    PollCaptureCommand();
                }
                Check(Read("commands.jsonl")==rejected && Ack()==expected && g_last_seq==seq,"no repeated ack across polls or rewrites");
                auto after=Protocol();
                for(auto key : {"state.json","states.jsonl","last_id","last_type","last_content","pending","move_pending"})
                    Check(before[key]==after[key],"unknown rejection preserves state and pending bookkeeping");
                Check(g_pending.seq==900 && g_move_cmd.seq==901 && g_move_queue==std::deque<int>({2,4}),"unknown rejection preserves pending inputs");
            }
        }
    }
    std::cout << "PASS unknown: " << checks << " checks; 40 scenarios\n";
}
class TestUi : public BaseUi {
public:
    TestUi() : BaseUi(Game_Config{}) { main_surface=Bitmap::Create(320,240,Color(12,34,56,255)); }
    bool ProcessEvents() override { return true; }
    void UpdateDisplay() override {}
    void vGetConfig(Game_ConfigVideo&) const override {}
};
static void NoTemps() {
    Check(!fs::exists(g_dir+"/capture.png.tmp") && !fs::exists(g_dir+"/capture.json.tmp"),"temporary capture files cleaned");
}
static void ValidPng() {
    auto bytes=Read("capture.png");
    auto bitmap=Bitmap::Create(reinterpret_cast<const uint8_t*>(bytes.data()),bytes.size(),false);
    Check(bitmap && bitmap->GetWidth()==320 && bitmap->GetHeight()==240,"complete native PNG decodes at 320x240");
}
static void Gate(bool expected) {
    EnsureInit();
    Check(g_capture_enabled==expected,"gate exact initialization value");
    setenv("EASYRPG_AI_CAPTURE",expected?"0":"1",1);
    Send(1,"capture");
    PollCaptureCommand();
    Check(Ack()["status"]=="rejected","gate/no-surface rejection");
    Check(Ack()["detail"]==(expected?"no display surface":"capture disabled"),"gate is read only once and has exact detail");
    Check(Ack()["state_id"]==0,"ack before state emission has id zero");
    Check(!fs::exists(g_dir+"/capture.png") && !fs::exists(g_dir+"/state.json"),"refusal creates no capture or state");
    std::cout << "PASS gate: " << checks << " checks\n";
}
static void Capture() {
    EnsureInit();
    Check(g_capture_enabled,"capture gate enabled");
    Send(1,"capture");
    for(int d=0;d<5;++d) { Command cmd; Check(!ReadFreshCommand(cmd,d),"legacy domain ignores stale capture"); }
    Check(g_last_seq==0 && !fs::exists(g_dir+"/ack.json"),"legacy reads cannot acknowledge capture");
    PollCaptureCommand();
    Check(Ack()["status"]=="rejected" && Ack()["detail"]=="no display surface","no UI rejects without crash");
    Bitmap::SetFormat(format_R8G8B8A8_a().format());
    DisplayUi=std::make_shared<TestUi>();
    auto surface=DisplayUi->GetDisplaySurface();
    DisplayUi->GetDisplaySurface().reset();
    Send(2,"capture"); PollCaptureCommand();
    Check(Ack()["detail"]=="no display surface","UI without surface rejects without crash");
    DisplayUi->GetDisplaySurface()=surface;
    Player::player_config.screenshot_scale.Set(4);
    Scene::instance=std::make_shared<Scene>(); Scene::instance->type=Scene::Title;
    Send(3,"capture"); PollCaptureCommand();
    Check(Ack()["status"]=="applied" && Ack()["state_id"]==0,"title capture applies before state emission despite stale requested id");
    Check(json::parse(Read("capture.json"))==json{{"seq",3},{"state_id",0},{"scene","title"},{"width",320},{"height",240}},"title metadata exact values");
    ValidPng(); NoTemps();
    Check(!fs::exists(g_dir+"/state.json") && !fs::exists(g_dir+"/states.jsonl"),"capture emits no text state");
    auto log=Read("commands.jsonl"); PollCaptureCommand();
    Check(log==Read("commands.jsonl"),"duplicate capture is not processed twice");
    Emit(json{{"type","explore"},{"visible","fixture"}});
    g_has_pending=true; g_pending.seq=800; g_pending.type="advance";
    g_has_move_cmd=true; g_move_cmd.seq=801; g_move_queue={2,4};
    const auto states=Read("states.jsonl"); const auto state=Read("state.json");
    const auto last_content=g_last_content; const auto last_type=g_last_type;
    const auto id=g_last_id;
    long seq=3;
    for (auto scene : {Scene::Map,Scene::Battle,Scene::Menu,Scene::Title}) {
        Scene::instance->type=scene;
        Send(++seq,"capture",id-100); PollCaptureCommand();
        Check(Ack()["status"]=="applied" && Ack()["state_id"]==id,"stale capture reports current state id");
        auto meta=json::parse(Read("capture.json"));
        Check(meta["scene"]==Utils::LowerCase(Scene::scene_names[scene]) && meta["seq"]==seq && meta["state_id"]==id,"metadata scene is current rather than stale state type");
        ValidPng(); NoTemps();
    }
    Check(Read("states.jsonl")==states && Read("state.json")==state && g_last_id==id && g_last_content==last_content && g_last_type==last_type,"capture preserves state files and dedupe bookkeeping");
    Check(g_has_pending && g_pending.seq==800 && g_has_move_cmd && g_move_cmd.seq==801 && g_move_queue==std::deque<int>({2,4}),"capture preserves pending message and movement commands");
    // Verify each legacy domain can still read its command after the capture poll,
    // including legacy stale foreign-command rejection eligibility.
    for(int d=0;d<5;++d) {
        for(auto type : {"advance","set_name","move","choose","back","unknown"}) {
            Send(++seq,type,id-1);
            auto before=Protocol(); PollCaptureCommand();
            Check(before==Protocol(),"capture poll leaves every non-capture command untouched");
            Command cmd;
            if (std::string(type)=="unknown") {
                Check(!ReadFreshCommand(cmd,d) && Ack()["seq"]==seq && Ack()["reason"]=="unknown command type", "unknown type is rejected before stale scene dispatch");
            } else {
                Check(ReadFreshCommand(cmd,d) && cmd.seq==seq && cmd.state_id==id-1 && cmd.type==type,"legacy domain retains original stale command eligibility");
                Consume(cmd,"rejected","stale state_id");
            }
            Check(!Ack().contains("detail"),"legacy ack schema unchanged");
        }
        Send(++seq,"capture",-99);
        Command cmd;
        for(int other=0;other<5;++other) Check(!ReadFreshCommand(cmd,other),"interleaved capture cannot be consumed by any legacy domain");
        PollCaptureCommand();
        Check(Ack()["seq"]==seq && Ack()["status"]=="applied","interleaved capture reaches its own poll");
    }
    // Inject deterministic open, write, and rename failures. Existing complete
    // output must survive all failures before publication.
    const auto old_png=Read("capture.png"); const auto old_json=Read("capture.json");
    auto fail = [&](const char* detail, bool old_unchanged) {
        Send(++seq,"capture"); PollCaptureCommand();
        Check(Ack()["status"]=="rejected" && Ack()["detail"]==detail,"file failure acknowledged with detail");
        NoTemps();
        if(old_unchanged) Check(Read("capture.png")==old_png && Read("capture.json")==old_json,"failed staging preserves previous complete outputs");
    };
    fs::create_directory(g_dir+"/capture.png.tmp"); fail("cannot open capture PNG",true);
    fs::create_symlink("/dev/full",g_dir+"/capture.png.tmp"); fail("cannot write capture PNG",true);
    fs::create_symlink("/dev/full",g_dir+"/capture.json.tmp"); fail("cannot write capture metadata",true);
    fs::remove(g_dir+"/capture.png"); fs::create_directory(g_dir+"/capture.png");
    fail("cannot publish capture PNG",false);
    fs::remove(g_dir+"/capture.png");
    fs::remove(g_dir+"/capture.json"); fs::create_directory(g_dir+"/capture.json");
    fail("cannot publish capture metadata",false); ValidPng();
    fs::remove(g_dir+"/capture.json");
    Send(++seq,"capture"); PollCaptureCommand();
    Check(Ack()["status"]=="applied","capture recovers after filesystem errors"); ValidPng(); NoTemps();
    fs::remove(g_dir+"/capture.png"); fs::remove(g_dir+"/capture.json");
    g_capture_enabled=false;
    Send(++seq,"capture");
    for(int d=0;d<5;++d) { Command cmd; Check(!ReadFreshCommand(cmd,d),"gate-off capture bypasses legacy stale rejection"); }
    PollCaptureCommand();
    Check(Ack()["status"]=="rejected" && Ack()["detail"]=="capture disabled" && Ack()["state_id"]==id,"gate-off capture consumed and loudly refused with current id");
    Check(!fs::exists(g_dir+"/capture.png") && !fs::exists(g_dir+"/capture.json") && Read("states.jsonl")==states,"gate-off capture does not create images or emit states");
    log=Read("commands.jsonl"); PollCaptureCommand();
    Check(log==Read("commands.jsonl"),"disabled capture consumed exactly once");
    DisplayUi.reset(); Scene::instance.reset();
    std::cout << "PASS capture: " << checks << " checks; output " << g_dir << '\n';
}
#endif
int main(int argc,char** argv) {
    char directory[]="/tmp/capture-regression-XXXXXX";
    Check(mkdtemp(directory)!=nullptr,"temporary output directory");
    setenv("EASYRPG_AI_STATE_DIR",directory,1);
    Output::SetLogLevel(LogLevel::Error);
    const std::string mode=argc>1?argv[1]:"compat";
    if(mode=="compat") { EnsureInit(); Compat(); }
#ifndef BASELINE
    else if(mode=="capture") Capture();
    else if(mode=="unknown") Unknown();
    else if(mode=="gate") Gate(argc>2 && std::string(argv[2])=="1");
    else if(mode=="disabled") {
        unsetenv("EASYRPG_AI_STATE_DIR");
        PollCaptureCommand();
        Check(!g_enabled && g_counter==0 && g_last_seq==0 && fs::is_empty(directory),"bridge disabled remains inert");
        std::cout << "PASS disabled: " << checks << " checks\n";
    }
#endif
}
