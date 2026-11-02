/* openRTS train plugin API: locomotive logic as a 64-bit DLL, the native
 * counterpart of legacy RTS locomotive DLLs (docs/train-dll.md).
 *
 * Rules:
 * - C ABI, every call cdecl, no C++ types across the boundary.
 * - Every struct starts with `size` (bytes, set by whoever fills it) and only
 *   grows by appending fields: read a field only if `size` covers it.
 * - One context per locomotive (create / destroy); no globals needed.
 * - Controls and indicators are found by the names of the cab's .sd
 *   (Switch / Display Name), once, and used through handles afterwards.
 * - Strings are UTF-8.
 *
 * A plugin DLL exports one function:
 *     const OrtsTrainPlugin* orts_train_plugin(uint32_t host_api_version);
 * and is named in the .eng:
 *     EngExtended ( ORTSTrainPlugin ( "cab/my_loco.dll" ) )
 */
#ifndef OPENRTS_TRAIN_PLUGIN_H
#define OPENRTS_TRAIN_PLUGIN_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ORTS_TRAIN_API_VERSION 1u

#if defined(_WIN32)
#define ORTS_PLUGIN_EXPORT __declspec(dllexport)
#else
#define ORTS_PLUGIN_EXPORT __attribute__((visibility("default")))
#endif

typedef int32_t OrtsHandle; /* < 0: not found */

/* OrtsTrainInputs::flags */
#define ORTS_INPUT_PLAYER 0x1u    /* the player drives from this locomotive */
#define ORTS_INPUT_FLIPPED 0x2u   /* the car faces the train rear */
#define ORTS_INPUT_MASTER 0x4u    /* it leads its multiple-unit group */

typedef struct OrtsTrainInputs {
    uint32_t size;
    double time;            /* simulation time, s */
    float speed;            /* m/s along the locomotive's own front */
    float acceleration;     /* m/s² along its front */
    float brake_pipe;       /* atm */
    float brake_cylinder;   /* atm */
    float main_reservoir;   /* atm */
    float independent;      /* independent brake cylinder, atm */
    float line_voltage;     /* V at the pantographs */
    float line_frequency;   /* Hz, 0 for DC */
    uint32_t flags;         /* ORTS_INPUT_* */
} OrtsTrainInputs;

typedef struct OrtsTrainOutputs {
    uint32_t size;
    float traction_force;    /* N along the locomotive's front */
    float brake_force;       /* N, dynamic brake */
    float brake_pipe_rate;   /* > 0 feed from the main reservoir, < 0 vent, atm/s (RTS meaning) */
    float main_reservoir_rate; /* atm/s, the compressor */
    float independent_brake; /* atm the independent cylinder goes to, < 0: unchanged */
    float equalising;        /* the equalising reservoir the valve holds, atm, < 0: none */
    float ep_value;          /* EP brake demand, atm, < 0: none */
    float sanding;           /* 0 or 1 */
    uint32_t pantographs;    /* bit n-1: pantograph n raised */
    uint32_t main_switch;    /* 0 open, 1 closed */
    uint32_t brake_systems;  /* 1 pipe, 2 EP, 4 second pipe */
    uint32_t emergency;      /* the driver's valve is in emergency */
} OrtsTrainOutputs;

typedef struct OrtsTrainSetup {
    uint32_t size;
    const char* engine_name;  /* .eng file stem */
    uint32_t engine_kind;     /* 1 electric, 2 diesel */
    uint32_t num_engines;
    uint32_t num_axles;
    float mass;               /* kg */
} OrtsTrainSetup;

/* Functions openRTS gives a plugin; `ctx` is the host's, pass it back. */
typedef struct OrtsHost {
    uint32_t size;
    void* ctx;
    OrtsHandle (*find_control)(void* ctx, const char* name);
    float (*control)(void* ctx, OrtsHandle control);              /* the state it is in */
    void (*set_control)(void* ctx, OrtsHandle control, float state); /* moves it (no request) */
    OrtsHandle (*find_indicator)(void* ctx, const char* name);
    void (*set_indicator)(void* ctx, OrtsHandle indicator, float value, uint32_t state);
    void (*sound_trigger)(void* ctx, uint32_t cab, uint32_t trigger); /* cab 1: the cab SMS, 0 exterior */
    float (*parameter)(void* ctx, const char* name, float fallback); /* EngineParameters */
    void (*message)(void* ctx, uint32_t kind, const char* utf8);     /* 1 box, 2 hint, 3 message line */
    void (*log)(void* ctx, uint32_t level, const char* utf8);
} OrtsHost;

typedef struct OrtsTrainPlugin {
    uint32_t size;
    uint32_t api_version;     /* ORTS_TRAIN_API_VERSION it was built for */
    const char* name;
    void* (*create)(const OrtsHost* host, const OrtsTrainSetup* setup);
    void (*destroy)(void* plugin);
    void (*step)(void* plugin, double dt, const OrtsTrainInputs* inputs, OrtsTrainOutputs* outputs);
    /* The driver asks to move a control: return 0 to refuse (CanSwitch). */
    int32_t (*control_request)(void* plugin, OrtsHandle control, float from, float to);
    /* A control got to its new state (Switched). */
    void (*control_changed)(void* plugin, OrtsHandle control, float previous);
} OrtsTrainPlugin;

typedef const OrtsTrainPlugin* (*OrtsTrainPluginEntry)(uint32_t host_api_version);

#ifdef __cplusplus
}
#endif

#endif
