#pragma once

#include <cstdint>
#include <string_view>

// RTS network package ids with openRTS's names for them
// (docs/reverse/multiplayer-protocol.md, "Package names"; RTS has none).
namespace openrts::net {

enum class Package : std::uint16_t {
    // Session and service.
    login = 0x4600,
    console_login = 0x4500,
    console_login_reply = 0x4501,
    console_wait_admin = 0x4502,
    console_loading = 0x4503,
    snapshot = 0x4608,
    nick_busy = 0x4701,
    not_admitted = 0x4702,
    snapshot_again = 0x4703,
    not_a_login = 0x4704,
    bad_version = 0x4705,
    flooding = 0x4706,
    drop_receive = 0x47FD,
    drop_short = 0x47FE,
    drop_bad = 0x47FF,
    message = 0x4611,
    server_down = 0x4612,
    pause = 0x4616,
    server_message = 0x4643,
    host_mark = 0x4630,
    user = 0x462E,
    player_pos = 0x462D,
    node_hello = 0x1700,
    node_text = 0x1701,
    // World.
    time = 0x460D,
    junctions = 0x4609,
    junction = 0x460A,
    junction_request = 0x4642,
    signals = 0x460B,
    signal_head = 0x460C,
    signal_control = 0x4618,
    weather_area = 0x4634,
    weather_values = 0x4635,
    object_state = 0x461D,
    object_sound = 0x463F,
    track_object_value = 0x464B,
    track_object_byte = 0x464C,
    track_object_word = 0x464D,
    track_object_list = 0x464E,
    // Electrification.
    piece_supply = 0x4645,
    substation = 0x4646,
    section_source = 0x4647,
    phase_voltages = 0x4648,
    substation_load = 0x4649,
    // Trains.
    train_add = 0x4607,
    train_cars = 0x460E, // inside train_add; uncouple on its own
    train_car = 0x460F,  // inside train_cars; couple on its own
    train_pos = 0x4603,
    car_pos = 0x4604,
    train_pos_forced = 0x462F,
    train_info = 0x4605,
    train_stands = 0x4606,
    train_remove = 0x4602,
    train_assign = 0x4601,
    helper_assign = 0x461E,
    train_id = 0x4610,
    train_brakes = 0x4613,
    train_speed = 0x4508,
    route_left = 0x4619,
    train_names = 0x461A,
    train_wake = 0x4641,
    derail = 0x4636,
    derailed_car = 0x4638,
    rerail = 0x4637,
    car_cargo = 0x461C,
    uncouple = 0x460E,
    couple = 0x460F,
    joint_action = 0x461B,
    car_action = 0x4640,
    seat_player = 0x4633,
    // Other players' trains.
    car_light = 0x4617,
    car_state = 0x463C,
    sound_trigger = 0x4639,
    sound_values = 0x463D,
    sound_state = 0x463A,
    sound_stream = 0x463B,
    state_request = 0x4623,
    // Helpers.
    helper_link = 0x461F,
    train_stream = 0x4620,
    car_stream = 0x4621,
    engine_stream = 0x4628,
    electric_stream = 0x4629,
    diesel_stream = 0x462A,
    cab_control = 0x4622,
    cab_state = 0x4624,
    dll_state = 0x4625,
    cab_controls = 0x4626,
    cab_display = 0x4631,
    aux_library_state = 0x463E,
    cab_drag = 0x4632,
    // Consoles.
    console_pause = 0x4504,
    train_go = 0x4505,
    train_reverse = 0x4506,
    train_allow_coupling = 0x4509,
    train_add_reply = 0x4507,
    event_line = 0x450A,
    route_segment = 0x450B,
    route_segment_reply = 0x450C,
    route_segment_cancel = 0x450D,
    route_segment_state = 0x450E,
    // Situation files.
    situation_user = 0x4614,
    situation_route = 0x4615,
};

[[nodiscard]] constexpr std::uint16_t id(const Package package) noexcept { return static_cast<std::uint16_t>(package); }

// The name of a package id; `parent` is the package it is nested in (0 at
// the top), which tells train_cars / train_car from uncouple / couple.
// Unknown ids give an empty view.
[[nodiscard]] std::string_view package_name(std::uint16_t id, std::uint16_t parent = 0) noexcept;

} // namespace openrts::net
