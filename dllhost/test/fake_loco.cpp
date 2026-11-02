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

void* g_dll = nullptr;
GetLocoObjectFn g_loco_object = nullptr;
GetCabObjectFn g_cab_object = nullptr;
GetIntegerFn g_integer = nullptr;
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
    if (loco->loco_flags & 1) eng->ur = 5.0f; // the master's own; slaves get theirs from it
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
