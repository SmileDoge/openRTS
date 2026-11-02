#pragma once

// The object layouts legacy RTS locomotive DLLs are compiled against
// (TS_VERSION 104). Written from the field lists and the offsets rts1.exe
// uses (docs/reverse/train-dll.md, runtime-overview.md, brakes.md); the
// header RTS ships is not copied here. x86 only: pointers are 4 bytes and
// everything is packed to 4, as C++ Builder lays it out (bool 1 byte, enum
// int, wchar_t 2 bytes).

#include <cstddef>
#include <cstdint>

#if !defined(_M_IX86)
#error "ts_abi.hpp describes the 32-bit DLL ABI; build openrts-dllhost32 for Win32"
#endif

namespace openrts::dllhost::abi {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using wchar = wchar_t;

inline constexpr u32 ts_version = 104;
inline constexpr u32 adapter_compliance = 39;
inline constexpr u32 max_sound_queue = 96;
inline constexpr u32 max_msg_queue = 16;

// RTSObjType
enum ObjType : int {
    ot_switch = 1, ot_display = 2, ot_loco = 3, ot_door = 4, ot_area = 5, ot_window = 6,
    ot_cabin = 7, ot_cls = 8, ot_cls_sub = 9, ot_scr = 10, ot_scr_sub = 11, ot_engine = 12,
    ot_exhaust = 13, ot_free_anim = 14, ot_int_light = 15, ot_light = 16, ot_sound = 17,
    ot_tex_change = 18, ot_eng_param = 19, ot_slave_loco = 20, ot_wagon = 21, ot_aux_lib = 22,
    ot_signals_info = 23, ot_chair = 24, ot_seat = 25,
};

// RTSOAL_* (where RTSGetObject's object lives)
inline constexpr short at_loco = 1, at_cabin = 2, at_salon = 3, at_auxlib = 4;

// DLLV_* codes of DLLVersion
enum VersionCode : u32 {
    v_version = 1, v_switch, v_display, v_screen, v_loco, v_loco_e, v_loco_d, v_engine,
    v_engine_e, v_engine_d, v_light, v_cabin, v_int_light, v_cls_sub, v_object, v_wagon,
    v_activator, v_adapter_compliance, v_stack_size, v_track_item, v_track_items_item,
};

// GlobalMsg actions (GM_*)
inline constexpr u16 gm_door_open = 1, gm_door_close = 2, gm_door_unlock = 3, gm_door_lock = 4,
                     gm_sms_post = 5, gm_light_on = 7, gm_light_off = 8, gm_light_enable = 9,
                     gm_light_disable = 10, gm_tex_change = 11, gm_tex_revert = 12, gm_msg_post = 14;

#pragma pack(push, 4)

struct Object {
    u16 uid;
    void* tile; void* obj_x; void* composition; void* reserved1; void* reserved2;
    char sort, draw;
    void* shape; void* mesh; void* attach[2];
    float coord[3];
    u32 flags, collide_flags, collide_function;
    float q_direction[4];
    u32 vb_id;
    u8 detail_level, obj_type;
    u32 idx;
    float elevation, distance;
    void* cur_lod;
    float volume[4];
    void* anim; void* model_x;
};
static_assert(sizeof(Object) == 132);

struct InterfaceObject { // DLLInterfaceObject: RTS's adapter bookkeeping
    void* adapt_link;
    u32 adapt_status;
    void* adapters;
};

struct TrackPDP {
    short tile_x, tile_z;
    float x, y, z;
    u32 a1, a2;
    void* node;
    u32 sub_idx;
    float node_off, node_len, node_off_high;
    float radius;
    float direction[3];
    bool reversed;
};
static_assert(sizeof(TrackPDP) == 64);

struct Wagon;
struct TrackTDP {
    void* ds; Wagon* wag; TrackPDP* pos; void* node;
    TrackTDP* next; TrackTDP* prev;
    float time, distance;
};
static_assert(sizeof(TrackTDP) == 32);

struct EngineParam {
    wchar* name;
    float value;
    wchar* str;
};
static_assert(sizeof(EngineParam) == 12);

struct Locomotive;
struct SMSObject;

struct AuxLibrary : InterfaceObject {
    void* lib;
    Locomotive* loco;
    float* var;
    u16 num_params;
    u32 stack_size;
    EngineParam* params;
    SMSObject* sound;
    void* sound_obj;
    wchar* name;
    u32 lib_flags; // bit 0: this call is after the main DLL's
    void* change_loco; void* lost_master;
    void* alsn; void* speed_limit;
    void* init; void* run;
    void* can_switch; void* switched;
};
static_assert(sizeof(AuxLibrary) == 0x54);
static_assert(offsetof(AuxLibrary, lib_flags) == 48);
static_assert(offsetof(AuxLibrary, switched) == 80);

struct AuxLibStack {
    AuxLibrary* libs;
    u32 count;
};

struct Activator {
    Wagon* parent;
    void* anim_part;
    wchar* name; wchar* hint;
    float pos[4];
    u8 type;
    short parent_obj, location;
    float anim_speed;
    u32 state, set_state;
};
static_assert(sizeof(Activator) == 52);

struct ActDoor : Activator {
    u32 flags;
    u32 global_id;
    short connects[4];
    u32 num_traj_points;
    float* trajectory;
    float door_area[6];
    float anim_time, soundproof[2];
    void* out_node;
    short sms_trigger[2];
};
static_assert(sizeof(ActDoor) == 120);

struct ActSwitch : Activator {
    u32 num_states, initial_state, num_hints, fixed_state;
    u32 flags;
    u8 key[4];
    float anim_time;
    float* anim_scale;
    float current_frame;
    float prec_dx, prec_dy;
    u32 key_pressed;
    void* hints;
};
static_assert(sizeof(ActSwitch) == 104);
static_assert(offsetof(ActSwitch, flags) == 68);

struct ActDisplay : Activator {
    float values_range[4], granularity, fade[4];
    u32 flags;
    int show_hint;
    u32 num_hints;
    void* hints;
    float value;
};
static_assert(sizeof(ActDisplay) == 108);


struct FreeAnimation {
    wchar* name;
    u32 num_nodes, num_key_frames, animate_to;
    void** nodes;
    u32 flags; // bit 0 running, 1 continuous, 2 loop; byte 1 bit 0 goes down
    float anim_time;
    float* anim_scale;
    float speed, speed_q, max_speed;
    wchar** node_names;
};
static_assert(sizeof(FreeAnimation) == 48);

struct TexChange {
    u32 tex_id, global_id;
    u16 state, set_state;
    wchar* old_name; wchar* name;
    u32 change;
};
static_assert(sizeof(TexChange) == 24);

// A sub-object of a SCREEN display or close-up scene (RTS loader 0x519B8C).
// Pt by type: ARROW center x y, ValuesRange in 2..5; LABEL 0 its font
// record, 1 the text buffer's size; BAR 0 style, 2..5 range; GRID 0 / 1 rows
// shown / on the texture, 2 / 3 columns; SECTOR 2..5 range, 6 shape + 10 *
// fill type, 7 granularity. ParentSub is kept + 1 (0: none).
struct CloseUpSubObject {
    u32 id, type, shader;
    Wagon* parent;
    u32 flags, anim_flags;
    wchar* hint;
    float area[4], uv[4], act[4], pt[8], rotate[3];
    float anim_time;
    float* anim_scale;
    float anim_speed, frame, value, fade[4];
    int current_tex, state, set_state;
    u32 colour;
    u32 num_tex, num_frames;
    void* tex; void* frames;
    CloseUpSubObject* references; CloseUpSubObject* belongs_to_list;
    u32 parent_sub, parent_list_count;
};
static_assert(sizeof(CloseUpSubObject) == 204);
static_assert(offsetof(CloseUpSubObject, state) == 160);

struct CloseUpScene : Activator {
    u32 current_background, num_back, shader;
    void* backs; float* uvs;
    u32 num_sub;
    CloseUpSubObject* obj;
    u32 s_prev;
    void* adj[4];
    bool transparent;
    float camera_pos[3], camera_dir[4], camera_fov;
    u32 colour;
};
static_assert(sizeof(CloseUpScene) == 140);
static_assert(offsetof(CloseUpScene, obj) == 76);

struct ActScreen {
    float pos[4], norm[3], pivot[3], up[3], width, height;
    u32 num_sub;
    CloseUpSubObject* obj;
    ActScreen* reference_scr;
    CloseUpScene* reference_scn;
};
static_assert(sizeof(ActScreen) == 76);

struct RTSSignalData {
    short tile[2];
    float pos[3];
    wchar* name;
};

struct SignalsInfo {
    float distance;
    u32 aspect[4], flags;
    float speed_limit;
    RTSSignalData* signal;
};
static_assert(sizeof(SignalsInfo) == 32);

struct LightObj {
    u32 flags, type; // bit 0 on, 1 fading out, 2 fading in, 3 turn on, 4 reversed
    u32 colour;
    float position[3], dir[4], uv[4], vol[4], vol1[4];
    float radius, phase, fade_in, fade_out;
    float attenuation[4], array_stride[4], source_radius, array_item_length;
    u32 array_size;
    float spot_exp;
    void* parent; void* light; void* next;
};
static_assert(sizeof(LightObj) == 164);

struct InternalLight {
    u32 flags;
    void* obj;
    float position[3], points[3], radius[3], fade[4], spot_exp, spot_gen;
    u32 colour, proj_size;
    void* light;
    u32 state, global_id;
    float auto_on_world_lit, attenuation[4], array_stride[4], array_item_length, source_radius;
    u32 array_size;
};
static_assert(sizeof(InternalLight) == 136);

struct ExhaustContr {
    float rate, magnitude, dispersion, mass;
    u32 colour;
};

struct Wagon : InterfaceObject {
    u8 is_locomotive, panto_raised, coupling;
    u32 uid;
    void* train_in;
    float durability;
    u32 flags, flags1;
    float mass;
    u32 passengers;
    float brake_force, cylinder_pressure, independent_brake_pressure, auxiliary_pressure, ept_value, pt_value;
    float velocity, acceleration, resistance, slope, force;
    double zord;
    float brake_cylinder_pressure, train_pipe_pressure, train_pipe_rate, charging_pipe_pressure, handbrake_value;
    float osc[8];
    Object* obj;
    TrackPDP tr_pos[2];
    TrackTDP tr_pos_t;
    void* wag;
    u16 num_doors, num_ladders, num_anim, num_ledges, num_planes, num_fr_anims;
    ActDoor* doors;
    void* ladders; void* ledges; void* planes;
    void* anim_parts;
    u32 num_bogies;
    void* bogies; void* wheels;
    FreeAnimation* anims;
    LightObj* lights;
    void* fr_anim; void* interior;
    SMSObject* sound;
    void* sound_obj;
    void* wagon_objs;
};
static_assert(offsetof(Wagon, is_locomotive) == 0x0C);
static_assert(offsetof(Wagon, flags) == 0x1C);
static_assert(offsetof(Wagon, velocity) == 0x44);
static_assert(offsetof(Wagon, force) == 0x54);
static_assert(offsetof(Wagon, zord) == 0x58);
static_assert(offsetof(Wagon, train_pipe_rate) == 0x68);
static_assert(offsetof(Wagon, tr_pos) == 0x98);
static_assert(offsetof(Wagon, wag) == 0x138);
static_assert(offsetof(Wagon, doors) == 328);
static_assert(offsetof(Wagon, anims) == 360);
static_assert(offsetof(Wagon, lights) == 364);
static_assert(offsetof(Wagon, sound) == 376);
static_assert(sizeof(Wagon) == 0x184);

struct Engine;
struct Cabin : InterfaceObject {
    Wagon* parent;
    void* nodes;
    void* shape;
    u32 num_switches, num_displays, num_anim, num_sw_decor;
    u32 signal_block_check_ahead, signal_num_lights, signal_num_aspects, signal_scripted;
    void** signals;
    u32* signal_aspects;
    u32 signal_aspects1;
    u32 num_areas, num_planes, num_doors, num_windows, num_chairs,
        num_tex_changes, num_lights, num_scenes, num_seats, num_ledges;
    SignalsInfo signal;
    u32 flags;
    ActSwitch* switches; ActSwitch* switches_decor;
    ActDisplay* displays;
    FreeAnimation* anims;
    void* areas;
    float* planes;
    int* plane_parent;
    ActDoor* doors;
    void* windows; void* chairs; void* seats;
    TexChange* tex;
    InternalLight* light;
    CloseUpScene* cls;
    void* ledges;
    void* player_in_chair;
    Object obj;
};
static_assert(offsetof(Cabin, num_switches) == 24);
static_assert(offsetof(Cabin, signal_block_check_ahead) == 40);
static_assert(offsetof(Cabin, signal_scripted) == 52);
static_assert(offsetof(Cabin, switches) == 144);
static_assert(offsetof(Cabin, cls) == 196);
static_assert(sizeof(Cabin) == 340);

struct Locomotive : Wagon {
    u32 loco_flags; // bit 0 master, 1 direction flipped, 2 in player's join, 3 cold, 4 has cabin,
                    // 5 script loaded, 6 cabin loaded for player, 7 AI; 8 sanding, 9 panto overburnt
    u32 num_engines, num_slaves, num_axles;
    float main_res_pressure;
    float sand_left, last_sanding, panto_burn_timer;
    Cabin* cab;
    u32 lib_param, stack_size;
    Locomotive** slaves;
    Wagon** first_in_group; Wagon** last_in_group;
    Engine* loco_a;
    void* ai;
    void* can_work_with;
    void* change_loco; void* lost_slave;
    void* alsn;
    void* speed_limit;
    void* lib_module;
    AuxLibStack* aux_libs;
};
static_assert(offsetof(Locomotive, loco_flags) == 0x184);
static_assert(offsetof(Locomotive, main_res_pressure) == 0x194);
static_assert(offsetof(Locomotive, cab) == 0x1A4);
static_assert(offsetof(Locomotive, slaves) == 0x1B0);
static_assert(offsetof(Locomotive, loco_a) == 0x1BC);
static_assert(offsetof(Locomotive, can_work_with) == 452);
static_assert(offsetof(Locomotive, lib_module) == 0x1D8);
static_assert(sizeof(Locomotive) == 0x1E0);

struct ElectricEngine;
struct DieselEngine;

struct ElectricLocomotive : Locomotive {
    void* eng;
    ElectricEngine* loco;
    void* init; void* run;
    void* can_switch; void* switched;
    float line_voltage, line_freq, line_current;
    void* line;
};
static_assert(offsetof(ElectricLocomotive, run) == 0x1EC);
static_assert(offsetof(ElectricLocomotive, can_switch) == 496);
static_assert(offsetof(ElectricLocomotive, line_voltage) == 0x1F8);
static_assert(sizeof(ElectricLocomotive) == 0x208);

struct DieselLocomotive : Locomotive {
    void* eng;
    DieselEngine* loco;
    void* init; void* run;
    void* can_switch; void* switched;
    float fuel_amount;
    u16 num_ex;
    void* ex;
};
static_assert(offsetof(DieselLocomotive, fuel_amount) == 0x1F8);
static_assert(sizeof(DieselLocomotive) == 0x204);

struct Engine : InterfaceObject {
    Cabin* cab;
    SMSObject* sound;
    float* var;
    EngineParam* parameters;
    float power, force, brake_force;
    float* engine_voltage; float* engine_current; float* engine_force;
    float independent_brake_value, train_pipe_rate, auxilary_rate, main_res_rate, charging_rate,
          handbrake_percent, dynamic_brake_percent, ept_value, ur;
    float external_power_source, external_power_source_voltage;
    double battery_charge;
    u16 brake_systems_engaged, alsn_on, sanding, num_lights, num_parameters;
    u32 engine_flags, flags, flags2;
    int reverse;
    u32 wheelslip;
    u32 throttle_position;
    float current_milepost;
    u16 global_msg[max_msg_queue * 2];
};
static_assert(offsetof(Engine, power) == 28);
static_assert(offsetof(Engine, independent_brake_value) == 52);
static_assert(offsetof(Engine, ur) == 84);
static_assert(offsetof(Engine, battery_charge) == 96);
static_assert(offsetof(Engine, brake_systems_engaged) == 104);
static_assert(offsetof(Engine, engine_flags) == 116);
static_assert(offsetof(Engine, reverse) == 128);
static_assert(offsetof(Engine, global_msg) == 144);
static_assert(sizeof(Engine) == 208);

struct ElectricEngine : Engine {
    u8 panto, main_switch;
    float power_consuming;
};
static_assert(sizeof(ElectricEngine) == 216);

struct DieselEngine : Engine {
    u8 diesel_on;
    float fuel_consuming, rpm;
    void* ex;
};
static_assert(sizeof(DieselEngine) == 224);

struct TrackItem {
    u32 type, id;
    float s_node_off;
    u32 s_data_flags;
    float p_data_x, p_data_z;
    short p_data_tile_x, p_data_tile_z;
    float r_data_x, r_data_y, r_data_z;
    short r_tile_x, r_tile_z;
};
static_assert(sizeof(TrackItem) == 44);

struct TrackItemsItem {
    float distance, z;
    u16 pos;
    TrackItem* obj;
};
static_assert(sizeof(TrackItemsItem) == 16);

struct SMSObject {
    void* sms;
    u32 flags; // bit 0 discrete triggers, 1 on, 2 starting
    u16 queue[max_sound_queue], queue_pos;
    float speed[2], var1[2], var2[2], var3[2], var4[2], var5[2], var6[2], var7[2], var8[2],
          distance[2], trav[2], accel[2], radius[2];
    float volume;
    void* streams;
    u32 loco_flags, triggers_processed, step_count;
    void* paired_flags;
};
static_assert(offsetof(SMSObject, queue_pos) == 200);
static_assert(sizeof(SMSObject) == 332);

struct SpeedLimitDescr {
    float limit, next_limit, distance;
};

#pragma pack(pop)

// Sizes DLLVersion reports for a DLL built against this layout.
inline u32 expected_size(const u32 code) {
    switch (code) {
    case v_version: return ts_version;
    case v_switch: return sizeof(ActSwitch);
    case v_display: return sizeof(ActDisplay);
    case v_screen: return sizeof(ActScreen);
    case v_loco: return sizeof(Locomotive);
    case v_loco_e: return sizeof(ElectricLocomotive);
    case v_loco_d: return sizeof(DieselLocomotive);
    case v_engine: return sizeof(Engine);
    case v_engine_e: return sizeof(ElectricEngine);
    case v_engine_d: return sizeof(DieselEngine);
    case v_light: return sizeof(LightObj);
    case v_cabin: return sizeof(Cabin);
    case v_int_light: return sizeof(InternalLight);
    case v_cls_sub: return sizeof(CloseUpSubObject);
    case v_object: return sizeof(Object);
    case v_wagon: return sizeof(Wagon);
    case v_activator: return sizeof(Activator);
    case v_track_item: return sizeof(TrackItem);
    case v_track_items_item: return sizeof(TrackItemsItem);
    default: return 0;
    }
}

// Exported entry points (all cdecl).
using InitRTSLibraryFn = bool (*)(void* module, void* dll);
using DLLVersionFn = u32 (*)(u32 code);
using InitFn = bool (*)(Engine* eng, Locomotive* loco, u32 state, float time, float air_temperature);
using RunFn = void (*)(Engine* eng, Locomotive* loco, u32 state, float time, float air_temperature);
using ChangeLocoFn = void (*)(Locomotive* loco, const Locomotive* prev, u32 state);
using LostMasterFn = void (*)(Locomotive* loco, const Locomotive* master, u32 state);
using CanWorkWithFn = bool (*)(const Locomotive* loco, const wchar* series);
using CanSwitchFn = bool (*)(const Locomotive* loco, Engine* eng, u32 id, u32 set_state);
using SwitchedFn = void (*)(const Locomotive* loco, Engine* eng, u32 id, u32 previous);
using SpeedLimitFn = void (*)(const Locomotive* loco, SpeedLimitDescr route, SpeedLimitDescr signal, SpeedLimitDescr event);
using ALSNFn = void (*)(const Locomotive* loco, SignalsInfo* a, u32 na, SignalsInfo* b, u32 nb, SignalsInfo* c, u32 nc,
                        float limit, int x, int y, bool reversed);

using AuxInitFn = bool (*)(AuxLibrary* lib, Locomotive* loco, u32 state, float time, float air_temperature);
using AuxRunFn = void (*)(AuxLibrary* lib, Locomotive* loco, u32 state, float time, float air_temperature);
using AuxChangeLocoFn = void (*)(AuxLibrary* lib, const Locomotive* loco, u32 state);
using AuxCanSwitchFn = bool (*)(const Locomotive* loco, AuxLibrary* lib, u32 id, u32 set_state);
using AuxSwitchedFn = void (*)(const Locomotive* loco, AuxLibrary* lib, u32 id, u32 previous);
using AuxSpeedLimitFn = void (*)(AuxLibrary* lib, SpeedLimitDescr route, SpeedLimitDescr signal, SpeedLimitDescr event);
using AuxALSNFn = void (*)(AuxLibrary* lib, SignalsInfo* a, u32 na, SignalsInfo* b, u32 nb, SignalsInfo* c, u32 nc,
                           float limit, int x, int y, bool reversed);

} // namespace openrts::dllhost::abi
