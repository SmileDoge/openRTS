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
    // ТСКБМ's lamps keep their number before the cab's: TSKBM_L1<n> .. L3<n>
    // (Brakes.dll appends the cab to "TSKBM_L1").
    if (name.substr(0, end) == "tskbm_l" && name.size() > end + 1) ++end;
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
    int power_count = 0, cab_count = 1, saut_power_count = 0;
    std::vector<std::pair<int, std::int32_t>> power, saut_power;
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
        else if (name == "saut_pcount") saut_power_count = id;
        else if (name.starts_with("saut_pit")) saut_power.emplace_back(std::atoi(name.c_str() + 8), id);
        else if (name == "saut_mode") p.saut_mode = value == 1.0f;
        else if (name == "saut_modet") p.saut_mode_switch = id;
        else if (name == "saut_modeon") p.saut_mode_on = static_cast<std::uint32_t>(id);
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
        else if (base == "tskbm_disp") c.tskbm_screen = id;
        else if (base == "saut_komid") c.saut_keys[0] = id;
        else if (base == "saut_grid") c.saut_keys[1] = id;
        else if (base == "saut_ykid") c.saut_keys[2] = id;
        else if (base == "saut_k20id") c.saut_keys[3] = id;
        else if (base == "saut_podid") c.saut_keys[4] = id;
        else if (base == "saut_otpid") c.saut_keys[5] = id;
        else if (base == "saut_osid") c.saut_os = id;
        else if (base == "saut_dbid") c.saut_db = id;
        else if (base == "saut_dfid") c.saut_df = id;
        else if (base == "tskbm_pit") c.tskbm_power = id;
        else if (base == "tskbm_l1") c.tskbm_lamps[0] = id;
        else if (base == "tskbm_l2") c.tskbm_lamps[1] = id;
        else if (base == "tskbm_l3") c.tskbm_lamps[2] = id;
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
    // САУТ's the same way: SAUT_PIT1 .. SAUT_PIT<SAUT_PCOUNT>,
    // a missing one -1 (on).
    for (int i = 1; i <= saut_power_count; ++i) {
        std::int32_t id = -1;
        for (const auto& [n, v] : saut_power) {
            if (n != i) continue;
            id = v;
            break;
        }
        p.saut_power.push_back(id);
    }
    return p;
}

RecorderParameters recorder_parameters(const formats::EngineDefinition& engine) {
    RecorderParameters p;
    const formats::AuxLibraryDefinition* library = nullptr;
    for (const auto& a : engine.aux_libraries) {
        if (is_brakes_dll(a.module)) {
            library = &a;
            break;
        }
    }
    if (!library) return p;
    // A parameter by its name as RTS finds it (the first given), Brakes.dll's
    // per cab with the cab's number appended (param_int).
    const auto find = [&](const std::string& name, std::int32_t* index = nullptr) -> const formats::EngineParameter* {
        for (std::size_t i = 0; i < library->parameters.size(); ++i) {
            if (core::fold_case(library->parameters[i].name) != name) continue;
            if (index) *index = static_cast<std::int32_t>(i);
            return &library->parameters[i];
        }
        return nullptr;
    };
    const auto value = [&](const std::string& name, const float fallback) {
        const auto* found = find(name);
        return found ? found->value : fallback;
    };
    const auto id = [&](const std::string& base, const std::size_t cab) {
        return static_cast<std::int32_t>(value(base + std::to_string(cab), -1.0f));
    };
    auto& k = p.settings;
    k.setup = static_cast<std::uint32_t>(value("sl_setup", 0.0f));
    k.klub_setup = static_cast<std::uint32_t>(value("pb_setup", 0.0f));
    p.trans_sound = value("trans_sound", 0.0f) == 1.0f;
    const bool needle = find("sl_speed1", &p.speed1_index) && value("sl_speed1", -1.0f) > -1.0f;
    const bool kpd = find("sl_kpdspd1", &p.kpd_index) && value("sl_kpdspd1", -1.0f) > -1.0f;
    k.needle = needle;
    // The КПД-3 runs where there is no needle (Brakes.dll: the needle first).
    k.kpd = kpd && !needle;
    k.kpd_mode = static_cast<std::uint32_t>(value("sl_kpdmode", 0.0f));
    k.flip = static_cast<std::int32_t>(value("pb_flip", -1.0f));
    p.speed1 = value("sl_speed1", -1.0f);
    p.kpd_value = value("sl_kpdspd1", -1.0f);
    k.mu = (k.setup & 0x10u) && !needle && !kpd;
    k.mu_speed = value("sl_muspeed", 100.0f);
    find("sl_setup", &p.setup_index);
    p.present = needle || kpd || k.mu;
    p.own = p.present; // the УКБМ (SL_SETUP 8) and the МУ too, since 2026-11-24
    const auto cabs = static_cast<std::size_t>(std::clamp(static_cast<int>(value("br_numcabs", 1.0f)), 1, 8));
    for (std::size_t c = 1; c <= cabs; ++c) {
        simulation::RecorderCab cab;
        cab.epk_key = id("pb_epkkey", c);
        cab.rb = id("pb_rb", c);
        cab.rbs = id("pb_rbs", c);
        cab.als = id("sl_als", c);
        cab.h = id("sl_h", c);
        cab.dvij = id("sl_dvij", c);
        cab.vk = id("sl_vk", c);
        cab.zavod = id("sl_zavod", c);
        cab.kid = id("br_kid", c);
        cab.speed = id("sl_speed", c);
        cab.hour = id("sl_hour", c);
        cab.minute = id("sl_min", c);
        for (std::size_t d = 0; d < cab.odometer.size(); ++d) cab.odometer[d] = id("sl_odom" + std::to_string(d + 1), c);
        cab.pss = id("sl_pss", c);
        cab.hl = id("sl_hl", c);
        cab.pr = id("sl_pr", c);
        cab.kpd_speed = id("sl_kpdspd", c);
        cab.kpd_power = id("sl_kpdswp", c);
        cab.kpd_light = id("sl_kpdswl", c);
        cab.kpd_screen = id("sl_kpddsp", c);
        cab.kpd_upper = id("sl_kpdd1id", c);
        cab.kpd_lower = id("sl_kpdd2id", c);
        cab.kpd_lamp_power = id("sl_kpdlpit", c);
        cab.kpd_lamp_check = id("sl_kpdlkon", c);
        cab.kpd_lamp_dark = id("sl_kpdpod", c);
        cab.kpd_lamp_lit = id("sl_kpdsvt", c);
        cab.kpd_texture = id("sl_kpdatch", c);
        cab.ukbm_power = id("sl_ukbmpw", c);
        cab.mu_l1p = id("sl_mul1p", c);
        cab.mu_l2p = id("sl_mul2p", c);
        cab.mu_l1p1 = id("sl_mul1p1", c);
        cab.mu_l2p1 = id("sl_mul2p1", c);
        for (std::size_t b = 0; b < 7; ++b) {
            cab.mu_lamps[b] = id("sl_mu" + std::to_string(b + 1) + "lp", c);
            cab.mu_keys[b] = id("sl_mu" + std::to_string(b + 1) + "btn", c);
        }
        cab.mu_power1 = id("sl_musw1p", c);
        cab.mu_power2 = id("sl_musw2p", c);
        cab.mu_cassette = id("sl_mukass", c);
        cab.mu_cassette_lamp = id("sl_mukasslp", c);
        cab.mu_pss = id("sl_mupss", c);
        cab.mu_pss2 = id("sl_mupssp", c);
        cab.mu_disp1 = id("sl_mu1disp", c);
        cab.mu_disp1b = id("sl_mu1dispp", c);
        cab.mu_disp2 = id("sl_mu2disp", c);
        cab.mu_disp2b = id("sl_mu2dispp", c);
        cab.mu_fast = id("sl_mumpid", c);
        for (std::size_t b = 0; b < cab.kpd_keys.size(); ++b) cab.kpd_keys[b] = id("sl_kpd" + std::to_string(b + 1) + "btn", c);
        k.cabs.push_back(cab);
    }
    return p;
}

} // namespace openrts::trainplugin
