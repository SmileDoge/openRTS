#pragma once

// The legacy DLL host: mirror objects in the ts.h layout for every car of a
// consist, the DLL modules, and the call sequence of rts1.exe
// (docs/reverse/train-dll.md). One Host per consist; everything runs on the
// thread that calls it.

#include <windows.h>

#include <array>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <openrts/dllhost/protocol.hpp>

#include "ts_abi.hpp"

namespace openrts::dllhost {

struct Module {
    std::wstring path;
    HMODULE handle = nullptr;
    bool failed = false;          // refused or crashed: never called again
    std::wstring error;
    std::uint32_t version[22]{};  // DLLVersion(1..21)
    bool rts_library = false;     // InitRTSLibrary returned true
    // Adapters for DLLs built against older layouts (RTS 0x5364D0 / 0x537DC8):
    // the variant of each adapted object, 0 = the object is passed as it is.
    std::uint32_t loco_variant = 0;    // 1: no AuxLibs (+476) and no WagonObjs (+384); 2: no WagonObjs
    std::uint32_t cab_variant = 0;     // 1: SignalsInfo without its RTSSignalData pointer (+136)
    std::uint32_t signals_variant = 0; // 1: 28-byte SignalsInfo
    // Main DLL (Locomotive / Engine) and aux (AuxLibrary first) entry points
    // share the names; which signature applies depends on how it is used.
    void* init = nullptr;
    void* run = nullptr;
    void* change_loco = nullptr;
    void* lost_master = nullptr;
    void* can_work_with = nullptr;
    void* can_switch = nullptr;
    void* switched = nullptr;
    void* alsn = nullptr;
    void* speed_limit = nullptr;
    std::uint32_t refs = 0;
};

// Loads `path`, reads DLLVersion and resolves the entry points; does not
// call InitRTSLibrary. Returns false with module.error set on failure.
bool load_module(Module& module, const std::wstring& path, std::wstring* exports = nullptr);
// Compares the DLLVersion answers with the host's layouts; "" when they match.
std::wstring layout_mismatch(const Module& module);
// Decides how RTS would run the module (0x5364D0): directly, through the
// adapters for TS_VERSION 101..103 (sets the variants), or not at all
// (returns the reason).
std::wstring choose_adapters(Module& module);

struct AuxMirror {
    Module* module = nullptr;
    std::vector<AuxCallSpec> calls;
    std::vector<float> var;
    std::vector<abi::EngineParam> params;
    abi::SMSObject sound{};
    bool has_sound = false;
};

struct CarMirror {
    // ElectricLocomotive / DieselLocomotive / Wagon, as the kind says.
    alignas(4) std::array<std::uint8_t, sizeof(abi::ElectricLocomotive)> object{};
    alignas(4) std::array<std::uint8_t, sizeof(abi::DieselEngine)> engine{};
    CarSpec spec;
    std::uint32_t index{};
    bool has_engine = false;      // a DLL module is bound
    bool initialised = false;     // Init returned true
    Module* module = nullptr;

    abi::Cabin cabin{};
    std::vector<abi::ActSwitch> switches;
    // A switch the player drags (or left where a drag put it): its frame is
    // theirs until its SetState changes otherwise (RTS: the node driven by
    // hand, 0x437E64).
    std::vector<std::uint8_t> held_switches;
    std::vector<abi::ActDisplay> displays;
    std::vector<float> door_clock; // s an exterior door has been moving to its SetState
    // SCREEN displays: the ActScreen RTS hangs on the display (its +68) and
    // the sub-objects; LABELs write into `text`.
    struct Screen {
        std::uint32_t display{};
        abi::ActScreen screen{};
        std::vector<abi::CloseUpSubObject> objects;
        std::vector<std::vector<wchar_t>> text;
    };
    std::vector<Screen> screens;
    std::vector<abi::InternalLight> internal_lights;
    std::vector<abi::TexChange> tex_changes;
    std::vector<abi::ActDoor> cab_doors;
    std::vector<abi::FreeAnimation> cab_anims;

    std::vector<abi::LightObj> lights;
    std::vector<abi::ActDoor> doors;
    std::vector<abi::FreeAnimation> anims;
    std::vector<abi::ExhaustContr> exhaust;
    abi::SMSObject sound_ext{}, sound_cab{};

    std::vector<float> var;
    std::vector<abi::EngineParam> params;
    std::vector<float> engine_voltage, engine_current, engine_force;
    std::vector<abi::Locomotive*> slaves;
    std::vector<abi::AuxLibrary> aux;
    std::vector<AuxMirror> aux_info;
    abi::AuxLibStack aux_stack{};

    // Shadow copies in an older layout for adapted DLLs, by loco variant (1, 2).
    struct Shadow {
        alignas(4) std::array<std::uint8_t, sizeof(abi::ElectricLocomotive)> loco{};
        alignas(4) std::array<std::uint8_t, sizeof(abi::Cabin)> cab{};
        bool loco_live = false; // handed out and not yet copied back
        bool cab_live = false;
    };
    Shadow shadows[3];

    abi::Object shape_object{};
    alignas(4) std::array<std::uint8_t, 1024> wag_type{};

    abi::Wagon* wagon() { return reinterpret_cast<abi::Wagon*>(object.data()); }
    abi::Locomotive* loco() { return reinterpret_cast<abi::Locomotive*>(object.data()); }
    abi::ElectricLocomotive* electric() { return reinterpret_cast<abi::ElectricLocomotive*>(object.data()); }
    abi::DieselLocomotive* diesel() { return reinterpret_cast<abi::DieselLocomotive*>(object.data()); }
    abi::Engine* eng() { return reinterpret_cast<abi::Engine*>(engine.data()); }
    abi::ElectricEngine* eng_e() { return reinterpret_cast<abi::ElectricEngine*>(engine.data()); }
    abi::DieselEngine* eng_d() { return reinterpret_cast<abi::DieselEngine*>(engine.data()); }
    [[nodiscard]] bool is_loco() const { return spec.kind == car_electric || spec.kind == car_diesel; }
};

class Host {
public:
    Host();
    ~Host();
    Host(const Host&) = delete;
    Host& operator=(const Host&) = delete;

    // Replaces the consist. Modules stay loaded across setups.
    bool setup(const SetupRequest& request, std::vector<ModuleReport>& modules);
    // The player takes locomotive `car`: binds and initialises it and its
    // group (0x468C74). Fills `reply` with the state after Init.
    bool enter(const EnterRequest& request, const std::vector<CarInput>& inputs, StepReply& reply);
    bool step(const StepRequest& request, StepReply& reply);
    bool can_switch(const CanSwitchRequest& request, bool& allowed);
    bool drag_switch(const DragSwitchRequest& request, bool& moved);
    // The state of switch `id` of car `car` (as a step's CarOutput has it).
    [[nodiscard]] SwitchState switch_state(std::uint32_t car, std::uint32_t id) const;

    // Log lines (warnings, DLL failures) since the last take.
    std::wstring take_log();
    void log(const std::wstring& line);

    // Host function support (the exported _RTS* functions).
    static Host* current();
    CarMirror* car_of(const void* object);      // a Wagon / Locomotive mirror
    CarMirror* car_of_cabin(const void* cabin);
    abi::AuxLibrary* aux_of(const void* object);
    CarMirror* car_of_aux(const abi::AuxLibrary* aux);
    int get_integer(const abi::Locomotive* loco, std::uint32_t code, int index);
    int set_integer(const abi::Locomotive* loco, std::uint32_t code, int value);
    void* loco_object(const Module* caller, const abi::Locomotive* loco, int code, int index, int sub);
    void* loco_object_by_name(const Module* caller, const abi::Locomotive* loco, int code, const wchar_t* name, int sub);
    void* cab_object(const Module* caller, const abi::Cabin* cab, int code, int index, int sub);
    void* cab_object_by_name(const Module* caller, const abi::Cabin* cab, int code, const wchar_t* name, int sub);
    Module* module_of(const void* dll);
    void* aux_object(const abi::AuxLibrary* aux, int code, int index, int sub);
    void* aux_object_by_name(const abi::AuxLibrary* aux, int code, const wchar_t* name, int sub);
    int track_items(const abi::Locomotive* loco, std::uint32_t code, float distance, abi::TrackItemsItem*& items,
                    std::uint32_t& count);

private:
    Module* module_for(const std::wstring& path);
    bool init_rts_library(Module& module);
    void build_car(CarMirror& car);
    void link_cars();
    bool bind(CarMirror& car);
    bool init_car(CarMirror& car, std::uint32_t state);
    std::uint32_t init_state(const CarMirror& car, bool player) const;
    std::uint32_t run_state(const CarMirror& car) const;
    void group(CarMirror& player);
    void write_inputs(const std::vector<CarInput>& inputs);
    void write_cabs(const StepRequest& request);
    void run_car(CarMirror& car, float dt, float air_temperature);
    void signals(const StepRequest& request);
    void collect(StepReply& reply);
    void collect_car(CarMirror& car, CarOutput& out);
    void build_screens(CarMirror& car, const CabSpec& cab);
    void switched(CarMirror& car, std::uint32_t id, std::uint32_t previous);
    // The DLL's (and aux libraries') CanSwitch for moving switch `id` to `state`.
    bool allows_switch(CarMirror& car, std::uint32_t id, std::uint32_t state);
    // aux_* event dispatch, before (`after` false) or after the main call.
    bool aux_event(CarMirror& car, std::uint16_t event, bool after, std::uint32_t a, std::uint32_t b);
    void aux_change_loco(CarMirror& car, bool lost, const abi::Locomotive* other, std::uint32_t state);
    void fail(Module& module, const wchar_t* call, std::uint32_t code);
    // The object as `module` expects it: the mirror, or a refreshed shadow
    // in the module's older layout. sync() copies the shadows handed out
    // since the last sync back into the mirrors (after every DLL call).
public:
    abi::Locomotive* view(CarMirror& car, const Module* module);
    abi::Cabin* cab_view(CarMirror& car, const Module* module);
    void sync();
private:
    std::vector<std::pair<CarMirror*, std::uint32_t>> live_shadows_;
    void prepare_fpu();

    std::vector<std::unique_ptr<Module>> modules_;
    std::vector<std::unique_ptr<CarMirror>> cars_;
    std::vector<abi::Wagon*> train_cars_; // the train's car list (FirstInGroup / LastInGroup point into it)
    std::unordered_map<const void*, CarMirror*> by_object_;
    std::unordered_map<const void*, CarMirror*> by_cabin_;
    std::unordered_map<const void*, CarMirror*> by_aux_;
    std::deque<std::wstring> strings_; // names and texts the mirrors point to
    alignas(4) std::array<std::uint8_t, 1024> train_object_{};
    std::wstring rts_root_;
    std::wstring log_;
    std::uint8_t train_type_ = 1;
    double day_time_{};
    bool night_ = false;
    bool train_reversed_ = false;
    bool start_running_ = true;
    std::int32_t start_state_ = -1; // an activity's TrainState, or -1
    float air_temperature_ = 20.0f;
    int set_direction_ = 0;
    std::uint32_t player_ = UINT32_MAX;
    std::vector<abi::TrackItemsItem> track_items_;
};

const wchar_t* keep(std::deque<std::wstring>& strings, std::u16string_view text);

} // namespace openrts::dllhost
