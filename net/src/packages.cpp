#include <openrts/net/packages.hpp>

namespace openrts::net {

namespace {

// Top-level names; the two ids that mean something else nested are handled
// in package_name().
struct Named {
    std::uint16_t id;
    std::string_view name;
};

constexpr Named names[] = {
    {0x4600, "login"},
    {0x4500, "console_login"},
    {0x4501, "console_login_reply"},
    {0x4502, "console_wait_admin"},
    {0x4503, "console_loading"},
    {0x4608, "snapshot"},
    {0x4701, "nick_busy"},
    {0x4702, "not_admitted"},
    {0x4703, "snapshot_again"},
    {0x4704, "not_a_login"},
    {0x4705, "bad_version"},
    {0x4706, "flooding"},
    {0x47FD, "drop_receive"},
    {0x47FE, "drop_short"},
    {0x47FF, "drop_bad"},
    {0x4611, "message"},
    {0x4612, "server_down"},
    {0x4616, "pause"},
    {0x4643, "server_message"},
    {0x4630, "host_mark"},
    {0x462E, "user"},
    {0x462D, "player_pos"},
    {0x1700, "node_hello"},
    {0x1701, "node_text"},
    {0x460D, "time"},
    {0x4609, "junctions"},
    {0x460A, "junction"},
    {0x4642, "junction_request"},
    {0x460B, "signals"},
    {0x460C, "signal_head"},
    {0x4618, "signal_control"},
    {0x4634, "weather_area"},
    {0x4635, "weather_values"},
    {0x461D, "object_state"},
    {0x463F, "object_sound"},
    {0x464B, "track_object_value"},
    {0x464C, "track_object_byte"},
    {0x464D, "track_object_word"},
    {0x464E, "track_object_list"},
    {0x4645, "piece_supply"},
    {0x4646, "substation"},
    {0x4647, "section_source"},
    {0x4648, "phase_voltages"},
    {0x4649, "substation_load"},
    {0x4607, "train_add"},
    {0x4603, "train_pos"},
    {0x4604, "car_pos"},
    {0x462F, "train_pos_forced"},
    {0x4605, "train_info"},
    {0x4606, "train_stands"},
    {0x4602, "train_remove"},
    {0x4601, "train_assign"},
    {0x461E, "helper_assign"},
    {0x4610, "train_id"},
    {0x4613, "train_brakes"},
    {0x4508, "train_speed"},
    {0x4619, "route_left"},
    {0x461A, "train_names"},
    {0x4641, "train_wake"},
    {0x4636, "derail"},
    {0x4638, "derailed_car"},
    {0x4637, "rerail"},
    {0x461C, "car_cargo"},
    {0x460E, "uncouple"},
    {0x460F, "couple"},
    {0x461B, "joint_action"},
    {0x4640, "car_action"},
    {0x4633, "seat_player"},
    {0x4617, "car_light"},
    {0x463C, "car_state"},
    {0x4639, "sound_trigger"},
    {0x463D, "sound_values"},
    {0x463A, "sound_state"},
    {0x463B, "sound_stream"},
    {0x4623, "state_request"},
    {0x461F, "helper_link"},
    {0x4620, "train_stream"},
    {0x4621, "car_stream"},
    {0x4628, "engine_stream"},
    {0x4629, "electric_stream"},
    {0x462A, "diesel_stream"},
    {0x4622, "cab_control"},
    {0x4624, "cab_state"},
    {0x4625, "dll_state"},
    {0x4626, "cab_controls"},
    {0x4631, "cab_display"},
    {0x463E, "aux_library_state"},
    {0x4632, "cab_drag"},
    {0x4504, "console_pause"},
    {0x4505, "train_go"},
    {0x4506, "train_reverse"},
    {0x4509, "train_allow_coupling"},
    {0x4507, "train_add_reply"},
    {0x450A, "event_line"},
    {0x450B, "route_segment"},
    {0x450C, "route_segment_reply"},
    {0x450D, "route_segment_cancel"},
    {0x450E, "route_segment_state"},
    {0x4614, "situation_user"},
    {0x4615, "situation_route"},
    {0x45F0, "console_command"},
    {0x45F1, "console_reply"},
    {0x45F2, "console_state"},
    {0x45F3, "console_place"},
};

} // namespace

std::string_view package_name(const std::uint16_t id, const std::uint16_t parent) noexcept {
    if (parent == net::id(Package::train_add) && id == net::id(Package::train_cars)) return "train_cars";
    if (parent == net::id(Package::train_cars) && id == net::id(Package::train_car)) return "train_car";
    for (const auto& named : names) {
        if (named.id == id) return named.name;
    }
    return {};
}

} // namespace openrts::net
