#pragma once

// The interior of an RTS cab: the ESD_Interior block of the cab shape's
// .sd file (Switches, Displays, LightSources, TextureChanges, Animations,
// LocomotiveSignalization). One description serves the 3-D cab, the
// keyboard and both kinds of locomotive DLL (docs/train-dll.md, "One cab for
// both kinds of DLL"). Strings are UTF-8 (ANSI files are read as cp1251).

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <openrts/core/vfs.hpp>

namespace openrts::formats {

// ActSwitch::Flags bits as RTS sets them from the .sd keywords.
inline constexpr std::uint32_t cab_controller_x = 0x1;
inline constexpr std::uint32_t cab_key = 0x2;
inline constexpr std::uint32_t cab_non_fixed = 0x4;
inline constexpr std::uint32_t cab_controller_y = 0x20;
inline constexpr std::uint32_t cab_precise = 0x40;
inline constexpr std::uint32_t cab_overscale = 0x80;
inline constexpr std::uint32_t cab_display_arrow = 0x100;
inline constexpr std::uint32_t cab_display_lamp = 0x200;
inline constexpr std::uint32_t cab_display_display = 0x400;
inline constexpr std::uint32_t cab_display_screen = 0x800;
inline constexpr std::uint32_t cab_loop = 0x10000;
inline constexpr std::uint32_t cab_disabled = 0x20000;
inline constexpr std::uint32_t cab_check_distance = 0x40000; // CheckDistanceOnPress / DisplaySequence
inline constexpr std::uint32_t cab_stretch_uv = 0x80000;

struct CabSwitch {
    std::uint32_t id{};
    std::string name;
    std::string hint;
    std::vector<std::string> state_hints;
    std::array<float, 3> position{};
    float radius{};
    std::uint32_t num_states = 2;
    std::uint32_t initial_state{};
    std::uint32_t fixed_state{};   // FixedState: where a KEY switch takes its key in and out
    std::uint32_t flags{};
    std::vector<std::string> keys; // AssignKey: "A", "SHIFT+X", "'" ...
    float anim_time{};
    // SwitchPrecision ( PrecX PrecY ): frames of the node's animation a pixel
    // of the cursor's move drags (RTS 0x437E64); both 0: not dragged smoothly.
    std::array<float, 2> precision{};
    // ParentObject ( id ): the switch it follows (-1: none). RTS's cab update
    // (0x52586C) gives it the parent's SetState: the EP1M's blind is twelve
    // pieces, shtora8_2.. following shtora8_1.
    std::int32_t parent = -1;
};

enum class CabDisplayType : std::uint8_t { lamp, arrow, display, screen };

// A sub-object of a SCREEN display (RTS CloseUpSubObject, loader 0x519B8C;
// docs/reverse/cab-screens.md). Type numbers are ts.h's CT_*.
enum class ScreenObjectType : std::uint8_t { frame = 0, arrow = 1, link = 2, label = 3, bar = 4, grid = 5, sector = 6 };

// Sub-object flags besides the cab_* keywords RTS keeps (it drops 0x20..0x80
// there and uses them for what the block has).
inline constexpr std::uint32_t screen_controller_y = 0x2; // CONTROLLERY
inline constexpr std::uint32_t screen_has_area = 0x20;
inline constexpr std::uint32_t screen_has_activation = 0x40;
inline constexpr std::uint32_t screen_has_uv = 0x80;
inline constexpr std::uint32_t screen_has_colour = 0x100; // SubColour
inline constexpr std::uint32_t screen_precise = 0x200;    // PRECISE: the frame follows Value, blended

// AnimationFrames ( n Frame ( Area UV Activation TexID ) ... ): what state n
// shows (RTS 0x51B6CC). `has`: 1 Area, 2 Activation, 4 UV, 8 TexID.
struct ScreenFrame {
    std::uint32_t has{};
    std::uint32_t texture{};
    std::array<float, 4> area{}, uv{}, activation{};
};

struct ScreenObject {
    ScreenObjectType type = ScreenObjectType::frame;
    std::string hint;
    std::uint32_t flags{};
    std::array<float, 4> area{}, uv{}, activation{}; // x0 y0 x1 y1 of the screen / texture, 0..1
    std::vector<std::string> textures; // relative to the .sd's folder
    std::uint32_t shader{};            // NamedShader: 1 Tex .. 7 AddATexDiff (RTS 0x45B1E0), 0 none
    std::int32_t initial_state{};      // InitialState: < 0 hidden
    std::uint32_t anim_flags{};
    float anim_time{};
    float fade{};                      // SubFade ( fade blink ); without it blink is -1
    float blink = -1.0f;
    std::uint32_t colour{};            // SubColour, AARRGGBB
    std::int32_t parent = -1;          // ParentSub: index in the screen's objects
    // ARROW: Center; ARROW / BAR / SECTOR: ValuesRange ( min max first second ).
    std::array<float, 2> center{};
    std::array<float, 4> range{};
    std::uint32_t bar_style{};         // 1 H_LEFT, 4 H_RIGHT, 2 V_UP, 3 V_DOWN
    std::int32_t link = -1;            // LINK: LinkTo
    // LABEL ( screen units, texture units ). The label works only when its
    // character fits the texture (RTS clears char_width[1] otherwise).
    std::array<float, 2> char_width{}, char_height{}, char_spacing{}, line_spacing{};
    std::uint16_t h_align{}, v_align{}; // 0 LEFT, 1 CENTER, 2 RIGHT, 3 JUSTIFIED, 4 BOTTOM, 5 TOP
    std::uint16_t chars_per_row{};      // characters per texture row
    std::u32string alphabet;
    std::uint16_t max_length{};
    // GRID: GridNumRows ( shown, on the texture ), GridNumCols ( shown, on the texture ).
    std::array<std::uint32_t, 2> grid_rows{}, grid_columns{};
    // SECTOR: SectorShape (0 ROUND, 1 RECT), SectorFillType (0, 1 STARTPOS, 2 ENDPOS).
    std::uint32_t sector_shape{}, sector_fill{};
    float sector_granularity{};
    std::vector<ScreenFrame> frames;
    std::vector<float> anim_time_scale;
};

// DisplayScreen ( TopLeft TopRight BottomRight SubObjects ( ... ) ): a flat
// quad in the cab shape's coordinates (RTS 0x529F00).
struct CabScreen {
    bool present = false;
    std::array<float, 3> top_left{}, top_right{}, bottom_right{};
    std::int32_t reference_screen = -1; // ReferenceScreen ( display ID ): shows that screen's objects
    std::vector<ScreenObject> objects;
};

struct CabDisplay {
    std::uint32_t id{};
    std::string name;
    std::string hint;
    std::array<float, 3> position{};
    float radius{};
    CabDisplayType type = CabDisplayType::lamp;
    std::uint32_t flags{};          // keyword bits | the display type bit
    std::array<float, 4> range{};   // ValuesRange
    float granularity{};
    std::uint32_t initial_state{};
    float anim_speed{};
    std::uint32_t screen_subobjects{}; // DisplayScreen SubObjects
    CabScreen screen;
    // DisplayFade ( fade blink ) (RTS 0x51CD88, display +76 / +80): a lamp,
    // display or screen comes on and goes off over `fade` s and, with a
    // blink, is shown `blink` s and hidden `blink` s (0x525DB0).
    float fade{};
    float blink{};
    // StateNodes ( n "node" ... ) of a DISPLAY (+52 / +68): node j shows when
    // bit j of the State is set, or with DISPLAY_SEQUENCE (flag 0x40000)
    // when the State is j + 1.
    std::vector<std::string> state_nodes;
};

// LightSources ( LightSource ( ... ) ): a lamp of the cab or salon (RTS
// 0x51C364): it shines from Position towards PointsTo over Radius (x, z
// half sizes); GlobalID groups the lamps a DLL switches together
// (GM_LIGHT_ON / OFF). Flags LIGHT_CONE / LIGHT_SPHERE, else a box.
struct CabLight {
    std::string name;
    std::array<float, 3> position{}, points_to{};
    std::array<float, 2> radius{};
    float spot_exponent{};
    std::uint32_t colour{};
    std::array<float, 2> fade{};
    std::string flags;
    std::uint32_t global_id{};
    float source_radius{};
    std::array<float, 3> attenuation{2.0f, 0.6f, 0.4f}; // kind (2 linear), start share, length share
    // AutoWhenWorldLit (InternalLight +88): a NIGHT_AUTO lamp burns while the
    // world is at most this lit (0: at night).
    float auto_when_world_lit{};
};

// TextureChanges Change ( TexID "file" Change GlobalID ) (RTS 0x51CD88):
// the cab shape's image TexID is drawn as `file` while the change is on.
struct CabTextureChange {
    std::uint32_t texture{};
    std::string file;
    std::array<std::uint32_t, 2> values{}; // Change, GlobalID
};

struct CabAnimation {
    std::string name;
    float anim_time{};
    std::uint32_t key_frames{};
    std::string flags;
    std::vector<std::string> nodes;
};

struct CabSignalization {
    bool present = false;
    std::uint32_t block_check_ahead{};
    std::vector<std::string> lights;
    std::vector<std::vector<std::uint32_t>> aspects;
    bool scripted = false; // SignalizationScripted: the DLL's ALSN drives the lights
};

// InteriorChairs Chair, in cab shape coordinates: Position (where Enter
// finds it, within Radius), Pos1 where the driver sits (RTS: the eye is
// engineer.Height1 above it, 0x43778C), Pos2 where Enter stands him up,
// Pos3 where Ctrl+Enter does.
struct CabChair {
    std::array<float, 3> position{}, seated{}, standing{}, window{};
    float radius = 1.5f;
    std::string name; // Skip ( "..." ): the seat's caption
    std::string head_out_window;
};

// InteriorGeometryInfo: the rooms of a cab shape and how the engineer walks
// between them. Rectangles are (x1, z1)..(x2, z2) in cab shape coordinates.
struct CabArea {
    std::string name;
    float x1{}, z1{}, x2{}, z2{};
    std::vector<std::string> nodes; // ChildNodes: drawn while the engineer is in it
};
struct CabPlane {
    std::string name;
    std::array<float, 3> a{}, b{}; // InteriorPlane ( [name] x1 y1 z1 x2 y2 z2 )
};
// A point of a Trajectory ( n  x y z kind  ... ): where the engineer goes, in
// the frame of what it belongs to (a cab, a car's shape), and the kind of
// move to it (RTS 0x435654's speed profiles: docs/reverse/walking.md).
struct TrajectoryPoint {
    std::array<float, 3> at{};
    std::int32_t kind{};
};
struct CabDoor {
    std::string name;              // a node of the cab shape (its animation opens it)
    std::string from, to;          // Connects ( a b ): areas, EXTERIOR or WAGON_BACK / WAGON_FRONT
    std::array<float, 3> position{};
    float radius = 1.5f;
    CabPlane plane;                // the floor through the doorway
    // Trajectory: the way through it (Ctrl+Enter); for a door out onto the
    // car's outside (EXT_WAG), RTS runs the car's ExteriorDoor of the same
    // name instead (0x431270).
    std::vector<TrajectoryPoint> trajectory;
    float anim_time = 1.0f;
    bool has_position = false;
    // GlobalID: 1 the left doors, 2 the right ones: opened and closed with the
    // train's doors (GM_DOOR_OPEN / CLOSE, RTS 0x424488); 0 none.
    std::uint32_t global_id{};
    bool can_be_opened = true; // CanBeOpened: the player may open it by hand
    bool opened = false;       // Opened: open from the start (RTS: the keyword's presence)
    // ParentObject ( n ): the door (its index in InteriorDoors) whose state
    // it always has (RTSFileReference: "its state always equals the
    // parent's"): a leaf or a handle that opens with the door, not by hand.
    // -1: none. The EP2K's DOOR_CR1 / CR2 follow the cab door DOOR_C.
    std::int32_t parent = -1;
    // SoundProof ( closed open ): the share of a sound that passes the door
    // shut and open; without it everything passes (RTS 0x51CD88: 1, 1).
    float sound_closed = 1.0f, sound_open = 1.0f;
    // SMSTriggers ( open shut ): the codes for the car's sounds as it starts
    // to open and to shut (RTS +0x74 / +0x76, 0x438708); 0 none.
    std::array<std::uint16_t, 2> sms_triggers{};
};
// InteriorSeats ( n InteriorSeat ( Position Radius Pos1 Pos2 Area
// HeadOutWindow ) ): a salon's passenger seats (RTS 0x515434, object type
// 11). Enter by one (its Position within Radius, in view) takes him along
// the aisle to Pos1 and down at Pos2, the eye Height1 above it; Enter again
// back up at Pos1 (0x4372B8). Its window opens and he leans out of it as
// from a chair (Num0 / Num1).
struct CabSeat {
    std::array<float, 3> position{}, aisle{}, seated{};
    float radius = 3.0f;
    std::array<float, 4> area{}; // x1 z1 x2 z2: the seat's floor (RTS marks it taken)
    std::string head_out_window;
};
struct CabWindow {
    std::string name;
    bool can_open = false;
    bool opened = false; // Opened: open from the start
    std::array<float, 3> head_out{};
    std::array<float, 3> position{};
    bool has_position = false;
    float radius{};      // Enter by it (Position within Radius, in view) opens or shuts it (RTS type 6)
    float anim_time = 1.0f;
    // SoundProof ( closed open ) towards the outside of the room the window's
    // Position is in; without it nothing passes (RTS 0x51CD88: 0, 0).
    float sound_closed = 0.0f, sound_open = 0.0f;
    // SMSTriggers ( open shut ), as a door's (RTS +0x60 / +0x62).
    std::array<std::uint16_t, 2> sms_triggers{};
};

// ESD_Extended_Animation AnimationNode ( Name ( "x" ) Shortcut ( n f1 .. fn )
// ... ): frames of the node's animation it jumps between instead of running
// through the frames in between (RTS: parsed 0x4BB7BC, sorted; used
// 0x4B30DC with 0x4B3778 / 0x4B3884). The ChS7's controller goes from 0 to
// +1 past its field weakening sector so.
struct CabAnimationNode {
    std::string name;
    std::vector<std::vector<std::uint16_t>> shortcuts; // each ascending
};

struct CabDescription {
    std::vector<CabSwitch> switches;   // by position in the file; ids may have gaps
    std::vector<CabAnimationNode> animation_nodes;
    // The counts the file declares (Switches ( n .. ), Displays ( n .. )): RTS
    // makes that many (0x51CD88), so an id up to n - 1 exists without a block.
    std::uint32_t switch_count{};
    std::uint32_t display_count{};
    std::vector<CabDisplay> displays;
    std::vector<CabLight> lights;
    std::vector<CabTextureChange> texture_changes;
    std::vector<CabAnimation> animations;
    CabSignalization signalization;
    std::uint32_t interior_doors{};
    std::vector<CabChair> chairs;
    std::vector<CabSeat> seats; // a salon's (InteriorSeats)
    std::vector<CabArea> areas;
    std::vector<CabPlane> planes;
    std::vector<CabDoor> doors;
    std::vector<CabWindow> windows;
    std::vector<std::string> exterior_nodes;
    std::string error;
    [[nodiscard]] bool ok() const noexcept { return error.empty(); }
    [[nodiscard]] const CabSwitch* find_switch(std::uint32_t id) const;
    // The switch that one follows in the end (ParentObject over ParentObject;
    // itself when it follows none).
    [[nodiscard]] std::uint32_t root_switch(std::uint32_t id) const;
    [[nodiscard]] const CabSwitch* find_switch(std::string_view name) const;
};

// Parses a cab .sd file (bytes as read).
[[nodiscard]] CabDescription parse_cab_description(std::string_view bytes);

// What a car's own shape .sd adds outside (ESD_Exterior_Details): free
// animations a DLL finds by name (FindAnim), doors, and what the engineer
// walks on outside the car (RTS 0x4F0BC8; docs/reverse/walking.md).
// ESD_Exterior_Details ExteriorDoor: a door of the car's shape (its node opens
// by its animation). CanBeOpened ( 0 ): only a DLL opens it (passenger doors,
// GlobalID groups them by side).
struct ExteriorDoor {
    std::string name;
    // Connects ( a b ), lower case: ext_wag (the car's outside), interior
    // (its cab), exterior (the ground), wagon_front / wagon_back.
    std::string from, to;
    std::array<float, 3> position{};
    bool has_position = false;
    float radius = 2.0f;
    CabPlane plane;          // InteriorPlane: the doorway's floor
    bool has_plane = false;
    std::vector<TrajectoryPoint> trajectory; // the way through it
    float anim_time = 1.0f;
    std::uint32_t global_id{};
    bool can_be_opened = true;
    std::int32_t parent = -1; // ParentObject: the door whose state it has
};
// ExteriorLadder: steps or a ladder, run as a passage (Ctrl+Enter within
// Radius of Position): its Trajectory up or down, from the end nearer him.
struct ExteriorLadder {
    std::string from, to; // Connects, as a door's (none: he stays where it ends)
    std::array<float, 3> position{};
    float radius{};
    std::vector<TrajectoryPoint> trajectory;
};
// ExteriorPlane ( x1 y1 z1 x2 y2 z2 [EXIT] [WAGON_FRONT] [WAGON_BACK] ): a
// floor outside the car (walkways, steps); off its edge with EXIT he gets
// down to the ground, with WAGON_FRONT / WAGON_BACK over to that car.
struct ExteriorPlane {
    std::array<float, 3> a{}, b{};
    bool exit = false, front = false, back = false;
};
// ExteriorLedge ( Trajectory ( .. ) ): segments he walks along (a roof's
// edge), within 0.3 m of them.
struct ExteriorLedge {
    std::vector<TrajectoryPoint> points;
};

// A car's object shown by its state (an RTS .sd's ESD_Exterior_Details
// WagonObjects): DragA / DragB, the wheel chocks; EndValveA, BrakePipeA,
// ChargingPipeA and their B: the end cocks and hoses.
struct WagonObject {
    std::string kind;               // lower case: "draga", "dragb", "endvalvea", ...
    std::vector<std::string> nodes; // ChildNodes
    std::uint32_t num_states = 2;
    bool first_state_invisible = false; // state 0: the nodes are not drawn
    std::vector<float> state_key_frames; // StateKeyFrames, after its count
    float anim_time{};
};

struct ExteriorDetails {
    std::vector<CabAnimation> animations;
    std::vector<std::string> doors;
    std::vector<ExteriorDoor> door_details; // in the same order
    std::vector<WagonObject> wagon_objects;
    std::vector<ExteriorLadder> ladders;
    std::vector<ExteriorPlane> planes;
    std::vector<ExteriorLedge> ledges;
    // Anything the engineer can walk on or climb outside.
    [[nodiscard]] bool walkable() const noexcept { return !ladders.empty() || !planes.empty() || !ledges.empty(); }
};
[[nodiscard]] ExteriorDetails parse_exterior_details(std::string_view bytes);

// An RTS .repaint file: another shape with replaced textures
// ( Repaint ( Shape ( "x.s" ) ShapeDescription ( "x.sd" ) TextureReplace ( "a.ace" "b.ace" ) ) ).
struct Repaint {
    std::string shape;
    std::string description;
    std::vector<std::pair<std::string, std::string>> textures;
    [[nodiscard]] bool ok() const noexcept { return !shape.empty(); }
};
[[nodiscard]] Repaint parse_repaint(std::string_view bytes);
// The .sd of a shape path: <shape>.sd, or for an RTS .repaint the
// ShapeDescription (or its Shape's .sd) it names, relative to the repaint.
[[nodiscard]] core::VirtualPath shape_description(const core::Vfs& vfs, const core::VirtualPath& shape);

// The switch and display flag bits of a Flags ( ... ) keyword list.
[[nodiscard]] std::uint32_t cab_flags(std::string_view keywords);

} // namespace openrts::formats
