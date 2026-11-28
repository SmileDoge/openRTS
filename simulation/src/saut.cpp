#include "saut_part.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cwchar>

// openRTS's САУТ (docs/brakes-dll-replacement.md, "The rest", 3), set
// against Brakes.dll's by openrts-train-check --klub shadow: the same
// readings, to the rounding.
namespace openrts::simulation::saut {

namespace {

namespace d = klub_device;
namespace f = klub_flag;

// САУТ's flags.
constexpr std::uint32_t gr = 0x1, yk = 0x2, gr_on = 0x4, yk_on = 0x8, kom = 0x10, moving = 0x20, near_red = 0x40,
                        warned = 0x80, caught = 0x100, pod_moving = 0x200, pod_standing = 0x400, measured = 0x800,
                        k20 = 0x1000, otp = 0x2000, announcing = 0x8000, entry_named = 0x10000, announced = 0x20000;
constexpr std::uint32_t pod = pod_moving | pod_standing;
constexpr std::uint32_t modes = pod | measured | k20 | otp;

std::uint32_t whole(const double x) { return static_cast<std::uint32_t>(static_cast<std::int64_t>(x)); }

// The speed as САУТ counts it: whole km/h of |v| × 3.6, at most 255.
std::uint32_t kmh_of(const float v) {
    const auto kmh = whole(std::abs(static_cast<double>(v) * 3.6));
    return kmh > 255 ? 255 : kmh;
}

std::u16string ascii(const char* format, const long long a, const long long b = 0, const char* middle = nullptr) {
    char buffer[32];
    if (middle) std::snprintf(buffer, sizeof buffer, format, a, middle, b);
    else std::snprintf(buffer, sizeof buffer, format, a);
    std::u16string out;
    for (const char* c = buffer; *c; ++c) out.push_back(static_cast<char16_t>(static_cast<unsigned char>(*c)));
    return out;
}

void text(SautObject& o, std::u16string t) {
    o.text = std::move(t);
    o.text_set = true;
}
void state(SautObject& o, const std::int32_t v) {
    o.state = v;
    o.state_set = true;
}
void blink(SautObject& o, const float period) {
    o.blink = period;
    o.blink_set = true;
}

// CharUpperW for the items' names: Latin and Cyrillic.
std::u16string upper(std::u16string s) {
    for (auto& c : s) {
        if (c >= u'a' && c <= u'z') c = static_cast<char16_t>(c - 32);
        else if (c >= u'а' && c <= u'я') c = static_cast<char16_t>(c - 32);
        else if (c == u'ё') c = u'Ё';
    }
    return s;
}
bool starts(const std::u16string& s, const std::u16string_view with) {
    return s.size() >= with.size() && std::u16string_view(s).substr(0, with.size()) == with;
}

// The signal names САУТ announces within 500 m (equal).
bool named_signal(const std::u16string_view name) {
    for (const std::u16string_view n : {u"1", u"2", u"1+УКСПС", u"2+УКСПС", u"1+УК", u"2+УК"})
        if (name == n) return true;
    return false;
}

} // namespace

// A key, powered, in the working cab (the only one, or its 367
// working). Each takes the aspect and the signal's distance as the last
// step left them.
void keys(KlubState& s, const KlubSettings& k, const KlubInputs& in, KlubOutput& out) {
    const auto& u = in.saut_in;
    if (!u.powered) return;
    const auto kmh = kmh_of(in.speed);
    const auto aspect = s.signal.aspect[0];
    const SautInputs::Cab* working = nullptr;
    for (const auto& c : u.cabs)
        if (u.cabs.size() == 1 || c.working) {
            working = &c;
            break;
        }
    if (!working) return;
    for (const auto key : u.keys) {
        switch (key) {
        case SautKey::kom:
            s.saut_flags = (s.saut_flags & ~(gr | yk)) | kom;
            s.saut_display_timer = 0.0f;
            break;
        case SautKey::gr:
            s.saut_flags = (s.saut_flags & ~(yk | kom)) | gr;
            if (s.saut_flags & gr_on) {
                s.saut_flags &= ~gr_on;
                out.sounds.push_back(3250);
            } else {
                s.saut_flags |= gr_on;
                out.sounds.push_back(3251);
            }
            s.saut_display_timer = 0.0f;
            break;
        case SautKey::yk:
            s.saut_flags = (s.saut_flags & ~(gr | kom)) | yk;
            s.saut_flags ^= yk_on;
            s.saut_display_timer = 0.0f;
            break;
        case SautKey::k20:
            if (!working->on) break;
            if (aspect < 2 || ((s.flags & f::white_permit) && aspect == 2)) {
                if (s.saut_from_50 > 300.0f || aspect == 0) return;
                if (kmh < 1 || working->os) {
                    s.saut_mode_distance = 600.0f;
                    s.saut_flags |= k20;
                    s.saut_mode_time = 0.0f;
                }
                out.sounds.push_back(3215);
            }
            break;
        case SautKey::pod:
            if (!working->on || (aspect != 1 && aspect != 2)) break;
            if (s.saut_from_50 >= 560.0f) return;
            if (in.speed <= 0.01 && in.speed >= -0.01) {
                s.saut_mode_distance = 50.0f;
                s.saut_flags |= pod_standing;
                s.saut_mode_time = 0.0f;
            } else {
                s.saut_mode_distance = 250.0f;
                s.saut_flags |= pod_moving;
            }
            out.sounds.push_back(3215);
            break;
        case SautKey::otp:
            if (!working->on || aspect != 2) break;
            if (s.saut_from_50 >= 560.0f) return;
            s.saut_mode_distance = 600.0f;
            s.saut_flags |= otp;
            s.saut_mode_time = 0.0f;
            out.sounds.push_back(3215);
            break;
        }
    }
    (void)k;
}

// A switch moved (PB_SETUP 1), each cab: its РБ moved in the
// working cab standing lets the caught-up mark go; its САУТ switch turned
// on, powered: 3215.
void switched(KlubState& s, const KlubInputs& in, KlubOutput& out) {
    const auto& u = in.saut_in;
    for (const auto& cab : u.cabs) {
        if (cab.rb_moved && (u.cabs.size() == 1 || cab.working) && std::abs(static_cast<double>(in.speed)) < 0.1 && (s.saut_flags & caught))
            s.saut_flags &= ~caught;
        if (cab.turned_on && u.powered) out.sounds.push_back(3215);
    }
}

// The step (PB_SETUP 1), first in the КЛУБ's.
void step(KlubState& s, const KlubSettings& k, const KlubInputs& in, KlubOutput& out) {
    const auto& u = in.saut_in;
    const float dt = in.dt;
    // Brakes.dll's Init: the coefficient by the mode.
    if (!s.saut_started) {
        s.saut_started = true;
        s.saut_coefficient = u.high ? 67 : 33;
    }
    out.saut_screens.assign(u.cabs.size(), {});
    s.saut_clock += dt;
    if (s.saut_clock > 0.3) s.saut_clock = 0.0f; // 0.3 a double: 0.25 + 0.05 in float is over it
    if (s.saut_flags & (gr | yk | kom)) s.saut_display_timer += dt;
    if (s.saut_display_timer >= 1.0f) {
        s.saut_flags &= ~(gr | yk | kom);
        s.saut_display_timer = 0.0f;
    }
    const auto kmh = kmh_of(in.speed);
    // openRTS's service braking (saut_service_braking()), from the margin of
    // the last redraw: on over the permitted speed, deeper every 5 s it
    // stays over, off 3 km/h under it, standing or unpowered.
    if (!saut_service_braking() || !u.powered || kmh == 0 || !(s.device & d::saut_on)) {
        s.saut_service = 0.0f;
        s.saut_service_timer = 0.0f;
    } else if (s.saut_margin < 0.0f) {
        if (s.saut_service <= 0.0f) {
            s.saut_service = 0.5f;
            s.saut_service_timer = 0.0f;
        }
        s.saut_service_timer += dt;
        if (s.saut_service_timer >= 5.0f && s.saut_service < 1.5f) {
            s.saut_service = (std::min)(s.saut_service + 0.3f, 1.5f);
            s.saut_service_timer = 0.0f;
        }
    } else if (s.saut_margin >= 3.0f) {
        s.saut_service = 0.0f;
        s.saut_service_timer = 0.0f;
    }
    out.saut_service = s.saut_service;
    // Unpowered, and at every redraw: out, blank.
    if (!u.powered || s.saut_clock == 0.0f) {
        s.device &= ~d::saut_on;
        s.saut_braking = false;
        for (auto& screen : out.saut_screens) {
            for (int j = 0; j < 4; ++j) text(screen.db[static_cast<std::size_t>(j)], std::u16string());
            state(screen.db[4], -1);
            state(screen.db[5], -1);
            state(screen.df[0], -1);
            state(screen.df[4], -1);
        }
    }
    if (!u.powered) {
        s.saut_coefficient = u.high ? 67 : 33;
        return;
    }
    // The braking coefficient: every 10 s braking on the move without the ЭПТ.
    if ((u.brake_bits & 0x80) || (!(u.brake_bits & 0x100) && s.saut_coefficient_timer <= 0.0f) || kmh == 0 || u.cylinder <= 0.1 ||
        u.ept) {
        s.saut_coefficient_timer = 0.0f;
    } else if (s.saut_coefficient_timer <= 10.0f) {
        s.saut_coefficient_timer += dt;
    } else {
        s.saut_coefficient =
            static_cast<std::uint32_t>(static_cast<std::int64_t>(std::abs(static_cast<double>(u.acceleration)) / u.cylinder * 200.0));
        s.saut_coefficient_timer = 0.0f;
    }
    const auto aspect = s.signal.aspect[0];
    for (std::size_t j = 0; j < u.cabs.size(); ++j) {
        const auto& cab = u.cabs[j];
        auto& screen = out.saut_screens[j];
        if (!(u.cabs.size() == 1 || cab.working)) {
            state(screen.db[4], 0);
            blink(screen.db[4], 0.5f);
            continue;
        }
        // The aspect's announcement.
        if (cab.on && aspect != s.saut_aspect) {
            switch (aspect) {
            case 0:
            case 8: out.sounds.push_back(3209); s.saut_flags |= announced; break;
            case 1: out.sounds.push_back(3208); s.saut_flags |= announced; break;
            case 2: out.sounds.push_back(3210); s.saut_flags |= announced; break;
            case 3:
            case 4:
            case 5: out.sounds.push_back(3207); s.saut_flags |= announced; break;
            case 6:
            case 7: out.sounds.push_back(3206); break;
            default: break;
            }
            s.saut_aspect = aspect;
        }
        if (cab.on) {
            // The signal's name within 500 m; the items ahead (behind reversed).
            if (named_signal(u.signal_name) && s.saut_signal_distance <= 500.0f) {
                if (!(s.saut_flags & entry_named)) {
                    s.saut_flags |= entry_named;
                    out.sounds.push_back(3202);
                }
            } else {
                s.saut_flags &= ~entry_named;
            }
            // RTSGetTrackItems(1 ahead, 2 behind; the car's flip swaps
            // them), 4000 m.
            std::uint16_t way = (s.device & d::reversed) ? 2 : 1;
            if (u.flipped) way = way == 1 ? 2 : 1;
            std::vector<const KlubTrackItem*> items;
            for (const auto& item : u.items)
                if ((item.pos & way) && item.distance <= 4000.0f) items.push_back(&item);
            const auto announce = [&](const std::uint16_t code) {
                if (!(s.saut_flags & announcing)) {
                    s.saut_flags |= announcing;
                    out.sounds.push_back(code);
                }
            };
            for (std::size_t i = 0; i < items.size(); ++i) {
                const auto& item = *items[i];
                bool done = false;
                const bool within = item.distance > 400.0f && item.distance < 500.0f;
                if (item.type == 3 && within) {
                    // A platform whose name one further on repeats (its other end).
                    const auto name = upper(item.name);
                    for (std::size_t m = i + 1; m < items.size() && !done; ++m) {
                        if (items[m]->type != 3 || upper(items[m]->name) != name) continue;
                        announce(3201);
                        done = true;
                    }
                } else if (item.type == 5 && within) {
                    announce(3214);
                    done = true;
                } else if (item.type == 4 && within) {
                    // A siding named %1 .. %8, %A, repeated further on.
                    static constexpr std::u16string_view marks[] = {u"%1", u"%2", u"%3", u"%4", u"%5", u"%6", u"%7", u"%8", u"%A", u"%a"};
                    bool marked = false;
                    for (const auto mark : marks) marked = marked || starts(item.name, mark);
                    if (marked) {
                        const auto name = upper(item.name);
                        for (std::size_t m = i + 1; m < items.size() && !done; ++m) {
                            if (items[m]->type != 4 || upper(items[m]->name) != name) continue;
                            if (!(s.saut_flags & announcing)) {
                                s.saut_flags |= announcing;
                                static constexpr std::pair<std::u16string_view, std::uint16_t> codes[] = {
                                    {u"%1", 3205}, {u"%2", 3220}, {u"%3", 3216}, {u"%4", 3221}, {u"%5", 3219},
                                    {u"%6", 3212}, {u"%7", 3222}, {u"%8", 3218}, {u"%A", 3223}};
                                for (const auto& [mark, code] : codes)
                                    if (starts(name, mark)) out.sounds.push_back(code);
                            }
                            done = true;
                        }
                    }
                } else if (item.type == 4 && item.distance < 20.0f && starts(item.name, u"%9")) {
                    announce(3217);
                    done = true;
                }
                if (done) break;
                if (i + 1 == items.size()) s.saut_flags &= ~announcing;
            }
        }
        // A mode at a stand: off after 60 s; after a stop's start off, its 10 s.
        if ((s.saut_flags & (pod_standing | k20 | otp)) && kmh == 0) {
            if (s.saut_mode_time < 60.0f) {
                s.saut_mode_time += dt;
            } else {
                s.saut_mode_time = 0.0f;
                s.saut_flags &= ~(pod_standing | measured | k20 | otp);
                s.saut_mode_start = 0.0f;
                s.saut_mode_distance = 0.0f;
            }
        }
        if (kmh == 0 && (s.saut_flags & moving)) {
            s.saut_flags &= ~moving;
            s.saut_stop_timer = 10.0f;
        }
        if (s.saut_stop_timer > 0.0f) {
            s.saut_stop_timer -= dt;
            if (s.saut_stop_timer <= 0.0f && (s.saut_flags & (pod | k20 | otp))) {
                s.saut_flags &= ~modes;
                s.saut_mode_start = 0.0f;
                s.saut_mode_distance = 0.0f;
                s.saut_stop_timer = 0.0f;
            }
        }
        // The target.
        float shown_distance = s.saut_from_50;
        double gone = 0.0;
        if (aspect > 1) {
            if (s.limit <= s.target_speed || static_cast<double>(whole(s.limit - s.target_speed)) * 12.49999999999999974 <= s.target_distance)
                s.saut_target = s.limit;
            else
                s.saut_target = static_cast<float>(static_cast<double>(s.target_distance) * 0.08 + s.target_speed);
            s.saut_flags &= ~modes;
            s.saut_mode_start = 0.0f;
            s.saut_mode_distance = 0.0f;
        } else {
            if ((s.saut_from_50 < 1.0f || (s.saut_flags & k20)) && !(s.saut_flags & measured) && s.saut_mode_distance > 0.0f) {
                s.saut_flags |= measured;
                s.saut_mode_start = u.milepost;
            }
            if (s.saut_mode_start == -1.0f) s.saut_flags &= ~measured;
            if (s.saut_flags & measured) {
                const double a = static_cast<double>(u.milepost) * 1000.0, b = static_cast<double>(s.saut_mode_start) * 1000.0;
                gone = (std::max)(b, a) - (std::min)(b, a);
                if (gone <= s.saut_mode_distance) {
                    if (s.saut_flags & pod) s.saut_target = 17.0f;
                    if (s.saut_flags & k20) s.saut_target = 22.0f;
                    if (s.saut_flags & otp) s.saut_target = 52.0f;
                } else {
                    const double over = gone - s.saut_mode_distance;
                    if (s.saut_flags & pod) s.saut_target = static_cast<float>((34.0 - over) * 0.5);
                    if (s.saut_flags & k20) s.saut_target = static_cast<float>((44.0 - over) * 0.5);
                    if (s.saut_flags & otp) s.saut_target = static_cast<float>((104.0 - over) * 0.5);
                }
            } else if (static_cast<double>(whole(s.limit)) * 12.49999999999999974 <= s.saut_from_50) {
                s.saut_target = (std::max)(s.limit - 1.0f, 0.0f);
            } else {
                s.saut_target = static_cast<float>(static_cast<double>(s.saut_from_50) * 0.08);
                if (s.saut_flags & pod) s.saut_target += 17.0f;
                if (s.saut_flags & k20) s.saut_target = (std::max)(s.saut_target, 22.0f);
                if (s.saut_flags & otp) s.saut_target = (std::max)(s.saut_target, 52.0f);
            }
        }
        if (s.saut_flags & measured) shown_distance = static_cast<float>(s.saut_mode_distance - gone);
        if (shown_distance < 0.0f) shown_distance = 0.0f;
        float cap = u.high ? 162.0f : 102.0f;
        if (k.setup & 8) cap = k.green_speed + 2.0f;
        if (s.saut_target > cap) s.saut_target = cap;
        if (s.saut_target < 0.0f) s.saut_target = 0.0f;
        // The speed shown: 10 km/h a second to the target.
        if (s.saut_target <= s.saut_shown) {
            if (s.saut_target < s.saut_shown) {
                const bool was_zero = s.saut_shown == 0.0f;
                s.saut_shown = static_cast<float>(s.saut_shown - static_cast<double>(dt) * 10.0);
                if (static_cast<double>(s.saut_shown) - s.saut_target < 2.0) s.saut_shown = s.saut_target;
                if (s.saut_shown == 0.0f && !was_zero) out.sounds.push_back(3215);
            }
        } else {
            s.saut_shown = static_cast<float>(static_cast<double>(dt) * 10.0 + s.saut_shown);
            if (static_cast<double>(s.saut_target) - s.saut_shown < 2.0) s.saut_shown = s.saut_target;
        }
        s.saut_distance = shown_distance;
        // On the move the first time: the direction, the reverser at 0.
        if (kmh > 0 && !(s.saut_flags & moving)) {
            const int direction = u.flip >= 0 && static_cast<std::size_t>(u.flip) == j ? -1 : 1;
            s.saut_flags |= moving;
            if (u.reverser == 0) {
                out.sounds.push_back(static_cast<float>(direction) * in.speed <= 0.0f ? 3203 : 3204);
                s.saut_flags |= announced;
            }
        }
        blink(screen.db[4], 0.0f);
        const auto draw = [&](const std::int32_t df4) {
            text(screen.db[0], ascii("%lld", kmh));
            text(screen.db[1], ascii("%lld", static_cast<long long>(s.saut_shown)));
            text(screen.db[2], shown_distance > 10.0f ? ascii("%lld", 10 * static_cast<long long>(0.1 * shown_distance))
                                                      : ascii("%lld", static_cast<long long>(shown_distance)));
            if (u.clock) {
                const auto t = static_cast<long long>(in.day_time);
                text(screen.db[3], ascii("%02lld%s%02lld", t / 3600, std::llabs(t % 3600 / 60), cab.cassette ? ":" : "-"));
            }
            state(screen.df[4], df4);
            if (s.saut_flags & gr) text(screen.db[3], (s.saut_flags & gr_on) ? u"1 " : u"2 ");
            if (s.saut_flags & yk) text(screen.db[3], (s.saut_flags & yk_on) ? u"1 " : u"2 ");
            if (s.saut_flags & kom) {
                text(screen.db[3], ascii("0%02lld", static_cast<long long>(s.saut_coefficient)));
                state(screen.db[5], df4);
            }
        };
        if (cab.on && s.saut_clock == 0.0f) {
            s.device |= d::saut_on;
            draw(0);
            if (s.saut_flags & caught) state(screen.df[0], 0);
            if (kmh == 0) {
                s.saut_flags &= ~caught;
            } else {
                const double margin = static_cast<double>(s.saut_shown) - static_cast<double>(kmh);
                // Against its permitted speed itself, not the one shown (that
                // climbs 10 km/h a second from 0 when it comes on).
                s.saut_margin = static_cast<float>(static_cast<double>(s.saut_target) - static_cast<double>(kmh));
                if (margin <= 3.0 && !(s.saut_flags & warned) && u.traction) {
                    s.saut_flags |= warned;
                    out.sounds.push_back(3211);
                } else if (margin > 3.0) {
                    s.saut_flags &= ~warned;
                }
                if (margin <= 2.0) s.saut_braking = true;
                if (margin < 1.0 && !(s.saut_flags & caught)) {
                    s.saut_flags |= caught;
                    s.saut_caught = u.equalising;
                }
                if (margin > 5.0 && (s.saut_flags & caught)) s.saut_flags &= ~caught;
            }
            if (s.saut_from_50 <= 100.0f && s.saut_from_50 > 90.0f && !(s.saut_flags & near_red) && aspect == 1 &&
                (s.limit_marks & 0x4000)) {
                s.saut_flags |= near_red;
                out.sounds.push_back(3213);
            } else if (s.saut_from_50 > 100.0f) {
                s.saut_flags &= ~near_red;
            }
        } else if (s.saut_clock == 0.0f) {
            draw(1);
            s.saut_flags &= ~caught;
        }
    }
}

} // namespace openrts::simulation::saut
