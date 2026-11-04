// The 11 host functions a legacy DLL looks up in InitRTSLibrary with
// GetProcAddress(module, "_RTS...") (docs/reverse/train-dll.md section 7).
// exports.def gives them the exact underscored names rts1.exe exports.

#include "host.hpp"

using namespace openrts::dllhost;

namespace {

Host* host() { return Host::current(); }
const Module* caller(void* dll) { return host() ? host()->module_of(dll) : nullptr; }

} // namespace

extern "C" {

int __cdecl RTSGetInteger(const abi::Locomotive* loco, unsigned code, int index) {
    return host() ? host()->get_integer(loco, code, index) : 0;
}

int __cdecl RTSSetIntegerG(const abi::Locomotive* loco, unsigned code, int value) {
    return host() ? host()->set_integer(loco, code, value) : 0;
}

void* __cdecl RTSGetLocoObject(void* dll, const abi::Locomotive* loco, int code, int index, int sub) {
    return host() ? host()->loco_object(caller(dll), loco, code, index, sub) : nullptr;
}

void* __cdecl RTSGetLocoObjectByName(void* dll, const abi::Locomotive* loco, int code, const wchar_t* name, int sub) {
    return host() ? host()->loco_object_by_name(caller(dll), loco, code, name, sub) : nullptr;
}

void* __cdecl RTSGetCabObject(void* dll, const abi::Cabin* cab, int code, int index, int sub) {
    return host() ? host()->cab_object(caller(dll), cab, code, index, sub) : nullptr;
}

void* __cdecl RTSGetCabObjectByName(void* dll, const abi::Cabin* cab, int code, const wchar_t* name, int sub) {
    return host() ? host()->cab_object_by_name(caller(dll), cab, code, name, sub) : nullptr;
}

// RTSGetObject / ByName: `where` is RTSOAL_* (1 loco, 2 cabin, 4 aux library).
void* __cdecl RTSGetObject(void* dll, const void* obj, short where, int code, int index, int sub) {
    if (!host()) return nullptr;
    switch (where) {
    case abi::at_loco: return host()->loco_object(caller(dll), static_cast<const abi::Locomotive*>(obj), code, index, sub);
    case abi::at_cabin: return host()->cab_object(caller(dll), static_cast<const abi::Cabin*>(obj), code, index, sub);
    case abi::at_auxlib: return host()->aux_object(static_cast<const abi::AuxLibrary*>(obj), code, index, sub);
    default: return nullptr;
    }
}

void* __cdecl RTSGetObjectByName(void* dll, const void* obj, short where, int code, const wchar_t* name, int sub) {
    if (!host()) return nullptr;
    switch (where) {
    case abi::at_loco: return host()->loco_object_by_name(caller(dll), static_cast<const abi::Locomotive*>(obj), code, name, sub);
    case abi::at_cabin: return host()->cab_object_by_name(caller(dll), static_cast<const abi::Cabin*>(obj), code, name, sub);
    case abi::at_auxlib: return host()->aux_object_by_name(static_cast<const abi::AuxLibrary*>(obj), code, name, sub);
    default: return nullptr;
    }
}

int __cdecl RTSGetTrackItems(void* dll, const abi::Locomotive* loco, unsigned code, float distance,
                             abi::TrackItemsItem*& items, unsigned& count) {
    (void)dll;
    if (!host()) {
        items = nullptr;
        count = 0;
        return 0;
    }
    std::uint32_t n = 0;
    const int r = host()->track_items(loco, code, distance, items, n);
    count = n;
    return r;
}

// RTSGetNamedInteger(sms, RTS_TRIGGERID, name): the script's
// NamedTriggersTable, the name compared without case (RTS 0x4394F4); 0 for
// any other code or an unknown name.
int __cdecl RTSGetNamedInteger(const void* obj, unsigned code, wchar_t* name) {
    if (!host() || code != 5) return 0;
    return host()->named_integer(obj, name);
}

// RTSIsSMSTriggerOn: whether the trigger pair with this On code is on (RTS
// 0x439548; host.cpp).
int __cdecl RTSIsSMSTriggerOn(const abi::SMSObject* sound, unsigned short trigger) {
    if (!host()) return -3;
    return host()->trigger_on(sound, trigger);
}

} // extern "C"
