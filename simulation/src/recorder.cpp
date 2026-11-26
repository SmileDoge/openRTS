#include <openrts/simulation/recorder.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

// openRTS's speed recorder and ALSN vigilance (docs/brakes-dll-replacement.md,
// "The rest", 4), set against Brakes.dll's by openrts-train-check --klub
// shadow: the same readings, to the rounding.
namespace openrts::simulation {

namespace {

namespace f = recorder_flag;

// a + b rounded once to float: `a` exact in a double (a float product),
// the sum's error kept (two-sum) to settle a double rounding on a midpoint.
float add_once(const double a, const float b) {
    const double sum = a + b;
    const double bb = sum - a;
    const double error = (a - (sum - bb)) + (b - bb);
    float r = static_cast<float>(sum);
    if (error != 0.0) {
        const float other = std::nextafter(r, static_cast<double>(r) < sum ? std::numeric_limits<float>::infinity()
                                                                            : -std::numeric_limits<float>::infinity());
        const double mid = 0.5 * (static_cast<double>(r) + other);
        if (sum == mid && ((error > 0.0) == (other > r))) r = other;
    }
    return r;
}

// 80-bit numbers for the odometer: the distance (a float) times 0.01 in
// 80 bits (a little under 0.01) and 100.0, each product rounded to 64 bits
// of mantissa, truncated, so its digits turn where Brakes.dll's do.
struct Extended {
    std::uint64_t m{}; // bit 63 set (or 0)
    int e{};           // value m · 2^e
};
constexpr Extended hundredth{0xA3D70A3D70A3D70Aull, -70}; // 0.01 in 80 bits

Extended times(const Extended a, const float f) {
    if (a.m == 0 || f == 0.0f) return {};
    int exponent = 0;
    const double fraction = std::frexp(static_cast<double>(std::abs(f)), &exponent);
    const auto mf = static_cast<std::uint64_t>(std::ldexp(fraction, 24)); // 24 bits
    // The 88-bit product, kept as 128.
    const std::uint64_t a_lo = a.m & 0xFFFFFFFFull, a_hi = a.m >> 32;
    const std::uint64_t p0 = a_lo * mf, p1 = a_hi * mf;
    std::uint64_t lo = p0 + (p1 << 32);
    std::uint64_t hi = (p1 >> 32) + (lo < p0 ? 1 : 0);
    int e = a.e + exponent - 24;
    // Normalise to 64 bits, rounding to nearest, ties to even.
    int top = 63;
    while (top > 0 && !(hi >> top)) --top;
    const int shift = hi ? top + 1 : 0;
    if (shift == 0) {
        int t = 63;
        while (t > 0 && !(lo >> t)) --t;
        return {lo << (63 - t), e - (63 - t)};
    }
    const std::uint64_t dropped = lo & ((1ull << shift) - 1);
    std::uint64_t m = (lo >> shift) | (hi << (64 - shift));
    e += shift;
    const std::uint64_t half = 1ull << (shift - 1);
    if (dropped > half || (dropped == half && (m & 1))) {
        if (++m == 0) {
            m = 1ull << 63;
            ++e;
        }
    }
    return {m, e};
}

long long whole(const Extended a) {
    if (a.m == 0) return 0;
    if (a.e >= 0) return static_cast<long long>(a.m << a.e);
    if (a.e <= -64) return 0;
    return static_cast<long long>(a.m >> -a.e);
}

// The vigilance timer's patterns, as Brakes.dll repeats them.
struct Vigilance {
    RecorderState& s;
    const RecorderInputs& in;
    RecorderOutput& out;
    double dt() const { return in.dt; }

    void sound(const std::uint16_t code) const { out.sounds.push_back(code); }
    // The timer back to 0 (the whistle off).
    void reset() const {
        if (s.whistle != 0.0f) sound(57);
        s.periodic = 0.0f;
        s.whistle = 0.0f;
    }
    // Up to 30 s nothing (the whistle off); past it whistling, at 35 the
    // whistle, past 42 overdue.
    void whistling(const bool flag_at_35, const bool flag_unless_crossing) const {
        if (s.periodic <= 30.0f) {
            if (s.whistle != 0.0f) sound(57);
            s.whistle = 0.0f;
            return;
        }
        if (s.whistle < 1.0f) s.whistle = 1.0f;
        const bool crossing = s.periodic >= 35.0f && static_cast<double>(s.periodic) - dt() < 35.0;
        if (flag_unless_crossing) {
            if (crossing) sound(56);
            else s.whistle_flag = true;
        } else {
            if (crossing) sound(56);
            if (flag_at_35 && !(s.flags & f::stood) && s.periodic >= 35.0f) s.whistle_flag = true;
        }
        if (s.periodic > 42.0f) s.flags |= f::overdue;
    }
    // From 35 − dt / 2 at least, then on.
    void from_34(const bool flag_at_35, const bool flag_unless_crossing) const {
        if (s.periodic < 34.0f) s.periodic = static_cast<float>(35.0 - dt() * 0.5);
        s.periodic = static_cast<float>(in.dt + s.periodic);
        whistling(flag_at_35, flag_unless_crossing);
    }
    void on(const float by) const {
        s.periodic = by + s.periodic;
        whistling(false, false);
    }
};

// The КПД-3 (SL_KPDSPD1, in place of the 3СЛ-2М's needle; docs/brakes-dll-
// replacement.md "The rest", 4): its screens, lamps and light each step.
// Its vigilance is the same as the needle recorder's (Run goes on into it).
std::u16string digits(const char* format, const long long v) {
    char buffer[32];
    std::snprintf(buffer, sizeof buffer, format, v);
    return std::u16string(buffer, buffer + std::strlen(buffer));
}
// Rounded half down (0.5 stays): Brakes.dll's.
long long rounded(const double x) {
    const double whole = std::floor(x);
    return static_cast<long long>(x - whole <= 0.5 ? whole : whole + 1.0);
}

void step_kpd(RecorderState& s, const RecorderSettings& k, const RecorderInputs& in, RecorderOutput& out) {
    const auto state = [&](const std::int32_t id, const std::uint32_t v) {
        if (id >= 0) out.states.emplace_back(id, v);
    };
    const auto text = [&](const std::int32_t screen, const std::uint32_t part, std::u16string t) {
        if (screen < 0) return;
        RecorderOutput::ScreenPart p;
        p.screen = screen;
        p.part = part;
        p.text_set = true;
        p.text = std::move(t);
        out.screen_parts.push_back(std::move(p));
    };
    const auto shown = [&](const std::int32_t screen, const std::uint32_t part, const bool on) {
        if (screen < 0) return;
        RecorderOutput::ScreenPart p;
        p.screen = screen;
        p.part = part;
        p.state_set = true;
        p.state = on ? 0 : -1;
        out.screen_parts.push_back(std::move(p));
    };
    const auto blink = [&](const std::int32_t screen, const std::uint32_t part, const float period) {
        if (screen < 0) return;
        RecorderOutput::ScreenPart p;
        p.screen = screen;
        p.part = part;
        p.blink_set = true;
        p.blink = period;
        out.screen_parts.push_back(std::move(p));
    };
    const double v = std::abs(static_cast<double>(in.speed));
    const auto time = static_cast<long long>(in.day_time);
    const long long hours = time / 3600, minutes = (time - hours * 3600) / 60;
    // Every cab blank first; then each one powered.
    for (const auto& c : k.cabs) {
        for (std::uint32_t p = 0; p < 4; ++p) text(c.kpd_screen, p, u"");
        for (std::uint32_t p = 4; p < 8; ++p) shown(c.kpd_screen, p, false);
        blink(c.kpd_screen, 4, 0.0f);
        state(c.kpd_lamp_power, 0);
        state(c.kpd_lamp_check, 0);
        if (k.kpd_mode == 1) {
            text(c.kpd_upper, 0, u"");
            shown(c.kpd_upper, 1, false);
            blink(c.kpd_upper, 1, 0.0f);
            shown(c.kpd_upper, 0, true);
            text(c.kpd_lower, 0, u"");
            shown(c.kpd_lower, 1, false);
            shown(c.kpd_lower, 0, true);
        }
    }
    bool any = false;
    for (std::size_t n = 0; n < k.cabs.size() && n < in.cabs.size(); ++n) {
        const auto& c = k.cabs[n];
        const auto& ci = in.cabs[n];
        if (c.kpd_power < 0 || !ci.kpd_power) continue;
        any = true;
        if (c.kpd_speed >= 0) out.values.emplace_back(c.kpd_speed, static_cast<float>(v));
        const bool lit = (c.kpd_light >= 0 && ci.kpd_light) || (s.kpd_flags & 1u);
        state(c.kpd_lamp_dark, lit ? 0 : 1);
        state(c.kpd_lamp_lit, lit ? 1 : 0);
        if (c.kpd_texture >= 0) out.textures.emplace_back(c.kpd_texture, static_cast<std::uint16_t>(lit ? 1 : 0));
        state(c.kpd_lamp_power, 1);
        if (c.kpd_lamp_check >= 0) {
            state(c.kpd_lamp_check, 1);
            out.blinks.emplace_back(c.kpd_lamp_check, in.tape_recording ? 0.0f : 0.5f);
        }
        const double kmh = v * 3.6;
        const double accel = std::abs(static_cast<double>(in.acceleration) * 100.0);
        if (k.kpd_mode == 0) {
            const auto screen = c.kpd_screen;
            shown(screen, 6, true);
            shown(screen, 4, true);
            blink(screen, 4, 1.0f);
            text(screen, 1, digits("%02lld", hours) + digits("%02lld", minutes));
            if (kmh > 9.9) {
                text(screen, 0, digits("%lld", rounded(kmh)));
            } else {
                text(screen, 0, digits("%02lld", rounded(v * 36.0)));
                shown(screen, 7, true);
            }
            const float sign = k.flip == static_cast<std::int32_t>(n) ? -1.0f : 1.0f;
            if (sign * in.acceleration < 0.0f) shown(screen, 5, true);
            text(screen, 2, digits("%03lld", rounded(accel)));
            continue;
        }
        // Mode 1: the upper screen by its page, the lower the speed (or the page's number).
        const auto f = s.kpd_flags;
        if (f & 8u) text(c.kpd_upper, 0, u"1000");
        if (f & 2u) {
            text(c.kpd_upper, 0, digits("%03lld", static_cast<long long>(accel)));
            shown(c.kpd_upper, 1, true);
        }
        if (f & 4u) {
            text(c.kpd_upper, 0, digits("%02lld", hours) + digits("%02lld", (time % 3600) / 60));
            shown(c.kpd_upper, 1, true);
            blink(c.kpd_upper, 1, 0.5f);
        }
        if ((f & 0x40u) && !(f & 0x80u)) {
            text(c.kpd_upper, 0, digits("%02lld", s.kpd_day) + digits("%02lld", s.kpd_month));
            shown(c.kpd_upper, 1, true);
            text(c.kpd_lower, 0, u"0");
        } else if (!(f & 0x80u)) {
            if (kmh < 10.0) {
                text(c.kpd_lower, 0, digits("%02lld", static_cast<long long>(kmh * 10.0)));
                shown(c.kpd_lower, 1, true);
            } else {
                text(c.kpd_lower, 0, digits("%lld", static_cast<long long>(kmh)));
            }
        } else if (f & 0x40u) {
            text(c.kpd_upper, 0, digits("%03lld", s.kpd_number2));
            text(c.kpd_lower, 0, u"2");
        } else {
            text(c.kpd_upper, 0, digits("%03lld", s.kpd_number1));
            text(c.kpd_lower, 0, u"1");
        }
    }
    if (k.kpd_mode == 1 && !any) {
        // Powered off: back to the acceleration, the date and numbers gone.
        s.kpd_flags = 2;
        s.kpd_day = s.kpd_month = s.kpd_number1 = s.kpd_number2 = 0;
    }
}

// Its keys (Switched, a key moved to a state not 0), in a powered cab.
void kpd_keys(RecorderState& s, const RecorderSettings& k, const RecorderInputs& in) {
    const auto add100 = [](std::uint32_t& n) {
        n += 100;
        if (n > 999 && (n / 100) % 10 == 9 && (n / 1000) % 10 == 9) n -= 9900;
    };
    for (std::size_t n = 0; n < k.cabs.size() && n < in.cabs.size(); ++n) {
        const auto& ci = in.cabs[n];
        if (k.cabs[n].kpd_power < 0 || !ci.kpd_power || !ci.kpd_pressed) continue;
        auto& f = s.kpd_flags;
        const bool date = (f & 0x40u) && !(f & 0x80u), one = (f & 0x80u) && !(f & 0x40u), two = (f & 0xC0u) == 0xC0u;
        const bool normal = !(f & 0xC0u);
        const auto pressed = [&](const int key) { return ((ci.kpd_pressed >> (key - 1)) & 1u) != 0; };
        if (pressed(1)) f |= 1u;
        if (pressed(7)) f &= ~1u;
        if (pressed(4)) {
            if (two) add100(s.kpd_number2);
            if (!(f & 0x80u)) f = (f & ~(2u | 4u | 0x10u)) | 8u;
            else add100(s.kpd_number1); // on page 2 both, as Brakes.dll
        }
        if (pressed(3)) {
            if (two) f &= ~0x80u;
            else if (date) f = (f & ~0x40u) | 0x80u;
            else if (one) f |= 0x40u;
        }
        if (pressed(2)) {
            if (two) f = (f & ~0xC0u) | 2u;
            else if (f & 0x20u) f = (f & ~(2u | 4u | 0x20u)) | 0x40u;
            else if (f & 2u) f = (f & ~2u) | 4u;
            else f = (f | 2u) & ~4u;
            f &= ~(8u | 0x10u);
        }
        if (pressed(6)) {
            if (two) s.kpd_number2 = s.kpd_number2 % 10 == 9 ? s.kpd_number2 - 9 : s.kpd_number2 + 1;
            else if (normal) f ^= 0x20u;
            else if (date) s.kpd_month = s.kpd_month + 1 > 12 ? 1 : s.kpd_month + 1;
            else if (one) s.kpd_number1 = s.kpd_number1 % 10 == 9 ? s.kpd_number1 - 9 : s.kpd_number1 + 1;
        }
        if (pressed(5)) {
            if (two) s.kpd_number2 = (s.kpd_number2 / 10) % 10 == 9 ? s.kpd_number2 - 90 : s.kpd_number2 + 10;
            else if (date) s.kpd_day = s.kpd_day + 1 > 31 ? 1 : s.kpd_day + 1;
            else if (one) s.kpd_number1 = (s.kpd_number1 / 10) % 10 == 9 ? s.kpd_number1 - 90 : s.kpd_number1 + 10;
        }
    }
}

// The УКБМ (SL_SETUP 8, the 2ТЭ10's, 3ТЭ10's, 2М62У's) in place of the
// 3СЛ-2М's vigilance timer: a check every 30 s; the РБ in its window (30 ..
// 35 s) passes it; missed (the whistle at 35), the lamp SL_PR, the next
// checks shorter (20 / 25 / 32) and the РБ then the РБС to clear it; on К
// and КЖ too fast, the whistle at once and no РБ; the ЭПК past 42 s (Run
// vents on overdue). Its power switch off with the ALSN on: the same.
void ukbm_vigilance(RecorderState& s, const RecorderSettings& k, const RecorderInputs& in, RecorderOutput& out, const double speed,
                    const double along) {
    const double dt = in.dt;
    const auto sound = [&](const std::uint16_t code) { out.sounds.push_back(code); };
    s.flags &= ~ukbm_flag::window;
    s.whistle_flag = false; // + 8 0x10000000: the РБ and РБС refused
    const bool short_cycle = (s.flags & ukbm_flag::short_cycle) != 0;
    const auto latch = [&] {
        if (s.flags & ukbm_flag::missed) s.flags |= ukbm_flag::short_cycle;
        else s.flags &= ~ukbm_flag::short_cycle;
    };
    const auto quiet = [&] {
        if (s.whistle != 0.0f) sound(57);
        s.whistle = 0.0f;
        latch();
    };
    const auto reset = [&] {
        if (s.whistle != 0.0f) sound(57);
        s.periodic = 0.0f;
        s.whistle = 0.0f;
        latch();
    };
    const auto crossing = [&](const float at) { return s.periodic >= at && static_cast<double>(s.periodic) - dt < at; };
    const auto cycle = [&](const float t1, const float t2, const float t3) {
        if (s.periodic <= t1) {
            quiet();
            return;
        }
        if (s.whistle < 1.0f) s.whistle = 1.0f;
        if (s.periodic < t2) s.flags |= ukbm_flag::window;
        if (crossing(t2)) {
            sound(56);
            s.flags |= ukbm_flag::missed;
        }
        if (s.periodic > t3) s.flags |= recorder_flag::overdue;
    };
    const auto standard = [&] {
        if (short_cycle) cycle(20.0f, 25.0f, 32.0f);
        else cycle(30.0f, 35.0f, 42.0f);
    };
    // Too fast on К or КЖ: the whistle at once, the РБ refused, the ЭПК in some 7 s.
    const auto forced = [&] {
        if (s.periodic < 34.0f) s.periodic = static_cast<float>(35.0 - dt * 0.5);
        s.periodic = static_cast<float>(dt + s.periodic);
        if (s.periodic <= 30.0f) {
            quiet();
            return;
        }
        if (s.whistle < 1.0f) s.whistle = 1.0f;
        if (s.periodic < 35.0f) s.flags |= ukbm_flag::window;
        if (s.periodic < 35.0f || !crossing(35.0f)) {
            s.whistle_flag = true;
        } else {
            sound(56);
            s.flags |= ukbm_flag::missed;
        }
        if (s.periodic > 42.0f) s.flags |= recorder_flag::overdue;
    };
    bool powered = false;
    for (std::size_t i = 0; i < k.cabs.size() && i < in.cabs.size(); ++i)
        if (k.cabs[i].ukbm_power >= 0 && in.cabs[i].ukbm_power) powered = true;
    if (!powered) {
        s.flags &= ~recorder_flag::vk;
        if (!s.alsn_on) return;
        s.whistle_flag = true;
        if (s.periodic < 35.0f) s.periodic = static_cast<float>(35.0 - dt);
        s.periodic = static_cast<float>(dt + s.periodic);
        if (s.periodic <= 30.0f) {
            quiet();
            return;
        }
        if (s.whistle < 1.0f) s.whistle = 1.0f;
        if (crossing(35.0f)) sound(56);
        if (s.periodic > 42.0f) s.flags |= recorder_flag::overdue;
        return;
    }
    if (!s.alsn_on) {
        if (s.whistle != 0.0f) sound(57);
        s.periodic = s.aspect_seen = s.whistle = s.blink = 0.0f;
        s.flags &= ~recorder_flag::overdue;
        s.epk_bit = false;
        return;
    }
    const std::uint32_t aspect = in.aspect;
    const auto seen = static_cast<std::int64_t>(s.aspect_seen);
    if (((s.flags & recorder_flag::moving) && speed > 1.6 && !(s.flags & recorder_flag::acts)) || (s.flags & recorder_flag::stood)) {
        if (s.periodic < 30.0f) s.periodic = 30.0f;
        s.periodic = static_cast<float>(dt + s.periodic);
        if (s.periodic <= 30.0f) {
            quiet();
        } else {
            if (s.periodic >= 35.0f && !(s.flags & recorder_flag::stood)) s.whistle_flag = true;
            if (s.periodic < 35.0f) s.flags |= ukbm_flag::window;
            if (s.whistle < 1.0f) s.whistle = 1.0f;
            if (crossing(35.0f)) sound(56);
            if (s.periodic > 42.0f) s.flags |= recorder_flag::overdue;
        }
    } else if (seen == aspect && s.whistle != 2.0f) {
        if ((s.flags & recorder_flag::vk) && (aspect == 0 || aspect == 8)) out.cab_aspect = 2;
        if (in.speed > 0.1f || in.speed < -0.1f) {
            const double a = std::abs(along);
            if (aspect == 6 || aspect == 7) {
                s.periodic = static_cast<float>(dt + s.periodic);
                if (short_cycle) cycle(25.0f, 30.0f, 37.0f);
                else cycle(120.0f, 125.0f, 132.0f);
            } else if (aspect >= 3 && aspect <= 5) {
                s.periodic = static_cast<float>(dt + s.periodic);
                standard();
            } else if (aspect == 1) {
                if (a <= 16.66) {
                    if (a <= 2.77 || in.tskbm_on) reset();
                    else {
                        s.periodic = static_cast<float>(dt + s.periodic);
                        standard();
                    }
                } else {
                    forced();
                }
            } else if (aspect == 2) {
                s.periodic = static_cast<float>(dt * ((s.flags & recorder_flag::vk) ? 1.0 / 3.0 : 1.0) + s.periodic);
                standard();
            } else if (aspect == 0 || aspect == 8) {
                if (a <= 16.66 && (a <= 5.55 || (s.flags & recorder_flag::vk))) {
                    if (a <= 2.77 || in.tskbm_on) reset();
                    else {
                        s.periodic = static_cast<float>(dt + s.periodic);
                        standard();
                    }
                } else {
                    forced();
                }
            } else {
                reset();
            }
        }
    } else if (static_cast<std::uint32_t>(seen) >= aspect || aspect == 8 || aspect <= 1) {
        // A worse aspect: a request.
        if (speed < 0.1) s.flags |= recorder_flag::stood;
        if (seen != aspect) s.periodic = 0.0f;
        if (s.whistle < 2.0f) {
            s.whistle = 2.0f;
            s.periodic = static_cast<float>(35.0 - dt * 0.5);
        }
        if (s.periodic == 0.0f) {
            s.whistle = 0.0f;
            s.blink = 0.0f;
            sound(57);
            latch();
        } else {
            if (s.periodic < 35.0f && static_cast<double>(s.periodic) + dt >= 35.0) sound(56);
            s.periodic = static_cast<float>(dt + s.periodic);
            if (s.periodic > 42.0f) s.flags |= recorder_flag::overdue;
        }
    } else {
        // A better one.
        s.whistle = 0.0f;
        s.blink = 0.0f;
        sound(106);
        s.flags &= ~recorder_flag::vk;
        s.periodic = 0.0f;
    }
    s.aspect_seen = static_cast<float>(aspect);
}

// Its РБ, РБС, ВК and ЭПК key (Switched, SL_SETUP 8).
void ukbm_switched(RecorderState& s, const RecorderSettings& k, const RecorderInputs& in, RecorderOutput& out, const bool gate) {
    const auto sound = [&](const std::uint16_t code) {
        if (in.switched_sounds) out.sounds.push_back(code);
    };
    const auto quiet = [&] {
        if (s.whistle != 0.0f) sound(57);
        s.whistle = 0.0f;
    };
    for (std::size_t i = 0; i < k.cabs.size() && i < in.cabs.size(); ++i) {
        const auto& c = k.cabs[i];
        const auto& ci = in.cabs[i];
        if (gate && c.rb >= 0 && ci.rb_pressed && !(s.flags & ukbm_flag::needs_rbs) && !s.whistle_flag) {
            if (s.flags & ukbm_flag::window) s.flags &= ~ukbm_flag::missed;
            if (s.flags & ukbm_flag::missed) s.flags |= ukbm_flag::needs_rbs;
            s.periodic = 0.0f;
            s.flags &= ~recorder_flag::stood;
            quiet();
        }
        if (gate && c.rbs >= 0 && ci.rbs_pressed && !(s.flags & recorder_flag::overdue) && !s.whistle_flag) {
            s.periodic = 0.0f;
            s.flags &= ~recorder_flag::stood;
            if (s.whistle != 0.0f) {
                if (s.flags & ukbm_flag::needs_rbs) s.flags &= ~(ukbm_flag::needs_rbs | ukbm_flag::missed);
                sound(57);
            }
            s.whistle = 0.0f;
        }
        if (gate && c.vk >= 0 && ci.vk_pressed && s.alsn_on && c.rb >= 0 && ci.rb) {
            s.flags |= recorder_flag::vk;
            quiet();
            s.periodic = 0.0f;
        }
        if (gate && c.epk_key >= 0 && ci.epk_key_moved && !ci.epk_key_to) {
            s.flags &= ~(ukbm_flag::needs_rbs | ukbm_flag::missed | recorder_flag::stood);
            s.periodic = 0.0f;
            quiet();
        }
    }
}

// The speed recorder's МУ blocks (SL_SETUP 0x10 without the needle or the
// КПД-3; the 2ТЭ10М's, ЭР9М's): they do it all themselves: the ALSN's
// power by the ЭПК key, the vigilance (by km/h: 6, 10, 20, 60), the ЭПК,
// the lamp SL_MUPSS blinking; with the ALSN on and its self-test done, the
// permitted speed by the aspect (on КЖ falling after 700 m), the speed
// blinking within 5 km/h of it (3193 / 3194), the clock, the coordinate,
// the date; the self-test (the cassette, keys 1 and 7 together, 20 s).
void step_mu(RecorderState& s, const RecorderSettings& k, const RecorderInputs& in, RecorderOutput& out) {
    const double dt = in.dt;
    const double v = std::abs(static_cast<double>(in.speed));
    const double kmh = v * 3.6;
    const auto sound = [&](const std::uint16_t code) { out.sounds.push_back(code); };
    const auto state = [&](const std::int32_t id, const std::uint32_t value) {
        if (id >= 0) out.states.emplace_back(id, value);
    };
    const auto text = [&](const std::int32_t screen, std::u16string t) {
        if (screen < 0) return;
        RecorderOutput::ScreenPart p;
        p.screen = screen;
        p.part = 0;
        p.text_set = true;
        p.text = std::move(t);
        out.screen_parts.push_back(std::move(p));
    };
    const auto part_shown = [&](const std::int32_t screen, const bool on) {
        if (screen < 0) return;
        RecorderOutput::ScreenPart p;
        p.screen = screen;
        p.part = 1;
        p.state_set = true;
        p.state = on ? 0 : -1;
        out.screen_parts.push_back(std::move(p));
    };
    const auto blink = [&](const std::int32_t screen, const float period) {
        if (screen < 0) return;
        RecorderOutput::ScreenPart p;
        p.screen = screen;
        p.part = 0;
        p.blink_set = true;
        p.blink = period;
        out.screen_parts.push_back(std::move(p));
    };
    const auto crossing = [&] { return s.periodic < 35.0f && static_cast<double>(s.periodic) + dt >= 35.0; };
    const auto quiet = [&] {
        if (s.whistle != 0.0f) sound(57);
        s.whistle = 0.0f;
    };
    const auto cyc = [&] {
        if (s.periodic <= 30.0f) {
            quiet();
            return;
        }
        if (s.whistle < 1.0f) s.whistle = 1.0f;
        if (s.periodic >= 35.0f && static_cast<double>(s.periodic) - dt < 35.0) sound(56);
        if (s.periodic > 42.0f) s.flags |= recorder_flag::overdue;
    };
    const auto start35 = [&] {
        if (s.periodic < 34.0f) s.periodic = static_cast<float>(35.0 - dt * 0.5);
        s.periodic = static_cast<float>(dt + s.periodic);
        cyc();
    };
    const auto reset = [&] {
        if (s.whistle == 2.0f) return;
        quiet();
        s.periodic = 0.0f;
    };
    s.vented = 0.0f;
    for (const auto& c : k.cabs) {
        state(c.mu_l1p1, 0);
        state(c.mu_l2p1, 0);
        for (const auto id : c.mu_lamps) state(id, 0);
        state(c.mu_cassette_lamp, 0);
        state(c.mu_pss, 0);
        state(c.mu_pss2, 0);
        for (const auto id : {c.mu_disp1, c.mu_disp2, c.mu_disp1b, c.mu_disp2b}) text(id, u"");
        part_shown(c.mu_disp1, false);
        part_shown(c.mu_disp1b, false);
    }
    bool key = false;
    for (std::size_t i = 0; i < k.cabs.size() && i < in.cabs.size(); ++i)
        if (k.cabs[i].epk_key >= 0 && in.cabs[i].epk_key) key = true;
    bool any_on = false;
    for (std::size_t i = 0; i < k.cabs.size() && i < in.cabs.size(); ++i) {
        const auto& c = k.cabs[i];
        const auto& ci = in.cabs[i];
        if (!ci.mu_power) continue;
        any_on = true;
        for (std::size_t b = 0; b < 7; ++b)
            if (c.mu_keys[b] >= 0 && c.mu_lamps[b] >= 0) state(c.mu_lamps[b], ci.mu_held[b] ? 1u : 0u);
        state(c.mu_l1p, 1);
        state(c.mu_l2p, 1);
        const auto speed_text = digits("%lld", static_cast<long long>(kmh));
        text(c.mu_disp2, speed_text);
        text(c.mu_disp2b, speed_text);
        // The ALSN's power: the ЭПК key with the train's DLL's ALSN on.
        if (in.powered && key) s.alsn_on = true;
        if (!in.powered || !key) s.alsn_on = false;
        if (!key || in.powered) {
            if (!s.alsn_on) {
                s.periodic = s.aspect_seen = s.whistle = s.blink = 0.0f;
                s.flags &= ~recorder_flag::overdue;
                s.epk_bit = false;
            } else {
                const std::uint32_t a = in.aspect;
                const auto last = static_cast<std::int64_t>(s.aspect_seen);
                if (((s.flags & recorder_flag::moving) && kmh > 6.0 && !(s.flags & recorder_flag::acts)) || (s.flags & recorder_flag::stood)) {
                    start35();
                } else if (last == a && s.whistle != 2.0f) {
                    if ((s.flags & recorder_flag::vk) && (a == 0 || a == 8)) out.cab_aspect = 2;
                    if (v > 0.1) {
                        if (a == 1) {
                            if (kmh <= 60.0) {
                                if (kmh <= 20.0 || in.tskbm_on) reset();
                                else {
                                    s.periodic = static_cast<float>(dt + s.periodic);
                                    cyc();
                                }
                            } else {
                                start35();
                            }
                        } else if (a == 2) {
                            float als = 0.0f;
                            for (std::size_t j = 0; j < k.cabs.size() && j < in.cabs.size(); ++j)
                                als = (k.cabs[j].epk_key >= 0 && in.cabs[j].epk_key && k.cabs[j].als >= 0 && in.cabs[j].als) ? 1.0f : 0.0f;
                            s.periodic = static_cast<float>(dt * 0.5 * (1.0 + als) + s.periodic);
                            cyc();
                        } else if (a == 0 || a == 8) {
                            if (kmh <= 20.0) {
                                if (kmh <= 10.0 || in.tskbm_on) reset();
                                else {
                                    if (s.periodic < 15.0f) s.periodic = 15.0f;
                                    s.periodic = static_cast<float>(dt + s.periodic);
                                    cyc();
                                }
                            } else {
                                start35();
                            }
                        } else if (a >= 3 && a <= 5) {
                            if (kmh <= 60.0 || in.tskbm_on) reset();
                            else {
                                s.periodic = static_cast<float>(dt + s.periodic);
                                cyc();
                            }
                        } else {
                            reset();
                        }
                    }
                } else if (static_cast<std::uint32_t>(last) >= a || a == 8 || a <= 1) {
                    if (v < 0.1) s.flags |= recorder_flag::stood;
                    if (s.whistle < 2.0f) {
                        s.whistle = 2.0f;
                        s.periodic = static_cast<float>(35.0 - dt * 0.5);
                    }
                    if (crossing()) sound(56);
                    s.periodic = static_cast<float>(dt + s.periodic);
                    if (s.periodic > 42.0f) s.flags |= recorder_flag::overdue;
                } else {
                    s.whistle = 0.0f;
                    s.blink = 0.0f;
                    sound(106);
                }
                s.aspect_seen = static_cast<float>(a);
            }
            // Its lamp: steady off, else 1 s on, 1 s off while whistling.
            if (c.epk_key != 0) {
                if (s.whistle == 0.0f) {
                    state(c.mu_pss, 0);
                    state(c.mu_pss2, 0);
                } else {
                    s.blink = static_cast<float>(dt + s.blink);
                    if (s.blink > 2.0f) s.blink -= 2.0f;
                    state(c.mu_pss, s.blink < 1.0f ? 1u : 0u);
                    state(c.mu_pss2, s.blink < 1.0f ? 1u : 0u);
                }
            }
            if (s.flags & recorder_flag::overdue) {
                s.vented = 1.0f;
                s.epk_bit = true;
                out.vent = true;
            }
        } else {
            // The ЭПК key on, the ALSN not powered.
            if (s.periodic < 34.0f) s.periodic = static_cast<float>(35.0 - dt * 0.5);
            s.periodic = static_cast<float>(dt + s.periodic);
            if (s.periodic <= 30.0f) {
                quiet();
            } else if (in.main_reservoir > 0.0f) {
                if (s.whistle < 1.0f) s.whistle = 1.0f;
                if (s.periodic >= 35.0f && static_cast<double>(s.periodic) - dt < 35.0) sound(56);
                if (s.periodic > 42.0f) s.flags |= recorder_flag::overdue;
            }
        }
        const auto time = static_cast<long long>(in.day_time);
        const auto clock_text = digits("%02lld", time / 3600 % 24) + digits("%02lld", time % 3600 / 60);
        if (s.alsn_on && (s.mu_flags & 1u)) {
            // The ALSN display.
            if (c.mu_cassette_lamp >= 0) {
                state(c.mu_cassette_lamp, 1);
                out.blinks.emplace_back(c.mu_cassette_lamp, 0.5f);
            }
            if (ci.mu_held[2] && ci.mu_held[3]) s.mu_flags |= 2u;
            if (ci.mu_held[3] && ci.mu_held[4]) s.mu_flags &= ~2u;
            const std::uint32_t a = in.aspect;
            double permitted = a == 0 || a == 8 ? 20.0 : a == 1 ? 60.0 : a == 2 ? (ci.mu_fast ? 115.0 : 40.0) : a <= 5 ? 60.0 : a <= 7 ? k.mu_speed : 0.0;
            if (a == 1) {
                s.mu_kzh_run = static_cast<float>(v * dt + s.mu_kzh_run);
                if (s.mu_kzh_run > 700.0f) permitted = (std::max)(static_cast<double>(static_cast<long long>(permitted - (s.mu_kzh_run - 700.0) / 15.0)), 20.0);
            } else {
                s.mu_kzh_run = 0.0f;
            }
            if (permitted - kmh >= 5.0) {
                blink(c.mu_disp2, 0.0f);
                blink(c.mu_disp2b, 0.0f);
                if (s.mu_flags & 4u) {
                    s.mu_flags &= ~4u;
                    sound(3194);
                }
            } else {
                blink(c.mu_disp2, 0.5f);
                blink(c.mu_disp2b, 0.5f);
                if (!(s.mu_flags & 4u)) {
                    s.mu_flags |= 4u;
                    sound(3193);
                }
            }
            auto upper = digits("%lld", static_cast<long long>(permitted));
            if (s.mu_flags & 2u) {
                upper = clock_text;
                if (time % 2 == 0) {
                    part_shown(c.mu_disp1, true);
                    part_shown(c.mu_disp1b, true);
                }
            } else if (ci.mu_held[0]) {
                upper = digits("%lld", static_cast<long long>(in.milepost));
            } else if (ci.mu_held[1]) {
                upper = digits("%lld", static_cast<long long>(in.milepost * 1000.0) % 100);
            } else if (ci.mu_held[2]) {
                upper = digits("%lld", in.date_year);
            } else if (ci.mu_held[3]) {
                upper = digits("%lld", in.date_month);
            } else if (ci.mu_held[4]) {
                upper = digits("%lld", in.date_day);
            } else if (ci.mu_held[5]) {
                upper = digits("%lld", in.date_weekday);
            }
            text(c.mu_disp1, upper);
            text(c.mu_disp1b, upper);
        } else {
            // The self-test: the cassette in, keys 1 and 7 together, 20 s.
            if (c.mu_cassette >= 0 && ci.mu_cassette) {
                if (ci.mu_held[0] && ci.mu_held[6]) {
                    s.mu_test = 20.0f;
                    s.mu_flags &= ~1u;
                }
                if (s.mu_test > 0.0f) {
                    s.mu_test = static_cast<float>(s.mu_test - dt);
                    state(c.mu_lamps[6], 1);
                    state(c.mu_lamps[0], 1);
                    if (s.mu_test <= 0.0f) s.mu_flags |= 1u;
                }
            }
            text(c.mu_disp1, clock_text);
            text(c.mu_disp1b, clock_text);
            if (time % 2 == 1) {
                text(c.mu_disp1, u"000");
                text(c.mu_disp1b, u"000");
            } else {
                part_shown(c.mu_disp1, true);
                part_shown(c.mu_disp1b, true);
            }
        }
    }
    if (!any_on) {
        s.mu_test = -1.0f;
        if (s.mu_flags & 4u) {
            s.mu_flags &= ~4u;
            sound(3194);
        }
    }
}

} // namespace

void step_recorder(RecorderState& s, const RecorderSettings& k, const RecorderInputs& in, RecorderOutput& out) {
    out = {};
    const double dt = in.dt;
    // Run's start: the distance run; the ЭПК's bit cleared.
    if (std::abs(in.speed) > 0.0f) s.distance = add_once(std::abs(static_cast<double>(in.speed)) * in.dt, s.distance);
    s.epk_bit = false;
    const double speed = std::abs(static_cast<double>(in.speed)); // v972
    const auto value = [&](const std::int32_t id, const float v) {
        if (id >= 0) out.values.emplace_back(id, v);
    };
    const auto state = [&](const std::int32_t id, const std::uint32_t v) {
        if (id >= 0) out.states.emplace_back(id, v);
    };
    if (k.mu) {
        step_mu(s, k, in, out);
        return;
    }
    if (!k.needle && !k.kpd) {
        // Neither the needle, the КПД-3 nor the МУ: its lamps off. The КПД-3
        // goes on into the vigilance as the needle does.
        for (const auto& c : k.cabs) {
            state(c.pss, 0);
            state(c.hl, 0);
            state(c.pr, 0);
        }
        return;
    }
    if (k.kpd) step_kpd(s, k, in, out);
    // The needle each second, the clock, the odometer.
    if (k.needle) {
    if (s.needle_timer <= 1.0f) s.needle_timer = in.dt + s.needle_timer;
    else s.needle_timer = 0.0f;
    const int time = static_cast<int>(in.day_time);
    const auto hours = static_cast<std::uint32_t>(time / 3600);
    const auto minutes = static_cast<long long>(std::abs(static_cast<double>(static_cast<std::uint32_t>(time % 3600) / 60u)));
    for (const auto& c : k.cabs) {
        if (s.needle_timer == 0.0f && c.speed >= 0) {
            const double v = speed * 3.6 <= 3.0 ? speed : speed + in.random;
            value(c.speed, static_cast<float>(v));
        }
        if (s.flags & f::clock) {
            value(c.hour, static_cast<float>(hours));
            value(c.minute, static_cast<float>(minutes));
        }
        const Extended scaled = times(hundredth, s.distance);
        const auto hundreds = whole(scaled);
        const double part = static_cast<double>(static_cast<std::uint32_t>(whole(times(scaled, 100.0f))) % 100u) * 0.01;
        // Each wheel rolls on with the part while those under it stand at 9.
        bool nines = true;
        long long divisor = 1;
        for (std::size_t d = 0; d < c.odometer.size(); ++d, divisor *= 10) {
            if (c.odometer[d] < 0) {
                nines = false;
                continue;
            }
            const auto digit = static_cast<float>(hundreds / divisor % 10);
            value(c.odometer[d], d == 0 ? static_cast<float>(digit + part) : static_cast<float>((nines ? part : 0.0) + digit));
            nines = nines && digit == 9.0f;
        }
    }
    }
    // The vigilance.
    const double along = static_cast<double>(in.reverser) * in.speed; // v950
    s.vented = 0.0f;
    bool key = false;
    for (const auto& c : in.cabs) key = key || c.epk_key;
    if (in.powered && key) s.alsn_on = true;
    if (!in.powered || (!key && !(k.setup & 0x20))) s.alsn_on = false;
    if (s.alsn_on) {
        s.alsn_on_for += in.dt;
    } else {
        s.alsn_on_for = 0.0f;
        s.request_at_switch_on = false;
    }
    const Vigilance v{s, in, out};
    if (!key || (in.powered && (!(k.klub_setup & 0x10) || in.saut_powered))) {
        if (k.setup & 8) {
            ukbm_vigilance(s, k, in, out, speed, along);
        } else if (!s.alsn_on || (k.setup & 0x20)) {
            if (s.whistle != 0.0f) out.sounds.push_back(57);
            s.periodic = s.aspect_seen = s.whistle = s.blink = 0.0f;
            s.flags &= ~f::overdue;
            s.epk_bit = false;
        } else {
            s.whistle_flag = false;
            const std::uint32_t aspect = in.aspect;
            if (((s.flags & f::moving) && speed > 1.6 && !(s.flags & f::acts)) || (s.flags & f::stood)) {
                v.from_34(true, false);
            } else if (static_cast<std::int64_t>(s.aspect_seen) == aspect && s.whistle != 2.0f) {
                if ((s.flags & f::vk) && (aspect == 0 || aspect == 8)) out.cab_aspect = 2;
                if (in.speed > 0.1 || in.speed < -0.1) {
                    if (aspect < 3 || aspect >= 6) {
                        if (aspect == 1) {
                            if (std::abs(along) <= 16.66) {
                                if (std::abs(along) <= 2.77 || in.tskbm_on) v.reset();
                                else v.on(in.dt);
                            } else {
                                v.from_34(false, true);
                            }
                        } else if (aspect != 0 && aspect != 8) {
                            if (aspect == 2) {
                                // The last cab's ЭПК key with its ALS switch.
                                float als = 0.0f;
                                for (const auto& c : in.cabs) als = (c.epk_key && c.als) ? 1.0f : 0.0f;
                                s.periodic = static_cast<float>(in.dt * 0.5 * (als + 1.0) + s.periodic);
                                v.whistling(false, false);
                            } else {
                                v.reset();
                            }
                        } else if (std::abs(along) <= 16.66 && ((s.flags & f::vk) || std::abs(along) <= 5.55)) {
                            if (std::abs(along) <= 2.77 || in.tskbm_on) {
                                v.reset();
                            } else {
                                if (s.periodic < 15.0f) s.periodic = 15.0f;
                                v.on(in.dt);
                            }
                        } else {
                            v.from_34(false, true);
                        }
                    } else if (std::abs(along) <= 16.66 || in.tskbm_on) {
                        v.reset();
                    } else {
                        v.on(in.dt);
                    }
                }
            } else if (static_cast<std::uint32_t>(static_cast<std::int64_t>(s.aspect_seen)) >= aspect || aspect == 8 || aspect <= 1) {
                // A worse aspect: a request.
                if (speed < 0.1) s.flags |= f::stood;
                if (s.alsn_on_for <= 1.0f) s.request_at_switch_on = true;
                if (s.whistle < 2.0f) {
                    s.whistle = 2.0f;
                    s.periodic = static_cast<float>(35.0 - dt * 0.5);
                }
                if (in.ep_pressure == 0.0f) {
                    s.whistle = 0.0f;
                    s.blink = 0.0f;
                    out.sounds.push_back(57);
                } else {
                    if (s.periodic < 35.0f && static_cast<double>(s.periodic) + dt >= 35.0) out.sounds.push_back(56);
                    s.periodic = in.dt + s.periodic;
                    if (s.periodic > 42.0f) s.flags |= f::overdue;
                }
            } else {
                // A better one.
                s.whistle = 0.0f;
                s.blink = 0.0f;
                out.sounds.push_back(106);
                s.flags &= ~f::vk;
            }
            s.aspect_seen = static_cast<float>(aspect);
        }
    } else {
        // The ЭПК key on, the ALSN not powered.
        if (s.periodic < 34.0f) s.periodic = static_cast<float>(35.0 - dt * 0.5);
        s.periodic = in.dt + s.periodic;
        if (s.periodic <= 30.0f) {
            if (s.whistle != 0.0f) out.sounds.push_back(57);
            s.whistle = 0.0f;
        } else if (in.main_reservoir > 0.0f) {
            if (s.whistle < 1.0f) s.whistle = 1.0f;
            if (s.periodic >= 35.0f && static_cast<double>(s.periodic) - dt < 35.0) out.sounds.push_back(56);
            if (s.periodic > 42.0f) s.flags |= f::overdue;
        }
    }
    // The lamps, the driver's acts.
    bool no_cock = true;
    for (std::size_t i = 0; i < k.cabs.size(); ++i) {
        const auto& c = k.cabs[i];
        const RecorderCabInputs none;
        const auto& ci = i < in.cabs.size() ? in.cabs[i] : none;
        if ((k.setup & 9) && c.epk_key != 0 && in.powered) {
            if (s.whistle == 0.0f) {
                state(c.pss, 0);
            } else {
                if ((k.setup & 0xC) && in.aspect == 1) {
                    s.blink = in.dt + s.blink;
                    if (s.blink > 2.0f) s.blink = static_cast<float>(s.blink - 2.0);
                } else {
                    s.blink = 0.0f;
                }
                state(c.pss, s.blink < 1.0f ? 1u : 0u);
            }
        } else {
            state(c.pss, 0);
        }
        if ((k.setup & 0xA) && s.alsn_on && in.powered) {
            if (speed <= 0.1) s.flags &= ~(f::moving | f::acts);
            else s.flags |= f::moving;
            if (!(s.flags & f::moving) || speed <= 1.6 || (s.flags & f::acts)) {
                state(c.hl, 0);
            } else {
                state(c.hl, 1);
                if (s.periodic < 34.0f) {
                    std::uint32_t bits = 1108080067u; // Brakes.dll's 35.000011
                    float at{};
                    static_assert(sizeof at == sizeof bits);
                    std::memcpy(&at, &bits, sizeof at);
                    s.periodic = at;
                }
                s.whistle = 2.0f;
            }
            if (c.dvij < 0) {
                if (in.reverser == 0) s.flags |= f::acts;
                if (in.traction) s.flags |= f::acts;
                if (c.rb >= 0 && ci.rb) s.flags |= f::acts;
                if (c.rbs >= 0 && ci.rbs) s.flags |= f::acts;
                if (c.h >= 0 && ci.h) s.flags |= f::acts;
            } else if (ci.dvij) {
                s.flags |= f::acts;
            }
        }
        state(c.pr, s.flags & 0x8000u);
        if (c.kid >= 0 && ci.kid) no_cock = false;
    }
    // Running with the ALSN off and the brakes off 12 s: the ЭПК.
    if (!s.alsn_on && in.powered && no_cock) {
        if (s.kon_timer < 12.0f) {
            if ((in.speed > 0.1 || in.speed < -0.1) && ((in.cylinder < 0.7 && in.independent < 0.7) || in.bypass))
                s.kon_timer = in.dt + s.kon_timer;
        } else {
            s.vented = 1.0f;
            s.epk_bit = true;
            out.vent = true;
        }
    } else {
        s.kon_timer = 0.0f;
    }
    if (s.flags & f::overdue) {
        s.vented = 1.0f;
        out.vent = true;
        s.epk_bit = true;
    }
}

void recorder_switched(RecorderState& s, const RecorderSettings& k, const RecorderInputs& in, RecorderOutput& out) {
    if (k.kpd) kpd_keys(s, k, in);
    const auto sound = [&](const std::uint16_t code) {
        if (in.switched_sounds) out.sounds.push_back(code);
    };
    const double speed = std::abs(static_cast<double>(in.speed));
    // The cab's aspect after Run (the ВК's 2 on К).
    const std::uint32_t aspect = out.cab_aspect >= 0 ? static_cast<std::uint32_t>(out.cab_aspect) : in.aspect;
    // The РБ, the РБС and the ВК (the УКБМ's, SL_SETUP 8, its own).
    const bool gate = ((aspect != 0 && aspect != 8) || speed < 5.55) && (aspect != 1 || speed < 16.66) && !(s.flags & f::overdue);
    if (k.setup & 8) ukbm_switched(s, k, in, out, gate);
    if (gate) {
        for (std::size_t i = 0; i < k.cabs.size() && i < in.cabs.size(); ++i) {
            const auto& c = k.cabs[i];
            const auto& ci = in.cabs[i];
            const auto answer = [&] {
                s.periodic = (aspect == 1 && c.als >= 0 && ci.als) ? -10.0f : 0.0f;
                s.flags &= ~f::stood;
                s.request_at_switch_on = false;
                if (s.whistle != 0.0f) sound(57);
                s.whistle = 0.0f;
            };
            const double limit_rb = ((k.setup & 1) ? 0.0 : 10.0) + 35.0;
            const double limit_rbs = ((k.setup & 1) ? 0.0 : 10.0) + 45.0;
            if (c.rb >= 0 && ci.rb_pressed && !s.whistle_flag && (limit_rb > s.periodic || (s.flags & f::moving) || s.request_at_switch_on) &&
                in.powered && !(k.setup & 8))
                answer();
            if (c.rbs >= 0 && ci.rbs_pressed && !s.whistle_flag && limit_rbs > s.periodic && in.powered && !(k.setup & 8)) answer();
            if (c.vk >= 0 && ci.vk_pressed && s.alsn_on && !(k.setup & 8)) {
                s.flags |= f::vk;
                if (s.whistle != 0.0f) sound(57);
                s.whistle = 0.0f;
            }
        }
    }
    for (std::size_t i = 0; i < k.cabs.size() && i < in.cabs.size(); ++i) {
        const auto& c = k.cabs[i];
        const auto& ci = in.cabs[i];
        // The ЭПК key sets the ALSN on or off at once (the КЛУБ's part, any
        // PB_SETUP).
        if (k.klub_setup != 0 && c.epk_key >= 0 && ci.epk_key_moved) s.alsn_on = ci.epk_key_to;
        // The clock's key: wound, it runs.
        if (c.zavod >= 0 && ci.zavod_moved) {
            sound(970);
            if (!(s.flags & f::clock)) {
                s.flags |= f::clock;
                sound(171);
            }
        }
    }
}

} // namespace openrts::simulation
