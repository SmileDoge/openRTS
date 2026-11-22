#include <openrts/simulation/recorder.hpp>

#include <cmath>
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
    if (!k.needle) {
        // Brakes.dll's other way (the КПД-3, the МУ blocks): its lamps off.
        for (const auto& c : k.cabs) {
            state(c.pss, 0);
            state(c.hl, 0);
            state(c.pr, 0);
        }
        return;
    }
    // The needle each second, the clock, the odometer.
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
    // The vigilance.
    const double along = static_cast<double>(in.reverser) * in.speed; // v950
    s.vented = 0.0f;
    bool key = false;
    for (const auto& c : in.cabs) key = key || c.epk_key;
    if (in.powered && key) s.alsn_on = true;
    if (!in.powered || (!key && !(k.setup & 0x20))) s.alsn_on = false;
    const Vigilance v{s, in, out};
    if (!key || (in.powered && (!(k.klub_setup & 0x10) || in.saut_powered))) {
        if (!s.alsn_on || (k.setup & 0x20)) {
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
    const auto sound = [&](const std::uint16_t code) {
        if (in.switched_sounds) out.sounds.push_back(code);
    };
    const double speed = std::abs(static_cast<double>(in.speed));
    // The cab's aspect after Run (the ВК's 2 on К).
    const std::uint32_t aspect = out.cab_aspect >= 0 ? static_cast<std::uint32_t>(out.cab_aspect) : in.aspect;
    // The РБ, the РБС and the ВК (the УКБМ's, SL_SETUP 8, are not this one's).
    if (((aspect != 0 && aspect != 8) || speed < 5.55) && (aspect != 1 || speed < 16.66) && !(s.flags & f::overdue)) {
        for (std::size_t i = 0; i < k.cabs.size() && i < in.cabs.size(); ++i) {
            const auto& c = k.cabs[i];
            const auto& ci = in.cabs[i];
            const auto answer = [&] {
                s.periodic = (aspect == 1 && c.als >= 0 && ci.als) ? -10.0f : 0.0f;
                s.flags &= ~f::stood;
                if (s.whistle != 0.0f) sound(57);
                s.whistle = 0.0f;
            };
            const double limit_rb = ((k.setup & 1) ? 0.0 : 10.0) + 35.0;
            const double limit_rbs = ((k.setup & 1) ? 0.0 : 10.0) + 45.0;
            if (c.rb >= 0 && ci.rb_pressed && !s.whistle_flag && (limit_rb > s.periodic || (s.flags & f::moving)) && in.powered && !(k.setup & 8))
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
