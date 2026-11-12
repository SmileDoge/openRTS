// A tiny locomotive DLL with the RTS ABI, for testing openrts-dllhost32
// without RTS content (tests/dllhost_test.cpp). It exercises what real DLLs
// do: host functions, the cab, sounds, GlobalMsg texts, master writes into a
// slave, CanSwitch refusals and Switched.

#include <windows.h>

#include <cstring>

#include "../src/ts_abi.hpp"

using namespace openrts::dllhost::abi;

namespace {

using GetLocoObjectFn = void* (*)(void* dll, const Locomotive* loco, int code, int index, int sub);
using GetCabObjectFn = void* (*)(void* dll, const Cabin* cab, int code, int index, int sub);
using GetIntegerFn = int (*)(const Locomotive* loco, unsigned code, int index);
using SetIntegerFn = int (*)(const Locomotive* loco, unsigned code, int value);
using GetTrackItemsFn = int (*)(void* dll, const Locomotive* loco, unsigned code, float distance, TrackItemsItem*& items,
                                unsigned& count);
using GetNamedIntegerFn = int (*)(const void* obj, unsigned code, const wchar_t* name);
using IsTriggerOnFn = int (*)(const SMSObject* sound, unsigned short code);
using GetObjectByNameFn = void* (*)(void* dll, const void* obj, short where, int code, const wchar_t* name, int sub);

void* g_dll = nullptr;
GetLocoObjectFn g_loco_object = nullptr;
GetCabObjectFn g_cab_object = nullptr;
GetIntegerFn g_integer = nullptr;
SetIntegerFn g_set_integer = nullptr;
int g_tape_answer = -1;
GetTrackItemsFn g_track_items = nullptr;
GetNamedIntegerFn g_named = nullptr;
IsTriggerOnFn g_trigger_on = nullptr;
GetObjectByNameFn g_by_name = nullptr;
wchar_t g_text[] = L"fake: init done";

ActSwitch* sw(const Locomotive* loco, unsigned id) {
    auto* cab = static_cast<Cabin*>(g_loco_object(g_dll, loco, ot_cabin, 0, 0));
    return cab ? static_cast<ActSwitch*>(g_cab_object(g_dll, cab, ot_switch, static_cast<int>(id), 0)) : nullptr;
}

void post(SMSObject* sound, u16 trigger) {
    for (u32 i = sound->queue_pos; i < max_sound_queue; ++i) {
        if (!sound->queue[i]) {
            sound->queue[i] = trigger;
            return;
        }
    }
}

} // namespace

extern "C" {

bool __cdecl InitRTSLibrary(HINSTANCE module, void* dll) {
    g_dll = dll;
    g_loco_object = reinterpret_cast<GetLocoObjectFn>(GetProcAddress(module, "_RTSGetLocoObject"));
    g_cab_object = reinterpret_cast<GetCabObjectFn>(GetProcAddress(module, "_RTSGetCabObject"));
    g_integer = reinterpret_cast<GetIntegerFn>(GetProcAddress(module, "_RTSGetInteger"));
    g_set_integer = reinterpret_cast<SetIntegerFn>(GetProcAddress(module, "_RTSSetIntegerG"));
    g_track_items = reinterpret_cast<GetTrackItemsFn>(GetProcAddress(module, "_RTSGetTrackItems"));
    g_named = reinterpret_cast<GetNamedIntegerFn>(GetProcAddress(module, "_RTSGetNamedInteger"));
    g_trigger_on = reinterpret_cast<IsTriggerOnFn>(GetProcAddress(module, "_RTSIsSMSTriggerOn"));
    g_by_name = reinterpret_cast<GetObjectByNameFn>(GetProcAddress(module, "_RTSGetObjectByName"));
    return g_loco_object && g_cab_object && g_integer;
}

unsigned long __cdecl DLLVersion(unsigned code) {
    if (code == v_adapter_compliance) return adapter_compliance;
    if (code == v_stack_size) return 8;
    return expected_size(code);
}

bool __cdecl Init(ElectricEngine* eng, ElectricLocomotive* loco, unsigned state, float, float) {
    eng->var[0] = static_cast<float>(state & 0xFF);
    eng->var[1] = (state & 0x100) ? 1.0f : 0.0f; // the player is in this one
    if ((state & 3) == 3 || (state & 3) == 2) {
        eng->panto = 3;
        eng->main_switch = 1;
    }
    if (auto* s = sw(loco, 0)) s->state = s->set_state = 1; // SetBoth: no Switched
    eng->brake_systems_engaged = 1;
    return true;
}

void __cdecl ChangeLoco(Locomotive* loco, const Locomotive* prev, unsigned) {
    if (!prev) loco->loco_flags |= 1; // the first one taken becomes master
}

// A slave left its master's group (a control cable parted): marks itself.
void __cdecl LostMaster(Locomotive* loco, const Locomotive*, unsigned) {
    if (loco->loco_a) loco->loco_a->auxilary_rate = 7.0f;
}

// What ALSN was given: the three counts (ahead + 10 x behind + 100 x
// passing), the first aspect ahead and the next speed post (the integers
// carry floats).
void __cdecl ALSN(const Locomotive* loco, SignalsInfo* a, u32 na, SignalsInfo*, u32 nb, SignalsInfo*, u32 nc, float, int x, int y,
                  bool) {
    if (!loco->loco_a) return;
    loco->loco_a->charging_rate = static_cast<float>(na + 10 * nb + 100 * nc);
    loco->loco_a->handbrake_percent = na ? static_cast<float>(a[0].aspect[0]) : -1.0f;
    float limit = 0.0f, distance = 0.0f;
    std::memcpy(&limit, &x, sizeof limit);
    std::memcpy(&distance, &y, sizeof distance);
    loco->loco_a->dynamic_brake_percent = limit * 1000.0f + distance;
    // The first signal's RTSSignalData: its name's length x 1000 + its tile z.
    const RTSSignalData* data = na ? a[0].signal : nullptr;
    loco->loco_a->independent_brake_value = data && data->name ? static_cast<float>(lstrlenW(data->name) * 1000 + data->tile[1]) : -1.0f;
}

bool __cdecl CanWorkWith(const Locomotive*, const wchar_t* series) { return lstrcmpiW(series, L"fake") == 0; }

bool __cdecl CanSwitch(const Locomotive* loco, Engine*, unsigned id, unsigned set_state) {
    (void)loco;
    return !(id == 1 && set_state == 2); // switch 1 refuses state 2
}

void __cdecl Switched(const Locomotive* loco, Engine* eng, unsigned id, unsigned previous) {
    (void)previous;
    const auto* s = sw(loco, id);
    if (!s) return;
    if (id == 0) eng->throttle_position = s->state;
    if (id == 2) eng->reverse = s->state == 1 ? 1 : (s->state == 2 ? -1 : 0);
    post(loco->loco_a->sound, static_cast<u16>(100 + id));
}

void __cdecl Run(ElectricEngine* eng, ElectricLocomotive* loco, unsigned state, float time, float) {
    eng->var[2] += time;
    eng->power = static_cast<float>(g_integer(loco, 6, 0)); // RTS_TRAINDIR
    // Force along the loco's own front: throttle x 1 kN x reverser; the
    // velocity it sees is already in its own direction.
    eng->force = 1000.0f * static_cast<float>(eng->throttle_position) * static_cast<float>(eng->reverse);
    eng->brake_force = loco->velocity; // echo for the test
    // The tape: a velocity of 77 / 78 / 79 m/s asks RTSSetIntegerG(7, 1 / 0 /
    // 2 | 4); Wheelslip: its answer x 100 + RTSGetInteger 7 x 10 + LocoFlags 0x10000.
    if (g_set_integer && loco->velocity >= 76.5f && loco->velocity <= 79.5f) {
        const int value = loco->velocity < 77.5f ? 1 : loco->velocity < 78.5f ? 0 : 6;
        g_tape_answer = g_set_integer(loco, 7, value);
    } else {
        g_tape_answer = 0;
    }
    eng->wheelslip = static_cast<u32>(g_tape_answer * 100 + g_integer(loco, 7, 0) * 10 + ((loco->loco_flags & 0x10000u) ? 1 : 0));
    if (loco->loco_flags & 1) eng->ur = 5.0f; // the master's own; slaves get theirs from it
    // Track items ahead within 1 km and under the train: their count x 100 +
    // the first two types; ahead within 5 km: the first platform's station
    // name length (RTSGetTrackItems).
    {
        TrackItemsItem* items = nullptr;
        unsigned count = 0;
        eng->ept_value = -1.0f;
        if (g_track_items && g_track_items(g_dll, loco, 1 | 4, 1000.0f, items, count) >= 0) {
            float code = static_cast<float>(count) * 100.0f;
            if (count > 0 && items[0].obj) code += static_cast<float>(items[0].obj->type) * 10.0f;
            if (count > 1 && items[1].obj) code += static_cast<float>(items[1].obj->type);
            eng->ept_value = code;
        }
        eng->power_consuming = 0.0f;
        if (g_track_items && g_track_items(g_dll, loco, 1, 5000.0f, items, count) > 0) {
            for (unsigned i = 0; i < count; ++i) {
                if (!items[i].obj || items[i].obj->type != 3) continue;
                const auto* station = *reinterpret_cast<const wchar_t* const*>(reinterpret_cast<const u8*>(items[i].obj) + 56);
                eng->power_consuming = station ? static_cast<float>(lstrlenW(station)) : -1.0f;
                break;
            }
        }
    }
    // Parameters by name every Run, as Brakes.dll asks (RTSGetObjectByName):
    // "alpha" and "ALPHA" are both the first match, "Alpha" (its value), and
    // the same object; "missing" is none: value + 0.5 + 0.25.
    if (g_by_name) {
        const auto* first = static_cast<const EngineParam*>(g_by_name(g_dll, loco, at_loco, ot_eng_param, L"alpha", 0));
        const auto* upper = static_cast<const EngineParam*>(g_by_name(g_dll, loco, at_loco, ot_eng_param, L"ALPHA", 0));
        const void* missing = g_by_name(g_dll, loco, at_loco, ot_eng_param, L"missing", 0);
        eng->train_pipe_rate = (first ? first->value : -100.0f) + (first && first == upper ? 0.5f : 0.0f) + (missing ? 0.0f : 0.25f);
    }
    // The cab script's named trigger "Aux On" x 100 + whether its pair is on + 10.
    if (g_named && g_trigger_on && eng->sound)
        eng->main_res_rate = static_cast<float>(g_named(eng->sound->sms, 5, L"Aux On") * 100 + g_trigger_on(eng->sound, 1) + 10);
    // A master drives its slaves through their engines, as VL10k does.
    if (loco->loco_flags & 1) {
        for (u32 i = 0; i < loco->num_slaves; ++i) {
            auto* slave = static_cast<Locomotive*>(g_loco_object(g_dll, loco, ot_slave_loco, static_cast<int>(i), 0));
            if (slave && slave->loco_a) {
                slave->loco_a->ur = 4.25f;
                slave->loco_a->throttle_position = eng->throttle_position;
                slave->loco_a->reverse = eng->reverse;
            }
        }
    }
    if (eng->var[3] == 0.0f && (state & 0x100)) {
        eng->var[3] = 1.0f;
        post(eng->sound, 7);
        // ShowMessage: GM_MSG_POST, type, then the wchar_t* in two shorts.
        eng->global_msg[0] = gm_msg_post;
        eng->global_msg[1] = 2;
        wchar_t* text = g_text;
        std::memcpy(&eng->global_msg[2], &text, sizeof(text));
    }
}

} // extern "C"
