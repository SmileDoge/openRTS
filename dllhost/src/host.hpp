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
    std::array<std::uint32_t, 8> pairs{}; // its SMSObject::PairedFlags once a pair code came
};

struct CarMirror {
    // ElectricLocomotive / DieselLocomotive / Wagon, as the kind says.
    alignas(4) std::array<std::uint8_t, sizeof(abi::ElectricLocomotive)> object{};
    alignas(4) std::array<std::uint8_t, sizeof(abi::DieselEngine)> engine{};
    CarSpec spec;
    std::uint32_t index{};
    std::uint32_t train{};        // which of the host's trains (0: the player's)
    bool has_engine = false;      // a DLL module is bound
    bool initialised = false;     // Init returned true
    Module* module = nullptr;

    abi::Cabin cabin{};
    std::vector<abi::ActSwitch> switches;
    // A switch the player drags (or left where a drag put it): its frame is
    // theirs until its SetState changes otherwise (RTS: the node driven by
    // hand, 0x437E64).
    std::vector<std::uint8_t> held_switches;
    // Per switch id: its node's Shortcut sets, in positions (SwitchSpec).
    std::vector<std::vector<std::vector<float>>> switch_shortcuts;
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
    std::array<std::uint32_t, 8> pairs_ext{}, pairs_cab{}; // their PairedFlags (RTS: 4 words on, 4 known)

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
    // Cars coupled or cut off (ReconsistRequest): the mirrors move with
    // their cars, then the groups are made again. Fills `reply` like a step's.
    bool reconsist(const ReconsistRequest& request, StepReply& reply);
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
    // The by-name lookups' cache (above): answers given from it and found
    // by a scan; how often it was emptied; with OPENRTS_DLLHOST_NAME_CHECK,
    // the answers scanned again and how many of them differed.
    struct NameCacheStats {
        std::uint64_t hits = 0;
        std::uint64_t misses = 0;
        std::uint64_t cleared = 0;
        std::uint64_t checked = 0;
        std::uint64_t differed = 0;
    };
    [[nodiscard]] const NameCacheStats& name_cache_stats() const noexcept { return name_stats_; }
    int track_items(const abi::Locomotive* loco, std::uint32_t code, float distance, abi::TrackItemsItem*& items,
                    std::uint32_t& count);
    // RTSGetNamedInteger(sms, RTS_TRIGGERID, name) and RTSIsSMSTriggerOn
    // (RTS 0x4394F4, 0x439548): `table` is an SMSObject::sms.
    int named_integer(const void* table, const wchar_t* name) const;
    int trigger_on(const abi::SMSObject* sound, std::uint16_t code) const;

private:
    Module* module_for(const std::wstring& path);
    bool init_rts_library(Module& module);
    void build_car(CarMirror& car);
    void link_cars();
    bool bind(CarMirror& car);
    bool init_car(CarMirror& car, std::uint32_t state);
    std::uint32_t init_state(const CarMirror& car, bool player) const;
    std::uint32_t run_state(const CarMirror& car) const;
    // The group around `master` (cars joined by control cables, within its
    // train); `player`: the player's group (its slaves are in his join).
    void group(CarMirror& master, bool player = true);
    // RTS 0x4F995C after a cable changed or the trains changed: for every
    // master of every train the groups again; a slave left out of its
    // master's group gets LostMaster.
    void regroup();
    void regroup(CarMirror& master, bool player);
    void write_inputs(const std::vector<CarInput>& inputs);
    void write_cabs(const StepRequest& request);
    void run_car(CarMirror& car, float dt, float air_temperature);
    void signals(const StepRequest& request);
    void collect(StepReply& reply);
    // Every car's air as the Wagons hold it (enter: what Init wrote).
    void collect_air(StepReply& reply);
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
    // The trains whose DLLs run: [0] the player's, then the parts cut off
    // from it. Their cars are consecutive in cars_ (first, count). The
    // objects stay where they are (a DLL may keep Wagon::TrainIn).
    struct TrainMirror {
        alignas(4) std::array<std::uint8_t, 1024> object{};
        std::vector<abi::Wagon*> wagons; // FirstInGroup / LastInGroup point into it
        std::size_t first = 0, count = 0;
        std::uint8_t type = 1;
    };
    std::vector<std::unique_ptr<TrainMirror>> trains_;
    std::size_t train_count_ = 0;
    TrainMirror& train_of(const CarMirror& car) const { return *trains_[car.train]; }
    // Cars cut off from the consist, by key, with their DLLs' state, until
    // they are coupled again (RTS keeps the Engine with the car).
    std::unordered_map<std::uint32_t, std::unique_ptr<CarMirror>> parked_;
    void add_sound_tables(const std::vector<SoundTableSpec>& tables);
    std::unordered_map<const void*, CarMirror*> by_object_;
    std::unordered_map<const void*, CarMirror*> by_cabin_;
    std::unordered_map<const void*, CarMirror*> by_aux_;
    std::deque<std::wstring> strings_; // names and texts the mirrors point to
    std::wstring rts_root_;
    std::wstring log_;
    double day_time_{};
    bool night_ = false;
    bool train_reversed_ = false;
    bool start_running_ = true;
    std::int32_t start_state_ = -1; // an activity's TrainState, or -1
    float air_temperature_ = 20.0f;
    int set_direction_ = 0;
    // The tape (RTSGetInteger / RTSSetIntegerG 7): the car it rolls on, -1
    // none; what the DLLs changed this step (a start always opens a new file).
    std::int32_t tape_car_ = -1;
    bool tape_set_ = false;
    bool tape_started_ = false;
    void mark_tape();
    std::uint32_t player_ = UINT32_MAX;
    std::vector<std::uint32_t> cables_; // each car's control-cable bits last step (Wagon Flags 0x30000)
    std::vector<abi::TrackItemsItem> track_items_; // what RTSGetTrackItems hands out

    // What an SMSObject::sms points to: the script's tables (SoundTableSpec).
    struct SoundTable {
        std::vector<std::wstring> names;
        std::vector<std::uint16_t> codes;
        std::vector<std::vector<std::uint16_t>> pair_on;
    };
    std::vector<std::unique_ptr<SoundTable>> sound_tables_;
    void* sound_table(std::int32_t index) const;
    // Track items as RTS keeps them (TrackItem and its kind's fields), by
    // TDB id; the player's train's list this step (RTS's 0x4808DC cache).
    std::unordered_map<std::uint32_t, std::unique_ptr<std::uint32_t[]>> items_;
    std::vector<abi::TrackItemsItem> window_;
    float items_wanted_ = 0.0f;
    void add_item(const TrackItemSpec& spec);
    // The route database's signals (RTSSignalData), by unit id.
    std::unordered_map<std::int32_t, std::unique_ptr<abi::RTSSignalData>> units_;
    abi::RTSSignalData* unit(std::int32_t id) const;
    // OPENRTS_DLLHOST_TRACE: each kind of these calls is logged once.
    mutable std::vector<std::wstring> traced_;
    void trace_once(const std::wstring& key, const std::wstring& line) const;

    // By-name lookups (RTSGetObjectByName and the loco and cab ones; RTS
    // scans the lists the same way). A DLL may ask every Run: Brakes.dll
    // looks its parameters up, some 85 a car each step. The scan's answer is
    // kept by owner (the Locomotive, Cabin or AuxLibrary pointer the DLL
    // passed), kind, code, sub, caller and the name as written: the scan
    // ignores case, so each spelling is an entry of its own with the scan's
    // answer, the first match; no object is an answer too. The objects are
    // the mirrors', in vectors only setup, reconsist, build_car, link_cars
    // and bind change, and each of them empties the cache first
    // (names_changed); the names are the host's (strings_), the DLLs read
    // them. OPENRTS_DLLHOST_NAME_CHECK scans every answer given from the
    // cache again and logs a summary every 200 steps.
    struct NameKey {
        const void* owner = nullptr;
        const void* caller = nullptr;
        std::int32_t where = 0; // RTSOAL_*: 1 loco, 2 cabin, 4 aux library
        std::int32_t code = 0;
        std::int32_t sub = 0;
        std::wstring name;
        bool operator==(const NameKey&) const = default;
    };
    struct NameKeyHash {
        std::size_t operator()(const NameKey& key) const noexcept;
    };
    std::unordered_map<NameKey, void*, NameKeyHash> names_;
    NameKey name_probe_; // the key being looked up (its string's buffer reused)
    NameCacheStats name_stats_;
    bool name_check_ = false;
    std::uint32_t name_check_steps_ = 0;
    void names_changed();
    template <class Scan>
    void* named(std::int32_t where, const void* owner, const Module* caller, int code, const wchar_t* name, int sub, Scan&& scan);
    void* scan_loco_by_name(const abi::Locomotive* loco, int code, const wchar_t* name);
    void* scan_cab_by_name(const abi::Cabin* cab, int code, const wchar_t* name);
    void* scan_aux_by_name(const abi::AuxLibrary* aux, int code, const wchar_t* name);
    void report_names();
};

const wchar_t* keep(std::deque<std::wstring>& strings, std::u16string_view text);

} // namespace openrts::dllhost
