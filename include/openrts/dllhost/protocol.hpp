#pragma once

// Wire format between openRTS (x64) and openrts-dllhost32.exe (x86), the
// process that runs legacy RTS locomotive DLLs (docs/train-dll.md,
// docs/reverse/train-dll.md). Both sides compile this header, so it uses only
// fixed-size types and no pointers; every message is a flat byte stream
// written with Writer and read back with Reader in the same order.
//
// Transport: one named file mapping of shared_size bytes and two auto-reset
// events, "<name>.req" (game -> host) and "<name>.rep" (host -> game). The
// game writes a request at payload_offset, fills Header and signals .req; the
// host replaces the payload with the reply and signals .rep.

#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace openrts::dllhost {

inline constexpr std::uint32_t protocol_magic = 0x484C444Fu; // "ODLH"
inline constexpr std::uint32_t protocol_version = 8;
inline constexpr std::uint32_t shared_size = 8u << 20;
inline constexpr std::uint32_t payload_offset = 4096;
inline constexpr std::uint32_t payload_capacity = shared_size - payload_offset;

enum class Command : std::uint32_t {
    none = 0,
    hello = 1,     // -> host version, ABI sizes
    setup = 2,     // the consist: cars, DLL modules, aux libraries, cabs
    enter = 3,     // the player takes a locomotive: bind, Init, grouping
    step = 4,      // one simulation step: inputs in, outputs out
    can_switch = 5, // the player operates a switch: CanSwitch -> allowed
    shutdown = 6,
    drag_switch = 7, // the player drags a switch with the mouse -> moved
    reconsist = 8,   // cars coupled or cut off: the new car list, the DLLs keep their state
};

struct Header {
    std::uint32_t magic;
    std::uint32_t version;
    std::uint32_t command;      // Command
    std::int32_t status;        // reply: 0 ok, < 0 failed (message says why)
    std::uint32_t request_size; // bytes at payload_offset
    std::uint32_t reply_size;
    std::uint32_t sequence;     // request number, echoed
    std::uint32_t reserved;
    char16_t message[1024];     // reply: errors, warnings and DLL log lines
};
static_assert(sizeof(Header) <= payload_offset);

// Car kinds (ts.h Wagon::IsLocomotive, WCL_*).
inline constexpr std::uint8_t car_wagon = 0;
inline constexpr std::uint8_t car_electric = 1;
inline constexpr std::uint8_t car_diesel = 2;

// Auxiliary library events (RTS AuxCall keywords, 0x56EADC / 0x56EF60 codes).
inline constexpr std::uint16_t aux_run = 1;
inline constexpr std::uint16_t aux_alsn = 2;
inline constexpr std::uint16_t aux_speed_limit = 3;
inline constexpr std::uint16_t aux_switched = 4;
inline constexpr std::uint16_t aux_can_switch = 5;

class Writer {
public:
    Writer(std::uint8_t* data, std::uint32_t capacity) : data_(data), capacity_(capacity) {}

    template <class T>
    void put(const T& value) {
        static_assert(std::is_trivially_copyable_v<T>);
        bytes(&value, sizeof(T));
    }
    void bytes(const void* source, std::uint32_t size) {
        if (size > capacity_ - size_) {
            overflow_ = true;
            return;
        }
        std::memcpy(data_ + size_, source, size);
        size_ += size;
    }
    // UTF-16, length first.
    void text(std::u16string_view value) {
        put(static_cast<std::uint32_t>(value.size()));
        bytes(value.data(), static_cast<std::uint32_t>(value.size() * sizeof(char16_t)));
    }
    [[nodiscard]] std::uint32_t size() const noexcept { return size_; }
    [[nodiscard]] bool overflow() const noexcept { return overflow_; }

private:
    std::uint8_t* data_;
    std::uint32_t capacity_;
    std::uint32_t size_ = 0;
    bool overflow_ = false;
};

class Reader {
public:
    Reader(const std::uint8_t* data, std::uint32_t size) : data_(data), size_(size) {}

    template <class T>
    T get() {
        static_assert(std::is_trivially_copyable_v<T>);
        T value{};
        bytes(&value, sizeof(T));
        return value;
    }
    void bytes(void* target, std::uint32_t size) {
        if (size > size_ - pos_) {
            failed_ = true;
            std::memset(target, 0, size);
            pos_ = size_;
            return;
        }
        std::memcpy(target, data_ + pos_, size);
        pos_ += size;
    }
    std::u16string text() {
        const auto length = get<std::uint32_t>();
        if (length > (size_ - pos_) / sizeof(char16_t)) {
            failed_ = true;
            pos_ = size_;
            return {};
        }
        std::u16string value(length, u'\0');
        bytes(value.data(), static_cast<std::uint32_t>(length * sizeof(char16_t)));
        return value;
    }
    [[nodiscard]] bool failed() const noexcept { return failed_; }
    [[nodiscard]] bool done() const noexcept { return pos_ == size_; }

private:
    const std::uint8_t* data_;
    std::uint32_t size_;
    std::uint32_t pos_ = 0;
    bool failed_ = false;
};

// ---------------------------------------------------------------------------
// Messages. Each is written field by field by write(), read by read(); the
// structs themselves never cross the process boundary as a whole.

struct Parameter { // EngineParameters / AuxLibraryParameters entry
    std::u16string name;
    float value{};
    std::u16string text; // the string form when the value was quoted
};

struct AuxCallSpec {
    std::uint16_t event{}; // aux_*
    std::int16_t order{};  // < 0: before the main DLL, otherwise after
};

struct AuxLibrarySpec {
    std::u16string name;   // AuxLibraryName
    std::u16string module; // absolute path of the DLL
    std::uint32_t stack_size{};
    std::vector<Parameter> parameters;
    std::vector<AuxCallSpec> calls;
    bool has_sound = false; // AuxLibrarySound given: the library gets an SMS object
    std::int32_t sound_table = -1; // its SMS's SetupRequest::sound_tables entry, -1 none
};

// What a DLL can ask of a sound script (RTS 0x4394F4, 0x439548): its
// NamedTriggersTable (name, code), and the On codes of each of its group's
// DiscreteTriggerPairs pairs. One per script; SMS objects refer to it by
// index.
struct SoundTableSpec {
    std::vector<std::u16string> names;
    std::vector<std::uint16_t> codes;
    std::vector<std::vector<std::uint16_t>> pair_on;
};

struct SwitchSpec { // .sd Switch: what the mirror ActSwitch starts with
    std::uint32_t id{};
    std::uint32_t num_states{};
    std::uint32_t initial_state{};
    std::uint32_t fixed_state{}; // .sd FixedState (0 when not given)
    std::uint32_t flags{};     // ActSwitch::Flags (CONTROLLERX 1, KEY 2, NONFIXED 4, ...)
    float anim_time{};
    std::uint8_t keys[4]{};
    std::u16string name;
    float precision[2]{}; // SwitchPrecision PrecX, PrecY (ActSwitch prec_dx / prec_dy)
    std::int16_t parent = -1; // ParentObject: the switch it follows (Activator +34)
    // Its node's Shortcut sets (ESD_Extended_Animation), as positions of the
    // switch (a KEY switch's key-out state counted), each ascending.
    std::vector<std::vector<float>> shortcuts;
};

// A SCREEN display's sub-object as the host builds it (RTS CloseUpSubObject).
struct ScreenObjectSpec {
    std::uint32_t type{};          // formats::ScreenObjectType
    std::uint32_t flags{};
    std::int32_t initial_state{};
    std::int32_t parent = -1;      // ParentSub
    std::uint32_t text_size{};     // LABEL: buffer of text_size characters with the 0 (0: none)
    std::uint32_t frames{};
    std::uint32_t textures{};
    std::uint32_t shader{};
    std::uint32_t colour{};
    float area[4]{}, uv[4]{}, pt[8]{};
    float anim_time{}, fade{}, blink = -1.0f, value{};
};

struct DisplaySpec {
    std::uint32_t id{};
    float range[4]{};
    float granularity{};
    std::uint32_t flags{};
    std::uint32_t num_hints{};
    std::u16string name;
    std::uint32_t initial_state{};  // ActivatorObject::State from InitialState (RTS 0x51CD88)
    // SCREEN: the quad (top left, top right, bottom right in .sd axes) and
    // its sub-objects; reference_screen: shows that display's objects.
    bool has_screen = false;
    float corners[9]{};
    std::int32_t reference_screen = -1;
    std::vector<ScreenObjectSpec> screen;
};

struct CabSpec {
    bool present = false;
    std::vector<SwitchSpec> switches;    // indexed by .sd ID
    std::vector<DisplaySpec> displays;   // indexed by .sd ID
    std::uint32_t internal_lights{};
    std::uint32_t tex_changes{};
    std::uint32_t doors{};
    std::uint32_t signal_lights{};       // SignalNumLights
    std::uint32_t signal_aspects{};
    bool signal_scripted = false;        // Cabin::SignalScripted: ALSN is called
    std::vector<std::u16string> animations; // FreeAnimation names
    std::uint32_t switch_count{};        // Switches ( n ): RTS makes n switches
    std::uint32_t display_count{};       // Displays ( n )
    std::vector<std::uint32_t> internal_light_groups; // LightSource GlobalID, by lamp
    std::vector<std::uint32_t> tex_change_images;     // TexChange TexID, by change
    std::vector<std::uint32_t> tex_change_groups;     // TexChange GlobalID, by change
    std::vector<std::u16string> door_names;           // InteriorDoor Name, by door
    std::vector<std::uint32_t> door_groups;           // InteriorDoor GlobalID, by door
};

// An exterior light as the host fills its LightObj (Lights in the .eng).
struct LightSpec {
    std::uint32_t type{}, colour{};
    float radius{}, fade_in{}, fade_out{}, spot_exponent{};
    float position[3]{};
};

struct CarSpec {
    std::uint8_t kind = car_wagon;
    std::uint32_t uid{};
    std::uint32_t key{};          // this car in the game (0: none): follows it through couplings
    float mass{};
    std::uint32_t lights{};       // exterior LightObj count
    std::uint32_t doors{};
    std::vector<std::u16string> animations; // FreeAnimation names (wagon shape)
    std::vector<float> door_times;           // ExteriorDoor AnimTime, by door
    std::vector<std::uint32_t> door_groups;  // ExteriorDoor GlobalID, by door
    std::vector<LightSpec> light_specs;      // by LightID (lights of them)
    std::u16string name;          // ENG / WAG file stem
    // Locomotives with a DLL:
    std::u16string series;        // LocomotiveSeries (CanWorkWith)
    std::u16string module;        // absolute path of LibraryModule, empty: none
    std::uint32_t lib_param{};
    std::uint32_t stack_size{};
    std::uint32_t num_engines{};
    std::uint32_t num_axles{};
    std::vector<Parameter> parameters;
    std::vector<AuxLibrarySpec> aux;
    CabSpec cab;
    // The exterior and cab SMS scripts' SetupRequest::sound_tables entries, -1 none.
    std::int32_t sound_ext = -1, sound_cab = -1;
};

struct SetupRequest {
    std::u16string rts_root;      // searched for bass.dll / CC3260MT.DLL
    std::uint8_t train_type = 1;  // RTS train type: 1 the player's
    std::vector<CarSpec> cars;
    std::vector<SoundTableSpec> sound_tables;
};

struct ModuleReport {             // per loaded module, in the setup reply
    std::u16string path;
    std::int32_t status{};        // 0 ok, < 0 refused
    std::uint32_t version[22]{};  // DLLVersion(1..21), [0] unused
};



struct SpeedLimitInput {          // SpeedLimitDescr, m/s and m; -1 unknown
    float limit = -1.0f, next_limit = -1.0f, distance = -1.0f;
};

struct SignalInput {              // SignalsInfo; the RTSSignalData as a unit number
    float distance{};
    std::uint32_t aspect[4]{};
    std::uint32_t flags{};
    float speed_limit = -1.0f;
    std::int32_t unit = -1;       // SignalUnitSpec::id of the signal's route database entry, -1 none
};

// A signal of the route database as RTS hangs it on the TDB signal item
// (RTSSignalData, 0x4903DC): its tile, its position in RTS axes (x
// negated) and its name. Sent once; SignalInput::unit refers to it.
struct SignalUnitSpec {
    std::int32_t id{};
    std::int16_t tile[2]{};
    float position[3]{};
    std::u16string name;
};

// An SMS object's trigger pair states (RTS SMSObject::PairedFlags: 4 words
// of pairs on, 4 of pairs known; 0x4A87C8). Sent for the objects that have
// had a pair code.
inline constexpr std::int16_t sound_exterior = -1;
inline constexpr std::int16_t sound_cabin = -2; // else an AuxLibrary's index
struct SoundPairState {
    std::uint32_t car{};
    std::int16_t object{};
    std::uint32_t on[4]{}, known[4]{};
};

// A track item as RTS keeps it (TrackItem and its kind's fields, 0x482680):
// sent once per item. Positions in RTS axes (x negated).
inline constexpr std::uint16_t item_ahead = 1, item_behind = 2, item_under = 4;
struct TrackItemSpec {
    std::uint32_t id{}, type{};          // TIT_*: 1 signal .. 8 car spawner, 0 other
    float node_offset{};                 // TrItemSData
    std::uint32_t data_flags{};
    float pdata[2]{};                    // TrItemPData x z, tile
    std::int16_t ptile[2]{};
    float rdata[3]{};                    // TrItemRData x y z, tile
    std::int16_t rtile[2]{};
    std::uint32_t flags{};               // speed post Flags; platform / siding TrItemFlags; pickup PickupFlags
    std::uint32_t link{};                // platform / siding TrItemConj; crossover Conj
    std::uint32_t section{};             // crossover SectionIDX
    float value{}, off{}, km{};          // speed post Value (m/s), Off, KM; pickup Amount in value
    std::int32_t number{};               // speed post Number
    std::uint32_t min_wait{}, passengers{}; // platforms
    std::u16string name, station;        // platform / siding name, station
};
// One entry of RTSGetTrackItems (TrackItemsItem): the distance from the
// train's front (ahead, under it: negative) or rear (behind), the signed
// distance from its rear along it, ahead / behind / under, and the item.
struct TrackItemEntry {
    float distance{}, z{};
    std::uint16_t pos{};
    std::uint32_t item{};
};

struct SwitchState {
    std::uint32_t state{}, set_state{}, fixed_state{}, flags{};
    float current_frame{};
};

struct DisplayState {
    float value{};
    std::uint32_t state{};
    std::int32_t show_hint{};
    float fade[4]{};
};

// A screen sub-object after the step: State (= SetState, as RTS copies it
// when it draws), Value, Fade and Area the DLL may have changed.
struct ScreenObjectState {
    std::int32_t state{};
    float value{};
    float fade[4]{};
    float area[4]{};
};

struct ScreenOutput {
    std::uint32_t display{};
    std::vector<ScreenObjectState> objects;
    std::vector<std::u16string> texts; // per object: a LABEL's text, else empty
};

struct CarInput {
    std::uint32_t flags{};        // Wagon::Flags as RTS keeps them (bit 0 flipped, 2 player in, ...)
    std::uint8_t coupling{};
    float mass{};
    float velocity{}, acceleration{}; // towards the train front
    float resistance{}, slope{};
    double zord{};
    float brake_force{}, brake_cylinder{}, independent{}, auxiliary{}, control{};
    float train_pipe{}, charging_pipe{}, handbrake{};
    float main_reservoir{};       // locomotives
    float line_voltage{}, line_frequency{}; // electric
    float line_current{};         // electric: Locomotive::LineCurrent (RTS 0x503CE4, before Run)
    std::uint8_t line_burnt{};    // electric: the pantograph burnt the wire (LocoFlags 0x200)
};

struct EnterRequest {
    std::uint32_t car{};          // the locomotive the player takes
    bool running = true;          // State bit 1 (the train starts running); bit 0 "has cars" is the host's
    // The low byte of Init's State as an activity's TrainState gives it (0 cold,
    // 1 standing with cars, 2 / 3 running); -1: from the train (bits 0 and 1 above).
    std::int32_t state = -1;
    std::vector<CarInput> cars;   // the consist as it stands (flags: cables, player)
};

// The trains after a coupling or a cut (RTS 0x469ED8 / 0x469858): every
// car says where its mirror comes from. The mirrors of the cars that stay
// keep their DLLs' state (RTS binds a DLL to the car once, 0x5026FC). The
// host runs several trains: the player's first, then the parts cut off
// from it, whose DLLs RTS keeps running (the cut train keeps the train's
// flag 4, 0x4EEC50; Run goes to every such train, 0x424488). A car in no
// train is kept aside until it comes back. The groups are made again
// (0x4F995C: new slaves bound and initialised, lost ones LostMaster).
inline constexpr std::int32_t reconsist_new = -1;    // built from `spec`
inline constexpr std::int32_t reconsist_parked = -2; // the car cut off earlier with `spec.key`
struct ReconsistCar {
    std::int32_t from = reconsist_new; // >= 0: the car now at that index
    CarSpec spec;                      // for a new car (a parked one: only its key)
};

struct ReconsistTrain {
    std::uint8_t type = 3;            // RTS train type: 1 the player's, 3 a part cut off (LOOSE)
    std::vector<ReconsistCar> cars;
};

struct ReconsistRequest {
    std::vector<SoundTableSpec> sound_tables; // the new cars' scripts, numbered after the setup's
    std::vector<ReconsistTrain> trains;       // [0] the player's
    bool running = false;                     // Init's State bit 1 for engines bound now (the train moves)
    std::vector<CarInput> inputs;             // every train's cars in order (their cables decide the groups)
};

struct StepRequest {
    double day_time{};            // s since midnight (RTSGetInteger 1)
    float dt{};
    float air_temperature = 20.0f;
    std::uint8_t night = 0;
    std::uint8_t train_direction = 1; // 1 along car 0's front, 0 reversed
    std::vector<CarInput> cars;
    SpeedLimitInput route, signal, event;
    // The next signal the train obeys ahead of its leading end
    // (Cabin::Signal of the leading cab); at most one.
    std::vector<SignalInput> signals_ahead;
    // The ALSN arrays (RTS 0x508520): the signal objects ahead of the
    // train's front not yet reached, those behind its rear, and those its
    // ends pass this step (front ones first).
    std::vector<SignalInput> alsn_ahead;
    std::vector<SignalInput> alsn_behind;
    std::vector<SignalInput> alsn_passing;
    // The next speed post ahead (train +120 / +116): its limit, m/s, and
    // the distance to it, m; -1 when there is none.
    float next_limit = -1.0f;
    float next_limit_distance = -1.0f;
    std::vector<SignalUnitSpec> signal_units;  // units not sent before
    std::vector<SoundPairState> sound_pairs;
    std::vector<TrackItemSpec> items;          // items not sent before
    std::vector<TrackItemEntry> track_items;   // the player's train's items (RTS's 0x4808DC list)
    float items_range{};                       // how far ahead and behind the list reaches, m
};

struct EngineOutput {
    float power{}, force{}, brake_force{};
    float independent_brake{}, train_pipe_rate{}, auxiliary_rate{}, main_reservoir_rate{};
    float charging_rate{}, handbrake_percent{}, dynamic_brake_percent{}, ep_value{}, equalising{};
    std::uint16_t systems{}, alsn_on{}, sanding{};
    std::uint32_t engine_flags{};
    std::int32_t reverse{};
    std::uint32_t wheelslip{}, throttle{};
    std::uint8_t panto{}, main_switch{}; // electric
    float power_consuming{};
    std::uint8_t diesel_on{};            // diesel
    float fuel_consuming{}, rpm{};
};

struct GlobalMessage {
    std::uint16_t action{}, id{};
    std::u16string text;         // GM_MSG_POST
};

struct DoorState {
    std::uint32_t state{}, set_state{};
};

// What an AuxLibrary posted to its own SMS (AuxLibrarySound) this step.
struct AuxSoundOutput {
    std::vector<std::uint16_t> triggers; // discrete triggers, in order
    std::array<float, 8> vars{};         // its SMSObject Var1..Var8
};

// A diesel's ExhaustContr (RTSGetLocoObject ot_exhaust): what its DLL says
// the smoke of exhaust n is (rts1.exe 0x50D3B4 takes it instead of the .eng).
struct ExhaustOutput {
    float rate{}, magnitude{}, dispersion{}, mass{};
    std::uint32_t colour{};
};

struct CarOutput {
    bool has_engine = false;
    EngineOutput engine;
    std::uint8_t panto_raised{}, coupling{};
    std::uint32_t loco_flags{};
    std::vector<std::uint16_t> sound_ext, sound_cab; // posted SMS triggers, in order
    std::vector<std::uint32_t> light_flags;          // LightObj::Flags
    std::vector<GlobalMessage> messages;
    std::vector<SwitchState> switches;
    std::vector<DisplayState> displays;
    std::vector<std::uint32_t> internal_lights;      // InternalLight::State
    std::vector<std::uint16_t> tex_changes;          // TexChange::SetState
    // SMSObject Var1..Var8 the DLL set for the sound scripts (exterior, cab).
    std::array<float, 8> sms_ext{}, sms_cab{};
    // Cabin::Signal.Aspect[0] after the step: the aspect the cab's
    // locomotive signal lights show (RTS 0x525DB0); 8 when none.
    std::uint32_t cab_aspect = 8;
    // Per AuxLibrary of the engine, in .eng order (empty when it has no SMS).
    std::vector<AuxSoundOutput> aux_sound;
    // The cab's SCREEN displays.
    std::vector<ScreenOutput> screens;
    // The car's exterior doors (ActDoor State, SetState), in .sd order.
    std::vector<DoorState> doors;
    // Diesels with a DLL: the exhausts' smoke as the DLL sets it.
    std::vector<ExhaustOutput> exhausts;
};

struct StepReply {
    std::vector<CarOutput> cars;
    std::int32_t set_direction{}; // RTSSetIntegerG(6): -1 / 1 requested, 0 none
    float items_wanted{};         // the farthest RTSGetTrackItems asked for this step, m
};

struct CanSwitchRequest {
    std::uint32_t car{}, id{}, set_state{};
};

// The player drags a switch (CONTROLLERX / CONTROLLERY with SwitchPrecision,
// or PRECISE; RTS 0x437E64, 0x527190). `delta`: how far its frame moves, in
// states (the game turns the cursor's move into it with SwitchPrecision and
// the node's frames). `release`: let go: a controller takes the nearest
// state, a PRECISE switch stays where it is.
struct DragSwitchRequest {
    std::uint32_t car{}, id{};
    float delta{};
    std::uint32_t release{};
};

// --- serialisation ----------------------------------------------------------

inline void write(Writer& w, const Parameter& p) { w.text(p.name); w.put(p.value); w.text(p.text); }
inline void read(Reader& r, Parameter& p) { p.name = r.text(); p.value = r.get<float>(); p.text = r.text(); }

template <class T>
void write_list(Writer& w, const std::vector<T>& list) {
    w.put(static_cast<std::uint32_t>(list.size()));
    for (const auto& item : list) write(w, item);
}
template <class T>
void read_list(Reader& r, std::vector<T>& list, std::uint32_t limit = 100000) {
    const auto n = r.get<std::uint32_t>();
    if (n > limit) return;
    list.resize(n);
    for (auto& item : list) {
        read(r, item);
        if (r.failed()) return;
    }
}
template <class T>
void write_pod_list(Writer& w, const std::vector<T>& list) {
    w.put(static_cast<std::uint32_t>(list.size()));
    w.bytes(list.data(), static_cast<std::uint32_t>(list.size() * sizeof(T)));
}
template <class T>
void read_pod_list(Reader& r, std::vector<T>& list, std::uint32_t limit = 100000) {
    const auto n = r.get<std::uint32_t>();
    if (n > limit) return;
    list.resize(n);
    r.bytes(list.data(), static_cast<std::uint32_t>(n * sizeof(T)));
}

inline void write(Writer& w, const AuxCallSpec& c) { w.put(c.event); w.put(c.order); }
inline void read(Reader& r, AuxCallSpec& c) { c.event = r.get<std::uint16_t>(); c.order = r.get<std::int16_t>(); }

inline void write(Writer& w, const AuxLibrarySpec& a) {
    w.text(a.name); w.text(a.module); w.put(a.stack_size);
    write_list(w, a.parameters); write_list(w, a.calls); w.put(static_cast<std::uint8_t>(a.has_sound));
    w.put(a.sound_table);
}
inline void read(Reader& r, AuxLibrarySpec& a) {
    a.name = r.text(); a.module = r.text(); a.stack_size = r.get<std::uint32_t>();
    read_list(r, a.parameters); read_list(r, a.calls); a.has_sound = r.get<std::uint8_t>() != 0;
    a.sound_table = r.get<std::int32_t>();
}

inline void write(Writer& w, const SwitchSpec& s) {
    w.put(s.id); w.put(s.num_states); w.put(s.initial_state); w.put(s.flags); w.put(s.anim_time);
    w.bytes(s.keys, 4); w.text(s.name); w.put(s.fixed_state); w.put(s.precision[0]); w.put(s.precision[1]);
    w.put(s.parent);
    w.put(static_cast<std::uint32_t>(s.shortcuts.size()));
    for (const auto& set : s.shortcuts) write_pod_list(w, set);
}
inline void read(Reader& r, SwitchSpec& s) {
    s.id = r.get<std::uint32_t>(); s.num_states = r.get<std::uint32_t>(); s.initial_state = r.get<std::uint32_t>();
    s.flags = r.get<std::uint32_t>(); s.anim_time = r.get<float>(); r.bytes(s.keys, 4); s.name = r.text(); s.fixed_state = r.get<std::uint32_t>();
    s.precision[0] = r.get<float>(); s.precision[1] = r.get<float>();
    s.parent = r.get<std::int16_t>();
    const auto sets = r.get<std::uint32_t>();
    if (sets > 64) return;
    s.shortcuts.resize(sets);
    for (auto& set : s.shortcuts) read_pod_list(r, set, 1024);
}

inline void write(Writer& w, const DisplaySpec& d) {
    w.put(d.id); w.bytes(d.range, sizeof(d.range)); w.put(d.granularity); w.put(d.flags); w.put(d.num_hints); w.text(d.name);
    w.put(d.initial_state); w.put(static_cast<std::uint8_t>(d.has_screen)); w.bytes(d.corners, sizeof(d.corners));
    w.put(d.reference_screen); write_pod_list(w, d.screen);
}
inline void read(Reader& r, DisplaySpec& d) {
    d.id = r.get<std::uint32_t>(); r.bytes(d.range, sizeof(d.range)); d.granularity = r.get<float>();
    d.flags = r.get<std::uint32_t>(); d.num_hints = r.get<std::uint32_t>(); d.name = r.text();
    d.initial_state = r.get<std::uint32_t>(); d.has_screen = r.get<std::uint8_t>() != 0; r.bytes(d.corners, sizeof(d.corners));
    d.reference_screen = r.get<std::int32_t>(); read_pod_list(r, d.screen, 4096);
}

inline void write(Writer& w, const std::u16string& s) { w.text(s); }
inline void read(Reader& r, std::u16string& s) { s = r.text(); }

inline void write(Writer& w, const CabSpec& c) {
    w.put(static_cast<std::uint8_t>(c.present));
    write_list(w, c.switches); write_list(w, c.displays);
    w.put(c.internal_lights); w.put(c.tex_changes); w.put(c.doors);
    w.put(c.signal_lights); w.put(c.signal_aspects); w.put(static_cast<std::uint8_t>(c.signal_scripted));
    write_list(w, c.animations);
    w.put(c.switch_count); w.put(c.display_count);
    write_pod_list(w, c.internal_light_groups);
    write_pod_list(w, c.tex_change_images); write_pod_list(w, c.tex_change_groups);
    write_list(w, c.door_names); write_pod_list(w, c.door_groups);
}
inline void read(Reader& r, CabSpec& c) {
    c.present = r.get<std::uint8_t>() != 0;
    read_list(r, c.switches); read_list(r, c.displays);
    c.internal_lights = r.get<std::uint32_t>(); c.tex_changes = r.get<std::uint32_t>(); c.doors = r.get<std::uint32_t>();
    c.signal_lights = r.get<std::uint32_t>(); c.signal_aspects = r.get<std::uint32_t>();
    c.signal_scripted = r.get<std::uint8_t>() != 0;
    read_list(r, c.animations);
    c.switch_count = r.get<std::uint32_t>(); c.display_count = r.get<std::uint32_t>();
    read_pod_list(r, c.internal_light_groups, 4096);
    read_pod_list(r, c.tex_change_images, 4096); read_pod_list(r, c.tex_change_groups, 4096);
    read_list(r, c.door_names); read_pod_list(r, c.door_groups, 4096);
}

inline void write(Writer& w, const CarSpec& c) {
    w.put(c.kind); w.put(c.uid); w.put(c.key); w.put(c.mass); w.put(c.lights); w.put(c.doors);
    write_list(w, c.animations); write_pod_list(w, c.door_times); write_pod_list(w, c.door_groups);
    write_pod_list(w, c.light_specs); w.text(c.name);
    w.text(c.series); w.text(c.module); w.put(c.lib_param); w.put(c.stack_size);
    w.put(c.num_engines); w.put(c.num_axles);
    write_list(w, c.parameters); write_list(w, c.aux); write(w, c.cab); w.put(c.sound_ext); w.put(c.sound_cab);
}
inline void read(Reader& r, CarSpec& c) {
    c.kind = r.get<std::uint8_t>(); c.uid = r.get<std::uint32_t>(); c.key = r.get<std::uint32_t>(); c.mass = r.get<float>();
    c.lights = r.get<std::uint32_t>(); c.doors = r.get<std::uint32_t>();
    read_list(r, c.animations); read_pod_list(r, c.door_times, 4096); read_pod_list(r, c.door_groups, 4096);
    read_pod_list(r, c.light_specs, 16384); c.name = r.text();
    c.series = r.text(); c.module = r.text(); c.lib_param = r.get<std::uint32_t>(); c.stack_size = r.get<std::uint32_t>();
    c.num_engines = r.get<std::uint32_t>(); c.num_axles = r.get<std::uint32_t>();
    read_list(r, c.parameters); read_list(r, c.aux); read(r, c.cab);
    c.sound_ext = r.get<std::int32_t>(); c.sound_cab = r.get<std::int32_t>();
}

inline void write(Writer& w, const SoundTableSpec& t) {
    write_list(w, t.names); write_pod_list(w, t.codes);
    w.put(static_cast<std::uint32_t>(t.pair_on.size()));
    for (const auto& pair : t.pair_on) write_pod_list(w, pair);
}
inline void read(Reader& r, SoundTableSpec& t) {
    read_list(r, t.names, 200000); read_pod_list(r, t.codes, 200000);
    const auto pairs = r.get<std::uint32_t>();
    if (pairs > 4096) return;
    t.pair_on.resize(pairs);
    for (auto& pair : t.pair_on) read_pod_list(r, pair, 4096);
}

inline void write(Writer& w, const SetupRequest& s) {
    w.text(s.rts_root); w.put(s.train_type); write_list(w, s.cars); write_list(w, s.sound_tables);
}
inline void read(Reader& r, SetupRequest& s) {
    s.rts_root = r.text(); s.train_type = r.get<std::uint8_t>(); read_list(r, s.cars, 2000); read_list(r, s.sound_tables, 4096);
}

inline void write(Writer& w, const SignalUnitSpec& u) {
    w.put(u.id); w.bytes(u.tile, sizeof(u.tile)); w.bytes(u.position, sizeof(u.position)); w.text(u.name);
}
inline void read(Reader& r, SignalUnitSpec& u) {
    u.id = r.get<std::int32_t>(); r.bytes(u.tile, sizeof(u.tile)); r.bytes(u.position, sizeof(u.position)); u.name = r.text();
}

inline void write(Writer& w, const TrackItemSpec& t) {
    w.put(t.id); w.put(t.type); w.put(t.node_offset); w.put(t.data_flags);
    w.bytes(t.pdata, sizeof(t.pdata)); w.bytes(t.ptile, sizeof(t.ptile));
    w.bytes(t.rdata, sizeof(t.rdata)); w.bytes(t.rtile, sizeof(t.rtile));
    w.put(t.flags); w.put(t.link); w.put(t.section); w.put(t.value); w.put(t.off); w.put(t.km); w.put(t.number);
    w.put(t.min_wait); w.put(t.passengers); w.text(t.name); w.text(t.station);
}
inline void read(Reader& r, TrackItemSpec& t) {
    t.id = r.get<std::uint32_t>(); t.type = r.get<std::uint32_t>(); t.node_offset = r.get<float>(); t.data_flags = r.get<std::uint32_t>();
    r.bytes(t.pdata, sizeof(t.pdata)); r.bytes(t.ptile, sizeof(t.ptile));
    r.bytes(t.rdata, sizeof(t.rdata)); r.bytes(t.rtile, sizeof(t.rtile));
    t.flags = r.get<std::uint32_t>(); t.link = r.get<std::uint32_t>(); t.section = r.get<std::uint32_t>();
    t.value = r.get<float>(); t.off = r.get<float>(); t.km = r.get<float>(); t.number = r.get<std::int32_t>();
    t.min_wait = r.get<std::uint32_t>(); t.passengers = r.get<std::uint32_t>(); t.name = r.text(); t.station = r.text();
}

inline void write(Writer& w, const ModuleReport& m) { w.text(m.path); w.put(m.status); w.bytes(m.version, sizeof(m.version)); }
inline void read(Reader& r, ModuleReport& m) { m.path = r.text(); m.status = r.get<std::int32_t>(); r.bytes(m.version, sizeof(m.version)); }

inline void write(Writer& w, const StepRequest& s) {
    w.put(s.day_time); w.put(s.dt); w.put(s.air_temperature); w.put(s.night); w.put(s.train_direction);
    write_pod_list(w, s.cars); w.put(s.route); w.put(s.signal); w.put(s.event);
    write_pod_list(w, s.signals_ahead);
    write_pod_list(w, s.alsn_ahead);
    write_pod_list(w, s.alsn_behind);
    write_pod_list(w, s.alsn_passing);
    w.put(s.next_limit); w.put(s.next_limit_distance);
    write_list(w, s.signal_units); write_pod_list(w, s.sound_pairs);
    write_list(w, s.items); write_pod_list(w, s.track_items); w.put(s.items_range);
}
inline void read(Reader& r, StepRequest& s) {
    s.day_time = r.get<double>(); s.dt = r.get<float>(); s.air_temperature = r.get<float>();
    s.night = r.get<std::uint8_t>(); s.train_direction = r.get<std::uint8_t>();
    read_pod_list(r, s.cars, 2000); s.route = r.get<SpeedLimitInput>(); s.signal = r.get<SpeedLimitInput>();
    s.event = r.get<SpeedLimitInput>(); read_pod_list(r, s.signals_ahead, 64);
    read_pod_list(r, s.alsn_ahead, 64); read_pod_list(r, s.alsn_behind, 64); read_pod_list(r, s.alsn_passing, 64);
    s.next_limit = r.get<float>(); s.next_limit_distance = r.get<float>();
    read_list(r, s.signal_units, 100000); read_pod_list(r, s.sound_pairs, 4096);
    read_list(r, s.items, 200000); read_pod_list(r, s.track_items, 200000); s.items_range = r.get<float>();
}

inline void write(Writer& w, const ReconsistCar& c) { w.put(c.from); write(w, c.spec); }
inline void read(Reader& r, ReconsistCar& c) { c.from = r.get<std::int32_t>(); read(r, c.spec); }

inline void write(Writer& w, const ReconsistTrain& t) { w.put(t.type); write_list(w, t.cars); }
inline void read(Reader& r, ReconsistTrain& t) { t.type = r.get<std::uint8_t>(); read_list(r, t.cars, 2000); }

inline void write(Writer& w, const ReconsistRequest& q) {
    write_list(w, q.sound_tables); write_list(w, q.trains); w.put(static_cast<std::uint8_t>(q.running)); write_pod_list(w, q.inputs);
}
inline void read(Reader& r, ReconsistRequest& q) {
    read_list(r, q.sound_tables, 4096); read_list(r, q.trains, 256); q.running = r.get<std::uint8_t>() != 0;
    read_pod_list(r, q.inputs, 2000);
}

inline void write(Writer& w, const EnterRequest& e) {
    w.put(e.car); w.put(static_cast<std::uint8_t>(e.running)); write_pod_list(w, e.cars); w.put(e.state);
}
inline void read(Reader& r, EnterRequest& e) {
    e.car = r.get<std::uint32_t>(); e.running = r.get<std::uint8_t>() != 0; read_pod_list(r, e.cars, 2000);
    e.state = r.get<std::int32_t>();
}

inline void write(Writer& w, const GlobalMessage& m) { w.put(m.action); w.put(m.id); w.text(m.text); }
inline void read(Reader& r, GlobalMessage& m) { m.action = r.get<std::uint16_t>(); m.id = r.get<std::uint16_t>(); m.text = r.text(); }

inline void write(Writer& w, const ScreenOutput& s) { w.put(s.display); write_pod_list(w, s.objects); write_list(w, s.texts); }
inline void read(Reader& r, ScreenOutput& s) {
    s.display = r.get<std::uint32_t>(); read_pod_list(r, s.objects, 4096); read_list(r, s.texts, 4096);
}

inline void write(Writer& w, const AuxSoundOutput& a) { write_pod_list(w, a.triggers); w.put(a.vars); }
inline void read(Reader& r, AuxSoundOutput& a) { read_pod_list(r, a.triggers); a.vars = r.get<std::array<float, 8>>(); }

inline void write(Writer& w, const CarOutput& c) {
    w.put(static_cast<std::uint8_t>(c.has_engine)); w.put(c.engine); w.put(c.panto_raised); w.put(c.coupling);
    w.put(c.loco_flags); write_pod_list(w, c.sound_ext); write_pod_list(w, c.sound_cab);
    write_pod_list(w, c.light_flags); write_list(w, c.messages); write_pod_list(w, c.switches);
    write_pod_list(w, c.displays); write_pod_list(w, c.internal_lights); write_pod_list(w, c.tex_changes);
    w.put(c.sms_ext); w.put(c.sms_cab); w.put(c.cab_aspect);
    write_list(w, c.aux_sound);
    write_list(w, c.screens);
    write_pod_list(w, c.doors);
    write_pod_list(w, c.exhausts);
}
inline void read(Reader& r, CarOutput& c) {
    c.has_engine = r.get<std::uint8_t>() != 0; c.engine = r.get<EngineOutput>();
    c.panto_raised = r.get<std::uint8_t>(); c.coupling = r.get<std::uint8_t>(); c.loco_flags = r.get<std::uint32_t>();
    read_pod_list(r, c.sound_ext); read_pod_list(r, c.sound_cab); read_pod_list(r, c.light_flags);
    read_list(r, c.messages); read_pod_list(r, c.switches); read_pod_list(r, c.displays);
    read_pod_list(r, c.internal_lights); read_pod_list(r, c.tex_changes);
    c.sms_ext = r.get<std::array<float, 8>>(); c.sms_cab = r.get<std::array<float, 8>>();
    c.cab_aspect = r.get<std::uint32_t>();
    read_list(r, c.aux_sound);
    read_list(r, c.screens, 256);
    read_pod_list(r, c.doors, 4096);
    read_pod_list(r, c.exhausts, 16);
}

inline void write(Writer& w, const StepReply& s) { write_list(w, s.cars); w.put(s.set_direction); w.put(s.items_wanted); }
inline void read(Reader& r, StepReply& s) {
    read_list(r, s.cars, 2000); s.set_direction = r.get<std::int32_t>(); s.items_wanted = r.get<float>();
}

} // namespace openrts::dllhost
