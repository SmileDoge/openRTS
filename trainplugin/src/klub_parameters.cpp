// MaDaDev's Brakes.dll's КЛУБ parameters as openRTS's КЛУБ takes them
// (legacy_consist.hpp, KlubParameters; docs/brakes-dll-replacement.md,
// step 3; the parameters: docs/reverse/brakes-dll.md §11).
#include <openrts/trainplugin/legacy_consist.hpp>

#include <algorithm>
#include <cstdlib>
#include <string>
#include <string_view>

#include <openrts/core/virtual_path.hpp>

namespace openrts::trainplugin {

namespace {

using simulation::KlubKey;
using simulation::KlubPress;

// A cab's keys: KLUB_<name><n> (the cab n from 1).
constexpr std::pair<std::string_view, KlubPress> key_names[] = {
    {"klub_b0id", {KlubKey::digit, 0}}, {"klub_b1id", {KlubKey::digit, 1}}, {"klub_b2id", {KlubKey::digit, 2}},
    {"klub_b3id", {KlubKey::digit, 3}}, {"klub_b4id", {KlubKey::digit, 4}}, {"klub_b5id", {KlubKey::digit, 5}},
    {"klub_b6id", {KlubKey::digit, 6}}, {"klub_b7id", {KlubKey::digit, 7}}, {"klub_b8id", {KlubKey::digit, 8}},
    {"klub_b9id", {KlubKey::digit, 9}}, {"klub_bplid", {KlubKey::plus}},    {"klub_bmnid", {KlubKey::minus}},
    {"klub_bsbid", {KlubKey::reset}},   {"klub_bkid", {KlubKey::command}},  {"klub_bvvid", {KlubKey::enter}},
    {"klub_bpid", {KlubKey::track}},    {"klub_blid", {KlubKey::data}},     {"klub_bvkid", {KlubKey::vk}},
    {"klub_bmpid", {KlubKey::mp}},      {"klub_bpsid", {KlubKey::backlight}}, {"klub_biid", {KlubKey::info}},
};

// A name and its cab: "klub_dbil2" is ("klub_dbil", 2); 0 when it ends in no digit.
std::pair<std::string_view, int> cab_name(const std::string_view name) {
    std::size_t end = name.size();
    while (end > 0 && name[end - 1] >= '0' && name[end - 1] <= '9') --end;
    if (end == name.size() || end == 0) return {name, 0};
    return {name.substr(0, end), std::atoi(std::string(name.substr(end)).c_str())};
}

bool is_brakes_dll(const std::string& module) {
    const auto m = core::fold_case(module);
    return m == "brakes.dll" || (m.size() > 11 && (m.ends_with("/brakes.dll") || m.ends_with("\\brakes.dll")));
}

} // namespace

const simulation::KlubPress* KlubCabParameters::key(const std::int32_t id) const noexcept {
    for (const auto& [k, press] : keys)
        if (k == id) return &press;
    return nullptr;
}

int KlubParameters::working_cab(const std::vector<dllhost::SwitchState>& switches) const noexcept {
    if (cabs.size() == 1) return 0;
    for (std::size_t c = 0; c < cabs.size(); ++c) {
        // Brakes.dll 0x401600: a switch the cab has not reads as -1.
        const auto id = cabs[c].cab_switch;
        const std::int32_t state = id >= 0 && static_cast<std::size_t>(id) < switches.size()
                                       ? static_cast<std::int32_t>(switches[static_cast<std::size_t>(id)].state)
                                       : -1;
        if (state == working_position) return static_cast<int>(c);
    }
    return -1;
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
    int power_count = 0, cab_count = 1;
    std::vector<std::pair<int, std::int32_t>> power;
    // A cab's id by its name: the first given, as RTS finds a parameter by
    // its name (PROBABLE).
    struct CabId {
        std::string name;
        int cab;
        std::int32_t id;
    };
    std::vector<CabId> cab_ids;
    for (std::size_t index = 0; index < library->parameters.size(); ++index) {
        const auto& parameter = library->parameters[index];
        const auto name = core::fold_case(parameter.name);
        const float value = parameter.value;
        const auto id = static_cast<std::int32_t>(value);
        if (name == "pb_setup") {
            k.setup = static_cast<std::uint32_t>(value);
            p.setup_index = static_cast<std::int32_t>(index);
        } else if (name == "pb_flip") p.flip = id;
        else if (name == "klub_plamp") p.lamp = id;
        else if (name == "klub_tchnum") p.backlight = id;
        else if (name == "alsn_delay") k.alsn_delay = value;
        else if (name == "klub_grspd") k.green_speed = value;
        else if (name == "klub_ylspd") k.yellow_speed = value;
        else if (name == "klub_bluch") k.block_length = value;
        else if (name == "br_ept") k.ept = value != 0.0f;
        else if (name == "block_mode") {
            k.block_mode = value != 0.0f;
            k.block_scale_150 = value == 1.0f;
        }
        else if (name == "klub_biltype") k.screen_type = static_cast<std::uint32_t>(id);
        else if (name == "br_numcabs") cab_count = id;
        else if (name == "trans_sound") p.trans_sound = value == 1.0f;
        else if (name == "br_367oppol") p.working_position = id;
        else if (name == "klub_pcount") power_count = id;
        else if (name.starts_with("klub_pit")) power.emplace_back(std::atoi(name.c_str() + 8), id);
        else if (const auto [base, cab] = cab_name(name); cab >= 1) {
            bool seen = false;
            for (const auto& c : cab_ids) seen = seen || (c.cab == cab && c.name == base);
            if (!seen) cab_ids.push_back({std::string(base), cab, id});
        }
    }
    p.cabs.assign(static_cast<std::size_t>(std::clamp(cab_count, 1, 8)), {});
    for (const auto& [base, cab, id] : cab_ids) {
        if (static_cast<std::size_t>(cab) > p.cabs.size()) continue;
        auto& c = p.cabs[static_cast<std::size_t>(cab - 1)];
        if (base == "klub_dbil") c.screen = id;
        else if (base == "klub_dals") c.dals = id;
        else if (base == "klub_dpom") c.dpom = id;
        else if (base == "klub_kass") c.cassette = id;
        else if (base == "pb_rb") c.rb = id;
        else if (base == "pb_rbs") c.rbs = id;
        else if (base == "pb_epkkey") c.epk_key = id;
        else if (base == "tskbm_sw") c.tskbm = id;
        else if (base == "saut_swid") c.saut = id;
        else if (base == "br_367id") c.cab_switch = id;
        else if (base.size() == 10 && base.starts_with("klub_b") && base.ends_with("pom") && base[6] >= '1' && base[6] <= '8')
            c.lamps[static_cast<std::size_t>(base[6] - '1')] = id;
        else {
            for (const auto& [key_name, press] : key_names)
                if (base == key_name && id >= 0) c.keys.emplace_back(id, press);
        }
    }
    k.has_screen = p.cabs[0].screen >= 0;
    k.cassette_given = p.cabs[0].cassette >= 0;
    // KLUB_PIT1 .. KLUB_PIT<KLUB_PCOUNT>, all on to power it (Brakes.dll
    // 0x4015A4). A missing one reads as switch -1, which is no cab object:
    // 0x401600 gives -1 for it, and only a state 0 is off, so it counts as
    // on (the ВЛ10 / ВЛ10У give KLUB_PIT2 twice and no KLUB_PIT1, the ЧС7
    // KLUB_PIT1 four times for KLUB_PCOUNT 4). A name given twice: the
    // first, as RTS finds a parameter by its name (PROBABLE).
    for (int i = 1; i <= power_count; ++i) {
        for (const auto& [n, v] : power) {
            if (n != i) continue;
            p.power.push_back(v);
            break;
        }
    }
    return p;
}

} // namespace openrts::trainplugin
