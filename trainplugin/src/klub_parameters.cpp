// MaDaDev's Brakes.dll's КЛУБ parameters as openRTS's КЛУБ takes them
// (legacy_consist.hpp, KlubParameters; docs/brakes-dll-replacement.md,
// step 3; the parameters: docs/reverse/brakes-dll.md §11).
#include <openrts/trainplugin/legacy_consist.hpp>

#include <cstdlib>
#include <string_view>

#include <openrts/core/virtual_path.hpp>

namespace openrts::trainplugin {

namespace {

using simulation::KlubKey;
using simulation::KlubPress;

// The first cab's keys: KLUB_<name>1.
constexpr std::pair<std::string_view, KlubPress> key_names[] = {
    {"klub_b0id1", {KlubKey::digit, 0}}, {"klub_b1id1", {KlubKey::digit, 1}}, {"klub_b2id1", {KlubKey::digit, 2}},
    {"klub_b3id1", {KlubKey::digit, 3}}, {"klub_b4id1", {KlubKey::digit, 4}}, {"klub_b5id1", {KlubKey::digit, 5}},
    {"klub_b6id1", {KlubKey::digit, 6}}, {"klub_b7id1", {KlubKey::digit, 7}}, {"klub_b8id1", {KlubKey::digit, 8}},
    {"klub_b9id1", {KlubKey::digit, 9}}, {"klub_bplid1", {KlubKey::plus}},    {"klub_bmnid1", {KlubKey::minus}},
    {"klub_bsbid1", {KlubKey::reset}},   {"klub_bkid1", {KlubKey::command}},  {"klub_bvvid1", {KlubKey::enter}},
    {"klub_bpid1", {KlubKey::track}},    {"klub_blid1", {KlubKey::data}},     {"klub_bvkid1", {KlubKey::vk}},
    {"klub_bmpid1", {KlubKey::mp}},      {"klub_bpsid1", {KlubKey::backlight}}, {"klub_biid1", {KlubKey::info}},
};

bool is_brakes_dll(const std::string& module) {
    const auto m = core::fold_case(module);
    return m == "brakes.dll" || (m.size() > 11 && (m.ends_with("/brakes.dll") || m.ends_with("\\brakes.dll")));
}

} // namespace

const simulation::KlubPress* KlubParameters::key(const std::int32_t id) const noexcept {
    for (const auto& [k, press] : keys)
        if (k == id) return &press;
    return nullptr;
}

KlubParameters klub_parameters(const formats::EngineDefinition& engine) {
    KlubParameters p;
    const formats::AuxLibraryDefinition* library = nullptr;
    for (const auto& a : engine.aux_libraries) {
        if (is_brakes_dll(a.module)) {
            library = &a;
            break;
        }
    }
    if (!library) return p;
    p.present = true;
    // Brakes.dll's defaults: KLUB_PLAMP and KLUB_TCHNUM 0 when not given.
    p.lamp = 0;
    p.backlight = 0;
    auto& k = p.settings;
    int power_count = 0;
    std::vector<std::pair<int, std::int32_t>> power;
    bool screen = false;
    for (std::size_t index = 0; index < library->parameters.size(); ++index) {
        const auto& parameter = library->parameters[index];
        const auto name = core::fold_case(parameter.name);
        const float value = parameter.value;
        const auto id = static_cast<std::int32_t>(value);
        if (name == "pb_setup") {
            k.setup = static_cast<std::uint32_t>(value);
            p.setup_index = static_cast<std::int32_t>(index);
        } else if (name == "pb_flip") p.flip = id;
        else if (name == "klub_dals1") p.dals = id;
        else if (name == "klub_dpom1") p.dpom = id;
        else if (name == "klub_plamp") p.lamp = id;
        else if (name == "klub_tchnum") p.backlight = id;
        else if (name.size() == 12 && name.starts_with("klub_b") && name.ends_with("pom1") && name[6] >= '1' && name[6] <= '8')
            p.lamps[static_cast<std::size_t>(name[6] - '1')] = id;
        else if (name == "alsn_delay") k.alsn_delay = value;
        else if (name == "klub_grspd") k.green_speed = value;
        else if (name == "klub_ylspd") k.yellow_speed = value;
        else if (name == "klub_bluch") k.block_length = value;
        else if (name == "br_ept") k.ept = value != 0.0f;
        else if (name == "block_mode") k.block_mode = value != 0.0f;
        else if (name == "klub_dbil1") {
            screen = id >= 0;
            p.screen = id;
        }
        else if (name == "klub_kass1") p.cassette = id;
        else if (name == "klub_pcount") power_count = id;
        else if (name.starts_with("klub_pit")) power.emplace_back(std::atoi(name.c_str() + 8), id);
        else if (name == "pb_rb1") p.rb = id;
        else if (name == "pb_rbs1") p.rbs = id;
        else if (name == "pb_epkkey1") p.epk_key = id;
        else if (name == "tskbm_sw1") p.tskbm = id;
        else if (name == "saut_swid1") p.saut = id;
        else {
            for (const auto& [key_name, press] : key_names)
                if (name == key_name && id >= 0) p.keys.emplace_back(id, press);
        }
    }
    k.has_screen = screen;
    k.cassette_given = p.cassette >= 0;
    // KLUB_PIT1 .. KLUB_PIT<KLUB_PCOUNT>, all on to power it (a missing one
    // reads as switch -1: off).
    for (int i = 1; i <= power_count; ++i) {
        std::int32_t id = -1;
        for (const auto& [n, v] : power)
            if (n == i) id = v;
        p.power.push_back(id);
    }
    return p;
}

} // namespace openrts::trainplugin
