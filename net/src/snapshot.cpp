#include <openrts/net/snapshot.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>

#include <openrts/net/packages.hpp>
#include <openrts/net/play.hpp>

namespace openrts::net {

namespace {

constexpr std::uint16_t P(const Package package) noexcept { return id(package); }

std::string hex(const std::uint16_t value) {
    static constexpr char digits[] = "0123456789ABCDEF";
    std::string out = "0x";
    for (int shift = 12; shift >= 0; shift -= 4) out.push_back(digits[(value >> shift) & 0xF]);
    return out;
}

std::string describe(const std::uint16_t value) {
    const auto name = package_name(value);
    return name.empty() ? hex(value) : hex(value) + " " + std::string(name);
}

// Parses one body; false when it is short or has bytes left over.
template <typename Parse>
bool parse_body(const ByteView body, Parse&& parse) {
    WireReader reader(body);
    parse(reader);
    return reader.ok() && reader.at_end();
}

PlayerRecord read_player(WireReader& r) {
    PlayerRecord p;
    p.user = r.u32();
    p.train = r.u32();
    p.value_8 = r.u32();
    p.value_12 = r.u32();
    for (auto& v : p.position) v = r.f32();
    p.value_28 = r.u32();
    for (auto& v : p.direction) v = r.f32();
    const auto name = r.bytes(p.name.size());
    if (name.size() == p.name.size()) std::copy(name.begin(), name.end(), p.name.begin());
    return p;
}

void write_player(WireWriter& w, const PlayerRecord& p) {
    w.u32(p.user);
    w.u32(p.train);
    w.u32(p.value_8);
    w.u32(p.value_12);
    for (const auto v : p.position) w.f32(v);
    w.u32(p.value_28);
    for (const auto v : p.direction) w.f32(v);
    w.bytes(p.name);
}

TrainAssign read_assign(WireReader& r) {
    TrainAssign a;
    a.mine = r.u8();
    a.number = r.u32();
    a.subtype = r.u16();
    a.kind = r.u16();
    return a;
}

void write_assign(WireWriter& w, const std::uint32_t train, const TrainAssign& a) {
    w.u32(train);
    w.u8(a.mine);
    w.u32(a.number);
    w.u16(a.subtype);
    w.u16(a.kind);
}

bool read_car(const Frame& frame, TrainCar& car) {
    if (frame.id != P(Package::train_car)) return false;
    return parse_body(frame.body, [&](WireReader& r) {
        car.kind = r.u16();
        car.file = r.text_u32();
        car.uid = r.u32();
        car.flags = r.u32();
        car.mass = r.f32();
        car.width = r.f32();
        car.length = r.f32();
        car.height = r.f32();
        if (r.remaining() >= 4) car.handbrake = r.f32();
        if (r.remaining() >= 4) car.max_handbrake = r.f32();
        const auto extra = r.rest();
        car.extra.assign(extra.begin(), extra.end());
    });
}

bool read_train(const ByteView body, SnapshotTrain& train) {
    return parse_body(body, [&](WireReader& r) {
        train.id = r.u32();
        train.kind = r.u16();
        train.subtype = r.u16();
        train.top_speed = r.f32();
        train.state = r.u8();
        train.value_4607 = r.u32();
        const auto list = r.frame();
        if (list.id != P(Package::train_cars)) {
            r.fail();
            return;
        }
        WireReader cars(list.body);
        const auto count = cars.u32();
        for (std::uint32_t i = 0; i < count && cars.ok(); ++i) {
            TrainCar car;
            if (!read_car(cars.frame(), car)) {
                r.fail();
                return;
            }
            train.cars.push_back(std::move(car));
        }
        if (!cars.ok() || !cars.at_end()) {
            r.fail();
            return;
        }
        train.value_84 = r.f32();
        train.value_88 = r.f32();
    });
}

bool read_pos(const ByteView body, std::uint32_t& train, TrainPos& pos) {
    return parse_body(body, [&](WireReader& r) {
        train = r.u32();
        pos.speed = r.f32();
        pos.acceleration = r.f32();
        pos.node = r.u32();
        pos.offset = r.f32();
        pos.direction = r.u8();
        const auto count = r.u32();
        for (std::uint32_t i = 0; i < count && r.ok(); ++i) {
            const auto car = r.frame();
            if (car.id != P(Package::car_pos)) {
                r.fail();
                return;
            }
            WireReader c(car.body);
            CarPos p;
            p.offset = c.f64();
            p.speed = c.f32();
            p.acceleration = c.f32();
            if (!c.ok() || !c.at_end()) {
                r.fail();
                return;
            }
            pos.cars.push_back(p);
        }
    });
}

void write_train(WireWriter& w, const SnapshotTrain& t) {
    w.begin(P(Package::train_add));
    w.u32(t.id);
    w.u16(t.kind);
    w.u16(t.subtype);
    w.f32(t.top_speed);
    w.u8(t.state);
    w.u32(t.value_4607);
    w.begin(P(Package::train_cars));
    w.u32(static_cast<std::uint32_t>(t.cars.size()));
    for (const auto& car : t.cars) {
        w.begin(P(Package::train_car));
        w.u16(car.kind);
        w.text_u32(car.file);
        w.u32(car.uid);
        w.u32(car.flags);
        w.f32(car.mass);
        w.f32(car.width);
        w.f32(car.length);
        w.f32(car.height);
        if (car.handbrake) w.f32(*car.handbrake);
        if (car.max_handbrake) w.f32(*car.max_handbrake);
        w.bytes(car.extra);
        w.end();
    }
    w.end();
    w.f32(t.value_84);
    w.f32(t.value_88);
    w.end();
}

void write_pos(WireWriter& w, const SnapshotTrain& t) {
    const auto& pos = *t.pos;
    w.begin(t.forced ? P(Package::train_pos_forced) : P(Package::train_pos));
    w.u32(t.id);
    w.f32(pos.speed);
    w.f32(pos.acceleration);
    w.u32(pos.node);
    w.f32(pos.offset);
    w.u8(pos.direction);
    w.u32(static_cast<std::uint32_t>(pos.cars.size()));
    for (const auto& car : pos.cars) {
        w.begin(P(Package::car_pos));
        w.f64(car.offset);
        w.f32(car.speed);
        w.f32(car.acceleration);
        w.end();
    }
    w.end();
}

} // namespace

std::string PlayerRecord::name_utf8() const {
    std::u16string text;
    for (std::size_t i = 0; i + 1 < name.size(); i += 2) {
        const auto unit = static_cast<char16_t>(name[i] | (name[i + 1] << 8));
        if (unit == 0) break;
        text.push_back(unit);
    }
    return to_utf8(text);
}

SnapshotRead read_snapshot(const ByteView data) {
    SnapshotRead result;
    auto& s = result.snapshot;
    const auto list = split_frames(data);
    if (!list.error.empty()) result.problems.push_back(list.error);
    const auto train_of = [&](const std::uint32_t id) -> SnapshotTrain* {
        for (auto& t : s.trains) {
            if (t.id == id) return &t;
        }
        return nullptr;
    };
    const auto keep = [&](const Frame& f, const std::string& why) {
        result.problems.push_back(describe(f.id) + " at byte " + std::to_string(f.offset) + ": " + why);
        s.other.push_back({f.id, Bytes(f.body.begin(), f.body.end())});
    };
    for (const auto& f : list.frames) {
        const auto body = f.body;
        switch (f.id) {
        case P(Package::login):
            if (body.empty()) {
                s.root = true;
            } else {
                keep(f, "a login with a body inside a snapshot");
            }
            break;
        case P(Package::snapshot):
            s.complete = true;
            if (!body.empty()) keep(f, "the end with a body");
            break;
        case P(Package::time): {
            float time{};
            if (parse_body(body, [&](WireReader& r) { time = r.f32(); })) {
                s.time = time;
            } else {
                keep(f, "not a time");
            }
            break;
        }
        case P(Package::junctions): {
            std::vector<JunctionState> junctions;
            const bool ok = parse_body(body, [&](WireReader& r) {
                while (r.ok() && !r.at_end()) {
                    const auto entry = r.frame();
                    if (entry.id != P(Package::junction)) {
                        r.fail();
                        return;
                    }
                    JunctionState j;
                    if (!parse_body(entry.body, [&](WireReader& e) {
                            j.index = e.u32();
                            j.branch = e.u32();
                            j.lock = e.u8();
                        })) {
                        r.fail();
                        return;
                    }
                    junctions.push_back(j);
                }
            });
            if (ok) {
                s.junctions.insert(s.junctions.end(), junctions.begin(), junctions.end());
            } else {
                keep(f, "a broken junction list");
            }
            break;
        }
        case P(Package::signals): {
            std::vector<SignalHeadState> heads;
            const bool ok = parse_body(body, [&](WireReader& r) {
                while (r.ok() && !r.at_end()) {
                    const auto entry = r.frame();
                    if (entry.id != P(Package::signal_head)) {
                        r.fail();
                        return;
                    }
                    SignalHeadState h;
                    if (!parse_body(entry.body, [&](WireReader& e) {
                            h.item = e.u32();
                            h.semaphore = e.u16();
                            h.state = e.f32();
                            h.draw_state = e.f32();
                            h.value_92 = e.f32();
                            h.mode = e.f32();
                            h.speed = e.f32();
                        })) {
                        r.fail();
                        return;
                    }
                    heads.push_back(h);
                }
            });
            if (ok) {
                s.signals.insert(s.signals.end(), heads.begin(), heads.end());
            } else {
                keep(f, "a broken signal list");
            }
            break;
        }
        case P(Package::substation): {
            if (const auto x = decode_substation(body)) {
                s.substations.push_back(*x);
            } else {
                keep(f, "not a substation");
            }
            break;
        }
        case P(Package::phase_voltages): {
            if (auto phases = decode_phase_voltages(body); phases && s.phases.empty()) {
                s.phases = std::move(*phases);
            } else {
                keep(f, "not phase voltages, or a second list");
            }
            break;
        }
        case P(Package::section_source): {
            if (const auto x = decode_section_source(body)) {
                s.sections.push_back(*x);
            } else {
                keep(f, "not a section source");
            }
            break;
        }
        case P(Package::piece_supply): {
            if (const auto x = decode_piece_supply(body)) {
                s.pieces.push_back(*x);
            } else {
                keep(f, "not a piece supply");
            }
            break;
        }
        case P(Package::train_add): {
            SnapshotTrain t;
            if (!read_train(body, t)) {
                keep(f, "a broken train");
            } else if (train_of(t.id)) {
                keep(f, "a second train " + std::to_string(t.id));
            } else {
                s.trains.push_back(std::move(t));
            }
            break;
        }
        case P(Package::train_pos):
        case P(Package::train_pos_forced): {
            std::uint32_t train{};
            TrainPos pos;
            auto* t = read_pos(body, train, pos) ? train_of(train) : nullptr;
            if (!t || t->pos) {
                keep(f, t ? "a second position" : "a position for no train");
            } else {
                t->pos = std::move(pos);
                t->forced = f.id == P(Package::train_pos_forced);
            }
            break;
        }
        case P(Package::train_stands): {
            std::uint32_t train{};
            auto* t = parse_body(body, [&](WireReader& r) { train = r.u32(); }) ? train_of(train) : nullptr;
            if (!t || t->stands) {
                keep(f, "not a standing train of the snapshot");
            } else {
                t->stands = true;
            }
            break;
        }
        case P(Package::train_assign):
        case P(Package::helper_assign): {
            std::uint32_t train{};
            TrainAssign a;
            auto* t = parse_body(body,
                                 [&](WireReader& r) {
                                     train = r.u32();
                                     a = read_assign(r);
                                 })
                          ? train_of(train)
                          : nullptr;
            auto* slot = t ? (f.id == P(Package::train_assign) ? &t->assign : &t->helper) : nullptr;
            if (!slot || *slot) {
                keep(f, "not an assignment of a snapshot train");
            } else {
                *slot = a;
            }
            break;
        }
        case P(Package::train_names): {
            std::uint32_t train{};
            std::array<std::u16string, 2> names;
            auto* t = parse_body(body,
                                 [&](WireReader& r) {
                                     train = r.u32();
                                     names[0] = r.text_u32();
                                     names[1] = r.text_u32();
                                 })
                          ? train_of(train)
                          : nullptr;
            if (!t || t->names) {
                keep(f, "not the names of a snapshot train");
            } else {
                t->names = std::move(names);
            }
            break;
        }
        case P(Package::train_brakes): {
            std::uint32_t train{};
            std::array<float, 4> values{};
            auto* t = parse_body(body,
                                 [&](WireReader& r) {
                                     train = r.u32();
                                     for (auto& v : values) v = r.f32();
                                 })
                          ? train_of(train)
                          : nullptr;
            if (!t || t->brakes) {
                keep(f, "not the brakes of a snapshot train");
            } else {
                t->brakes = values;
            }
            break;
        }
        case P(Package::route_left): {
            std::uint32_t train{};
            float length{};
            std::uint8_t flag{};
            auto* t = parse_body(body,
                                 [&](WireReader& r) {
                                     train = r.u32();
                                     length = r.f32();
                                     flag = r.u8();
                                 })
                          ? train_of(train)
                          : nullptr;
            if (!t || t->route_left) {
                keep(f, "not the route of a snapshot train");
            } else {
                t->route_left = std::pair{length, flag};
            }
            break;
        }
        case P(Package::car_cargo):
        case P(Package::derail): {
            WireReader r(body);
            auto* t = train_of(r.u32());
            if (!r.ok() || !t || (f.id == P(Package::derail) && t->derail)) {
                keep(f, "not a package of a snapshot train");
            } else if (f.id == P(Package::car_cargo)) {
                t->cargo.emplace_back(body.begin(), body.end());
            } else {
                t->derail = Bytes(body.begin(), body.end());
            }
            break;
        }
        case P(Package::user):
        case P(Package::player_pos): {
            PlayerRecord p;
            if (parse_body(body, [&](WireReader& r) { p = read_player(r); })) {
                (f.id == P(Package::user) ? s.users : s.players).push_back(p);
            } else {
                keep(f, "not a player record");
            }
            break;
        }
        case P(Package::pause):
        case P(Package::host_mark): {
            std::uint8_t value{};
            auto& slot = f.id == P(Package::pause) ? s.pause : s.host_mark;
            if (!slot && parse_body(body, [&](WireReader& r) { value = r.u8(); })) {
                slot = value;
            } else {
                keep(f, "not a single byte, or a second one");
            }
            break;
        }
        default:
            s.other.push_back({f.id, Bytes(body.begin(), body.end())});
            break;
        }
    }
    return result;
}

Bytes write_snapshot(const Snapshot& s) {
    WireWriter w;
    if (s.root) {
        w.begin(P(Package::login));
        w.end();
    }
    if (s.time) {
        w.begin(P(Package::time));
        w.f32(*s.time);
        w.end();
    }
    if (!s.junctions.empty()) {
        w.begin(P(Package::junctions));
        for (const auto& j : s.junctions) {
            w.begin(P(Package::junction));
            w.u32(j.index);
            w.u32(j.branch);
            w.u8(j.lock);
            w.end();
        }
        w.end();
    }
    for (const auto& x : s.substations) {
        w.begin(P(Package::substation));
        w.u32(x.id);
        w.u32(x.flags);
        w.f32(x.voltage);
        w.u32(x.tail);
        w.end();
    }
    if (!s.phases.empty()) {
        w.begin(P(Package::phase_voltages));
        w.u32(static_cast<std::uint32_t>(s.phases.size()));
        for (const auto& p : s.phases) {
            w.u32(p.id);
            w.u16(static_cast<std::uint16_t>(p.voltages.size()));
            for (const auto v : p.voltages) w.f32(v);
        }
        w.end();
    }
    for (const auto& x : s.sections) {
        w.begin(P(Package::section_source));
        w.u32(x.section);
        w.i16(x.source);
        w.end();
    }
    for (const auto& x : s.pieces) {
        w.begin(P(Package::piece_supply));
        w.u32(x.node);
        w.u32(x.piece);
        w.i16(x.source);
        w.u8(x.keep);
        w.end();
    }
    if (!s.signals.empty()) {
        w.begin(P(Package::signals));
        for (const auto& h : s.signals) {
            w.begin(P(Package::signal_head));
            w.u32(h.item);
            w.u16(h.semaphore);
            w.f32(h.state);
            w.f32(h.draw_state);
            w.f32(h.value_92);
            w.f32(h.mode);
            w.f32(h.speed);
            w.end();
        }
        w.end();
    }
    for (const auto& t : s.trains) {
        write_train(w, t);
        if (t.pos) write_pos(w, t);
        if (t.names) {
            w.begin(P(Package::train_names));
            w.u32(t.id);
            w.text_u32((*t.names)[0]);
            w.text_u32((*t.names)[1]);
            w.end();
        }
        if (t.stands) {
            w.begin(P(Package::train_stands));
            w.u32(t.id);
            w.end();
        }
        if (t.assign) {
            w.begin(P(Package::train_assign));
            write_assign(w, t.id, *t.assign);
            w.end();
        }
        if (t.brakes) {
            w.begin(P(Package::train_brakes));
            w.u32(t.id);
            for (const auto v : *t.brakes) w.f32(v);
            w.end();
        }
        for (const auto& cargo : t.cargo) w.frame(P(Package::car_cargo), cargo);
        if (t.derail) w.frame(P(Package::derail), *t.derail);
        if (t.helper) {
            w.begin(P(Package::helper_assign));
            write_assign(w, t.id, *t.helper);
            w.end();
        }
        if (t.route_left) {
            w.begin(P(Package::route_left));
            w.u32(t.id);
            w.f32(t.route_left->first);
            w.u8(t.route_left->second);
            w.end();
        }
    }
    for (const auto& raw : s.other) w.frame(raw.id, raw.body);
    for (const auto& p : s.users) {
        w.begin(P(Package::user));
        write_player(w, p);
        w.end();
    }
    for (const auto& p : s.players) {
        w.begin(P(Package::player_pos));
        write_player(w, p);
        w.end();
    }
    if (s.host_mark) {
        w.begin(P(Package::host_mark));
        w.u8(*s.host_mark);
        w.end();
    }
    if (s.pause) {
        w.begin(P(Package::pause));
        w.u8(*s.pause);
        w.end();
    }
    if (s.complete) {
        w.begin(P(Package::snapshot));
        w.end();
    }
    return w.take();
}

std::optional<float> decode_time(const ByteView body) {
    float time{};
    if (!parse_body(body, [&](WireReader& r) { time = r.f32(); })) return std::nullopt;
    return time;
}

std::optional<JunctionState> decode_junction(const ByteView body) {
    JunctionState j;
    if (!parse_body(body, [&](WireReader& r) {
            j.index = r.u32();
            j.branch = r.u32();
            j.lock = r.u8();
        })) {
        return std::nullopt;
    }
    return j;
}

std::optional<SignalHeadState> decode_signal_head(const ByteView body) {
    SignalHeadState h;
    if (!parse_body(body, [&](WireReader& r) {
            h.item = r.u32();
            h.semaphore = r.u16();
            h.state = r.f32();
            h.draw_state = r.f32();
            h.value_92 = r.f32();
            h.mode = r.f32();
            h.speed = r.f32();
        })) {
        return std::nullopt;
    }
    return h;
}

std::optional<SignalControl> decode_signal_control(const ByteView body) {
    SignalControl c;
    if (!parse_body(body, [&](WireReader& r) {
            c.unit = r.u32();
            c.aspect = r.u16();
            c.mode = r.f32();
            c.pinned = r.u8();
        })) {
        return std::nullopt;
    }
    return c;
}

namespace {

// u16 n, then n UTF-16 units and a zero when n > 0 (rts_server.exe's
// station name in 0x450B, a reason in 0x450C).
void write_text_z(WireWriter& w, const std::u16string& text) {
    w.u16(static_cast<std::uint16_t>(text.size()));
    if (text.empty()) return;
    for (const auto c : text) w.u16(static_cast<std::uint16_t>(c));
    w.u16(0);
}

std::u16string read_text_z(WireReader& r) {
    const auto n = r.u16();
    std::u16string text;
    if (n == 0) return text;
    for (std::uint16_t i = 0; i < n; ++i) text.push_back(static_cast<char16_t>(r.u16()));
    r.u16();
    return text;
}

} // namespace

std::optional<RouteSegment> decode_route_segment(const ByteView body) {
    RouteSegment s;
    if (!parse_body(body, [&](WireReader& r) {
            s.id = r.u32();
            s.train = r.u32();
            s.route = r.u32();
            s.station = read_text_z(r);
            s.tile_x = static_cast<std::int16_t>(r.u16());
            s.tile_z = static_cast<std::int16_t>(r.u16());
            s.reverse = r.u8();
            s.hold = r.u8();
            s.reversed = r.u8();
            s.length = r.f32();
            s.priority = r.u32();
            s.state = r.u8();
            s.path = r.u16();
            s.thrown = r.u16();
        })) {
        return std::nullopt;
    }
    return s;
}

Bytes encode_route_segment(const RouteSegment& value) {
    WireWriter w;
    w.u32(value.id);
    w.u32(value.train);
    w.u32(value.route);
    write_text_z(w, value.station);
    w.u16(static_cast<std::uint16_t>(value.tile_x));
    w.u16(static_cast<std::uint16_t>(value.tile_z));
    w.u8(value.reverse);
    w.u8(value.hold);
    w.u8(value.reversed);
    w.f32(value.length);
    w.u32(value.priority);
    w.u8(value.state);
    w.u16(value.path);
    w.u16(value.thrown);
    return w.take();
}

std::optional<RouteSegmentReply> decode_route_segment_reply(const ByteView body) {
    RouteSegmentReply reply;
    WireReader r(body);
    reply.request = r.u32();
    reply.id = r.u32();
    reply.ok = r.u8();
    if (!r.ok()) return std::nullopt;
    // The reason, when there is one: UTF-16 to a zero.
    while (r.remaining() >= 2) {
        const auto c = r.u16();
        if (c == 0) break;
        reply.text.push_back(static_cast<char16_t>(c));
    }
    return reply;
}

Bytes encode_route_segment_reply(const RouteSegmentReply& value) {
    WireWriter w;
    w.u32(value.request);
    w.u32(value.id);
    w.u8(value.ok);
    if (!value.text.empty()) {
        for (const auto c : value.text) w.u16(static_cast<std::uint16_t>(c));
        w.u16(0);
    }
    return w.take();
}

std::optional<RouteSegment> decode_route_segment_state(const ByteView body) {
    RouteSegment s;
    if (!parse_body(body, [&](WireReader& r) {
            s.id = r.u32();
            s.state = r.u8();
            s.path = r.u16();
            s.priority = r.u32();
            s.length = r.f32();
            s.thrown = r.u16();
        })) {
        return std::nullopt;
    }
    return s;
}

Bytes encode_route_segment_state(const RouteSegment& value) {
    WireWriter w;
    w.u32(value.id);
    w.u8(value.state);
    w.u16(value.path);
    w.u32(value.priority);
    w.f32(value.length);
    w.u16(value.thrown);
    return w.take();
}

Bytes encode_signal_control(const SignalControl& value) {
    WireWriter w;
    w.u32(value.unit);
    w.u16(value.aspect);
    w.f32(value.mode);
    w.u8(value.pinned);
    return w.take();
}

std::optional<SnapshotTrain> decode_train_add(const ByteView body) {
    SnapshotTrain t;
    if (!read_train(body, t)) return std::nullopt;
    return t;
}

std::optional<TrainPosUpdate> decode_train_pos(const ByteView body) {
    TrainPosUpdate update;
    if (!read_pos(body, update.train, update.pos)) return std::nullopt;
    return update;
}

std::optional<std::uint32_t> decode_train_id(const ByteView body) {
    std::uint32_t train{};
    if (!parse_body(body, [&](WireReader& r) { train = r.u32(); })) return std::nullopt;
    return train;
}

std::optional<TrainRemove> decode_train_remove(const ByteView body) {
    TrainRemove x;
    if (!parse_body(body, [&](WireReader& r) {
            x.train = r.u32();
            x.remove = r.u8();
        })) {
        return std::nullopt;
    }
    return x;
}

std::optional<TrainInfo> decode_train_info(const ByteView body) {
    TrainInfo x;
    if (!parse_body(body, [&](WireReader& r) {
            x.train = r.u32();
            x.kind = r.u16();
            x.subtype = r.u16();
            x.number = r.u32();
            x.state = r.u8();
            x.path = r.u32();
        })) {
        return std::nullopt;
    }
    return x;
}

std::optional<TrainAssignUpdate> decode_train_assign(const ByteView body) {
    TrainAssignUpdate x;
    if (!parse_body(body, [&](WireReader& r) {
            x.train = r.u32();
            x.assign = read_assign(r);
        })) {
        return std::nullopt;
    }
    return x;
}

std::optional<std::u16string> decode_message(const ByteView body) {
    // u32 n, the text and a zero; a server may pad it, so the rest is left.
    WireReader r(body);
    const auto count = r.u32();
    if (!r.ok() || count > r.remaining() / 2) return std::nullopt;
    std::u16string text(count, u'\0');
    for (auto& unit : text) unit = static_cast<char16_t>(r.u16());
    if (!r.ok()) return std::nullopt;
    while (!text.empty() && text.back() == u'\0') text.pop_back();
    return text;
}

std::optional<bool> decode_pause(const ByteView body) {
    std::uint8_t value{};
    if (!parse_body(body, [&](WireReader& r) { value = r.u8(); })) return std::nullopt;
    return value != 0;
}

std::optional<CarLight> decode_car_light(const ByteView body) {
    // train, car, light, lit; then radius, colour, position (left).
    WireReader r(body);
    CarLight x;
    x.train = r.u32();
    x.car = r.u32();
    x.light = r.u32();
    x.lit = r.u8() != 0;
    if (!r.ok()) return std::nullopt;
    return x;
}

std::optional<CarLook> decode_car_state(const ByteView body) {
    CarLook x;
    if (!parse_body(body, [&](WireReader& r) {
            x.train = r.u32();
            x.car = r.u32();
            x.kind = r.u8();
            x.flags = r.u32();
            x.value_32 = r.u32();
            x.value_14 = r.u8();
            x.pantographs = r.u8();
            x.running = r.u8() != 0;
        })) {
        return std::nullopt;
    }
    return x;
}

Bytes encode_train_pos(const std::uint32_t train, const TrainPos& pos) {
    WireWriter w;
    w.u32(train);
    w.f32(pos.speed);
    w.f32(pos.acceleration);
    w.u32(pos.node);
    w.f32(pos.offset);
    w.u8(pos.direction);
    w.u32(static_cast<std::uint32_t>(pos.cars.size()));
    for (const auto& car : pos.cars) {
        w.begin(id(Package::car_pos));
        w.f64(car.offset);
        w.f32(car.speed);
        w.f32(car.acceleration);
        w.end();
    }
    return w.take();
}

Bytes encode_train_id(const std::uint32_t train) {
    WireWriter w;
    w.u32(train);
    return w.take();
}

Bytes encode_openrts_cab_doors(const OpenrtsCabDoors& doors) {
    WireWriter w;
    w.u32(doors.train);
    w.u32(doors.car);
    w.u32(static_cast<std::uint32_t>(doors.open.size()));
    for (const auto o : doors.open) w.u8(o);
    return w.take();
}

std::optional<OpenrtsCabDoors> decode_openrts_cab_doors(const ByteView body) {
    WireReader r(body);
    OpenrtsCabDoors d;
    d.train = r.u32();
    d.car = r.u32();
    const auto count = r.u32();
    if (!r.ok() || count > 256) return std::nullopt;
    d.open.resize(count);
    for (auto& o : d.open) o = r.u8();
    if (!r.ok()) return std::nullopt;
    return d;
}

Bytes encode_openrts_doors(const OpenrtsDoors& doors) {
    WireWriter w;
    w.u32(doors.train);
    w.u32(static_cast<std::uint32_t>(doors.groups.size()));
    for (const auto g : doors.groups) w.u8(g);
    return w.take();
}

std::optional<OpenrtsDoors> decode_openrts_doors(const ByteView body) {
    WireReader r(body);
    OpenrtsDoors d;
    d.train = r.u32();
    const auto count = r.u32();
    if (!r.ok() || count > 4096) return std::nullopt;
    d.groups.resize(count);
    for (auto& g : d.groups) g = r.u8();
    if (!r.ok()) return std::nullopt;
    return d;
}

std::vector<Bytes> encode_train_state(const std::uint32_t train, const ByteView state) {
    constexpr std::size_t most = 12000;
    const auto parts = static_cast<std::uint16_t>((std::max<std::size_t>)(1, (state.size() + most - 1) / most));
    std::vector<Bytes> out;
    for (std::uint16_t p = 0; p < parts; ++p) {
        WireWriter w;
        w.u32(train);
        w.u16(p);
        w.u16(parts);
        const auto from = static_cast<std::size_t>(p) * most;
        const auto count = (std::min)(most, state.size() - (std::min)(from, state.size()));
        w.bytes(state.subspan(from, count));
        out.push_back(w.take());
    }
    return out;
}

std::optional<TrainStatePart> decode_train_state(const ByteView body) {
    WireReader r(body);
    TrainStatePart p;
    p.train = r.u32();
    p.part = r.u16();
    p.parts = r.u16();
    if (!r.ok() || p.parts == 0 || p.part >= p.parts) return std::nullopt;
    const auto rest = r.bytes(r.remaining());
    p.bytes.assign(rest.begin(), rest.end());
    return p;
}

std::optional<Derail> decode_derail(const ByteView body) {
    WireReader r(body);
    Derail d;
    d.train = r.u32();
    const auto count = r.u32();
    if (!r.ok() || count > 4096) return std::nullopt;
    d.derailed.resize(count);
    for (auto& flag : d.derailed) flag = r.u8();
    while (r.ok() && !r.at_end()) {
        const auto f = r.frame();
        if (!r.ok()) break;
        if (f.id != id(Package::derailed_car)) continue;
        WireReader c(f.body);
        DerailedCar car;
        car.car = c.u32();
        for (auto& v : car.heading) v = c.f32();
        c.f32();
        for (auto& v : car.position) v = c.f32();
        car.tip = c.f32();
        car.tip_rest = c.f32();
        car.tip_third = c.f32();
        c.f32();
        c.f32();
        car.weight = c.f32();
        c.f32();
        c.f32();
        c.u8();
        car.state = c.u32();
        if (c.ok()) d.cars.push_back(car);
    }
    if (!r.ok()) return std::nullopt;
    return d;
}

Bytes encode_derail(const Derail& derail) {
    WireWriter w;
    w.u32(derail.train);
    w.u32(static_cast<std::uint32_t>(derail.derailed.size()));
    for (const auto flag : derail.derailed) w.u8(flag);
    for (const auto& car : derail.cars) {
        // 0x4638: car, heading (3), 0 and the place in the tile (4), the
        // rolls (3), five forces (m * g the third), a u8, the state, and no
        // optional blocks (two u8 0).
        w.begin(id(Package::derailed_car));
        w.u32(car.car);
        for (const float v : car.heading) w.f32(v);
        w.f32(0.0f);
        for (const float v : car.position) w.f32(v);
        w.f32(car.tip);
        w.f32(car.tip_rest);
        w.f32(car.tip_third);
        w.f32(0.0f);
        w.f32(0.0f);
        w.f32(car.weight);
        w.f32(0.0f);
        w.f32(0.0f);
        w.u8(0);
        w.u32(car.state);
        w.u8(0);
        w.u8(0);
        w.end();
    }
    return w.take();
}

Bytes encode_car_light(const CarLightState& light) {
    WireWriter w;
    w.u32(light.train);
    w.u32(light.car);
    w.u32(light.light);
    w.u8(light.lit ? 1 : 0);
    w.f32(light.radius);
    w.u32(light.colour);
    for (const auto v : light.position) w.f32(v);
    return w.take();
}

Bytes encode_car_state(const CarLook& look) {
    WireWriter w;
    w.u32(look.train);
    w.u32(look.car);
    w.u8(look.kind);
    w.u32(look.flags);
    w.u32(look.value_32);
    w.u8(look.value_14);
    w.u8(look.pantographs);
    w.u8(look.running ? 1 : 0);
    return w.take();
}

std::optional<SoundTrigger> decode_sound_trigger(const ByteView body) {
    SoundTrigger x;
    if (!parse_body(body, [&](WireReader& r) {
            x.train = r.u32();
            x.car = r.u32();
            x.slot = r.u16();
            static_cast<void>(r.u16()); // 1
            x.code = r.u32();
            static_cast<void>(r.u32()); // 0, not read
        })) {
        return std::nullopt;
    }
    return x;
}

Bytes encode_sound_trigger(const SoundTrigger& trigger) {
    WireWriter w;
    w.u32(trigger.train);
    w.u32(trigger.car);
    w.u16(trigger.slot);
    w.u16(1);
    w.u32(trigger.code);
    w.u32(0);
    return w.take();
}

std::optional<SoundValues> decode_sound_values(const ByteView body) {
    SoundValues x;
    if (!parse_body(body, [&](WireReader& r) {
            x.train = r.u32();
            x.car = r.u32();
            x.slot = r.u16();
            for (auto& v : x.vars) v = r.f32();
            x.travelled = r.f32();
            x.volume = r.f32();
        })) {
        return std::nullopt;
    }
    return x;
}

Bytes encode_sound_values(const SoundValues& values) {
    WireWriter w;
    w.u32(values.train);
    w.u32(values.car);
    w.u16(values.slot);
    for (const auto v : values.vars) w.f32(v);
    w.f32(values.travelled);
    w.f32(values.volume);
    return w.take();
}

std::optional<Substation> decode_substation(const ByteView body) {
    Substation x;
    if (!parse_body(body, [&](WireReader& r) {
            x.id = r.u32();
            x.flags = r.u32();
            x.voltage = r.f32();
            x.tail = r.u32();
        })) {
        return std::nullopt;
    }
    return x;
}

std::optional<std::vector<SubstationPhases>> decode_phase_voltages(const ByteView body) {
    std::vector<SubstationPhases> phases;
    if (!parse_body(body, [&](WireReader& r) {
            const auto count = r.u32();
            for (std::uint32_t i = 0; i < count && r.ok(); ++i) {
                SubstationPhases p;
                p.id = r.u32();
                const auto n = r.u16();
                for (std::uint16_t k = 0; k < n && r.ok(); ++k) p.voltages.push_back(r.f32());
                phases.push_back(std::move(p));
            }
        })) {
        return std::nullopt;
    }
    return phases;
}

std::optional<SectionSource> decode_section_source(const ByteView body) {
    SectionSource x;
    if (!parse_body(body, [&](WireReader& r) {
            x.section = r.u32();
            x.source = r.i16();
        })) {
        return std::nullopt;
    }
    return x;
}

std::optional<PieceSupply> decode_piece_supply(const ByteView body) {
    PieceSupply x;
    if (!parse_body(body, [&](WireReader& r) {
            x.node = r.u32();
            x.piece = r.u32();
            x.source = r.i16();
            x.keep = r.u8();
        })) {
        return std::nullopt;
    }
    return x;
}

Bytes encode_substation_load(const SubstationLoadReport& load) {
    WireWriter w;
    w.u32(load.substation);
    w.u16(load.phase);
    w.f32(load.energy);
    w.f32(load.window);
    return w.take();
}

std::optional<SoundState> decode_sound_state(const ByteView body) {
    SoundState x;
    WireReader r(body);
    x.values.train = r.u32();
    x.values.car = r.u32();
    x.values.slot = r.u16();
    for (auto& v : x.values.vars) v = r.f32();
    x.values.travelled = r.f32();
    x.values.volume = r.f32();
    const auto count = r.u32();
    for (std::uint32_t i = 0; i < count && r.ok(); ++i) {
        const auto frame = r.frame();
        if (!r.ok() || frame.id != id(Package::sound_stream)) return std::nullopt;
        SoundStreamState s;
        if (!parse_body(frame.body, [&](WireReader& f) {
                s.flags = f.u32();
                s.current = f.u32();
                s.start = f.u32();
                s.stream_flags = f.u32();
                s.position = f.u32();
                s.volume = f.f32();
                s.value = f.f32();
                const auto n = f.u32();
                for (std::uint32_t k = 0; k < n && f.ok(); ++k) s.triggers.push_back(f.u32());
            })) {
            return std::nullopt;
        }
        x.streams.push_back(std::move(s));
    }
    if (!r.ok() || !r.at_end()) return std::nullopt;
    return x;
}

Bytes encode_sound_state(const SoundState& state) {
    WireWriter w;
    w.u32(state.values.train);
    w.u32(state.values.car);
    w.u16(state.values.slot);
    for (const auto v : state.values.vars) w.f32(v);
    w.f32(state.values.travelled);
    w.f32(state.values.volume);
    w.u32(static_cast<std::uint32_t>(state.streams.size()));
    for (const auto& s : state.streams) {
        w.begin(id(Package::sound_stream));
        w.u32(s.flags);
        w.u32(s.current);
        w.u32(s.start);
        w.u32(s.stream_flags);
        w.u32(s.position);
        w.f32(s.volume);
        w.f32(s.value);
        w.u32(static_cast<std::uint32_t>(s.triggers.size()));
        for (const auto t : s.triggers) w.u32(t);
        w.end();
    }
    return w.take();
}

std::optional<Coupling> decode_couple(const ByteView body) {
    Coupling x;
    if (!parse_body(body, [&](WireReader& r) {
            x.a = r.u32();
            x.b = r.u32();
            x.end_a = r.u8();
            x.end_b = r.u8();
        })) {
        return std::nullopt;
    }
    return x;
}

Bytes encode_couple(const Coupling& coupling) {
    WireWriter w;
    w.u32(coupling.a);
    w.u32(coupling.b);
    w.u8(coupling.end_a);
    w.u8(coupling.end_b);
    return w.take();
}

std::optional<Uncoupling> decode_uncouple(const ByteView body) {
    Uncoupling x;
    if (!parse_body(body, [&](WireReader& r) {
            x.train = r.u32();
            x.car = r.u32();
            x.part = r.u32();
            x.side = r.u8();
        })) {
        return std::nullopt;
    }
    return x;
}

Bytes encode_uncouple(const Uncoupling& cut) {
    WireWriter w;
    w.u32(cut.train);
    w.u32(cut.car);
    w.u32(cut.part);
    w.u8(cut.side);
    return w.take();
}

std::optional<std::pair<std::uint32_t, std::uint32_t>> decode_train_id_change(const ByteView body) {
    std::pair<std::uint32_t, std::uint32_t> x;
    if (!parse_body(body, [&](WireReader& r) {
            x.first = r.u32();
            x.second = r.u32();
        })) {
        return std::nullopt;
    }
    return x;
}

std::vector<TrainCar> coupled_cars(const std::vector<TrainCar>& a, const std::vector<TrainCar>& b, const std::uint8_t end_a,
                                   const std::uint8_t end_b) {
    // B turned when the same ends meet (rear to rear, front to front).
    auto other = b;
    if (end_a == end_b) {
        std::reverse(other.begin(), other.end());
        for (auto& car : other) car.flags ^= 1u;
    }
    std::vector<TrainCar> out;
    if (end_a == 0) {
        out = a; // A's rear: B behind it
        out.insert(out.end(), other.begin(), other.end());
    } else {
        out = std::move(other); // A's front: B ahead of it
        out.insert(out.end(), a.begin(), a.end());
    }
    return out;
}

std::pair<std::vector<TrainCar>, std::vector<TrainCar>> cut_cars(const std::vector<TrainCar>& cars, const std::size_t car,
                                                                 const std::uint8_t side) {
    const auto at = cars.begin() + static_cast<std::ptrdiff_t>((std::min)(car, cars.size()));
    std::vector<TrainCar> front(cars.begin(), at), rear(at, cars.end());
    if (side == 0) return {std::move(front), std::move(rear)};
    return {std::move(rear), std::move(front)};
}

Bytes encode_state_request(const std::uint32_t mode, const std::uint32_t train) {
    WireWriter w;
    w.u32(mode);
    w.u32(train);
    w.u32(0);
    w.u32(0);
    return w.take();
}

Bytes encode_console_state(const ConsoleState& s) {
    WireWriter w;
    w.f32(s.time);
    w.u8(s.paused ? 1 : 0);
    w.u8(s.admits ? 1 : 0);
    w.u32(static_cast<std::uint32_t>(s.users.size()));
    for (const auto& u : s.users) {
        w.u32(u.id);
        w.text_u32(u.nick);
        w.u8(static_cast<std::uint8_t>((u.connected ? 1 : 0) | (u.playing ? 2 : 0) | (u.console ? 4 : 0) | (u.placed ? 8 : 0)));
        w.f32(u.camera_x);
        w.f32(u.camera_z);
        w.u32(static_cast<std::uint32_t>(u.trains.size()));
        for (const auto t : u.trains) w.u32(t);
    }
    w.u32(static_cast<std::uint32_t>(s.trains.size()));
    for (const auto& t : s.trains) {
        w.u32(t.id);
        w.u32(t.number);
        w.u16(t.kind);
        w.u16(t.subtype);
        w.u8(t.state);
        w.u32(t.owner);
        w.u32(t.route_junctions);
        w.f32(t.speed);
        w.u8(t.stands ? 1 : 0);
        w.u32(t.cars);
        w.text_u32(t.first_car);
    }
    w.u32(static_cast<std::uint32_t>(s.segments.size()));
    for (const auto& g : s.segments) {
        w.u32(g.id);
        w.u32(g.train);
        w.u32(g.route);
        w.text_u32(g.station);
        w.text_u32(g.name);
        w.u8(static_cast<std::uint8_t>((g.reverse ? 1 : 0) | (g.hold ? 2 : 0) | (g.reversed ? 4 : 0)));
        w.u32(g.priority);
        w.u8(g.state);
        w.u16(g.thrown);
        w.u32(g.junctions);
    }
    return w.take();
}

std::optional<ConsoleState> decode_console_state(const ByteView body) {
    ConsoleState s;
    const bool ok = parse_body(body, [&](WireReader& r) {
        s.time = r.f32();
        s.paused = r.u8() != 0;
        s.admits = r.u8() != 0;
        const auto users = r.u32();
        for (std::uint32_t i = 0; i < users && r.ok() && i < 4096; ++i) {
            ConsoleUser u;
            u.id = r.u32();
            u.nick = r.text_u32();
            const auto flags = r.u8();
            u.connected = (flags & 1) != 0;
            u.playing = (flags & 2) != 0;
            u.console = (flags & 4) != 0;
            u.placed = (flags & 8) != 0;
            u.camera_x = r.f32();
            u.camera_z = r.f32();
            const auto count = r.u32();
            for (std::uint32_t k = 0; k < count && r.ok() && k < 4096; ++k) u.trains.push_back(r.u32());
            s.users.push_back(std::move(u));
        }
        const auto trains = r.u32();
        for (std::uint32_t i = 0; i < trains && r.ok() && i < 65536; ++i) {
            ConsoleTrain t;
            t.id = r.u32();
            t.number = r.u32();
            t.kind = r.u16();
            t.subtype = r.u16();
            t.state = r.u8();
            t.owner = r.u32();
            t.route_junctions = r.u32();
            t.speed = r.f32();
            t.stands = r.u8() != 0;
            t.cars = r.u32();
            t.first_car = r.text_u32();
            s.trains.push_back(std::move(t));
        }
        if (r.at_end()) return; // a server before 2026-11-21: no segments
        const auto segments = r.u32();
        for (std::uint32_t i = 0; i < segments && r.ok() && i < 65536; ++i) {
            ConsoleSegment g;
            g.id = r.u32();
            g.train = r.u32();
            g.route = r.u32();
            g.station = r.text_u32();
            g.name = r.text_u32();
            const auto flags = r.u8();
            g.reverse = (flags & 1) != 0;
            g.hold = (flags & 2) != 0;
            g.reversed = (flags & 4) != 0;
            g.priority = r.u32();
            g.state = r.u8();
            g.thrown = r.u16();
            g.junctions = r.u32();
            s.segments.push_back(std::move(g));
        }
    });
    if (!ok) return std::nullopt;
    return s;
}

Bytes encode_console_next_ask(const ConsoleNextAsk& ask) {
    WireWriter w;
    w.u32(ask.train);
    w.u8(ask.back ? 1 : 0);
    return w.take();
}

std::optional<ConsoleNextAsk> decode_console_next_ask(const ByteView body) {
    ConsoleNextAsk ask;
    const bool ok = parse_body(body, [&](WireReader& r) {
        ask.train = r.u32();
        ask.back = r.u8() != 0;
    });
    if (!ok) return std::nullopt;
    return ask;
}

Bytes encode_console_next(const ConsoleNext& n) {
    WireWriter w;
    w.u32(n.train);
    w.u8(static_cast<std::uint8_t>((n.back ? 1 : 0) | (n.found ? 2 : 0) | (n.rear_first ? 4 : 0)));
    w.u32(n.after);
    w.text_u32(n.after_name);
    w.u32(static_cast<std::uint32_t>(n.routes.size()));
    for (const auto& c : n.routes) {
        w.u32(c.route);
        w.u8(static_cast<std::uint8_t>((c.reverse ? 1 : 0) | (c.main_track ? 2 : 0) | (c.wrong_way ? 4 : 0) | (c.platform ? 8 : 0)));
        w.f32(c.track_length);
        w.f32(c.distance);
        w.text_u32(c.station);
        w.text_u32(c.name);
    }
    return w.take();
}

std::optional<ConsoleNext> decode_console_next(const ByteView body) {
    ConsoleNext n;
    const bool ok = parse_body(body, [&](WireReader& r) {
        n.train = r.u32();
        const auto flags = r.u8();
        n.back = (flags & 1) != 0;
        n.found = (flags & 2) != 0;
        n.rear_first = (flags & 4) != 0;
        n.after = r.u32();
        n.after_name = r.text_u32();
        const auto count = r.u32();
        for (std::uint32_t i = 0; i < count && r.ok() && i < 65536; ++i) {
            ConsoleRouteChoice c;
            c.route = r.u32();
            const auto f = r.u8();
            c.reverse = (f & 1) != 0;
            c.main_track = (f & 2) != 0;
            c.wrong_way = (f & 4) != 0;
            c.platform = (f & 8) != 0;
            c.track_length = r.f32();
            c.distance = r.f32();
            c.station = r.text_u32();
            c.name = r.text_u32();
            n.routes.push_back(std::move(c));
        }
    });
    if (!ok) return std::nullopt;
    return n;
}

Bytes encode_console_place(const ConsolePlace& p) {
    WireWriter w;
    w.text_u32(p.consist);
    w.f32(p.max_velocity);
    w.u32(p.number);
    w.text_u32(p.path);
    w.u32(p.node);
    w.f64(p.offset);
    w.u8(p.forward ? 1 : 0);
    w.u32(static_cast<std::uint32_t>(p.cars.size()));
    for (const auto& car : p.cars) {
        w.text_u32(car.folder);
        w.text_u32(car.name);
        w.u8(static_cast<std::uint8_t>((car.engine ? 1 : 0) | (car.flipped ? 2 : 0) | (car.cold ? 4 : 0) | ((car.chocks & 3u) << 3)));
        w.f32(car.handbrake);
    }
    w.text_u32(p.name);
    w.u16(p.kind);
    w.u8(p.state);
    w.text_u32(p.notes);
    return w.take();
}

Bytes encode_openrts_weather(const OpenrtsWeather& w) {
    WireWriter out;
    out.u8(w.dynamic);
    out.u8(w.weather);
    out.u32(static_cast<std::uint32_t>(w.seed));
    out.u32(static_cast<std::uint32_t>(w.seed >> 32));
    out.f64(w.clock);
    out.u8(w.by_season);
    out.f32(w.cloudiness);
    out.f32(w.wet);
    out.f32(w.storms);
    out.f32(w.spell_minutes);
    out.f32(w.change_minutes);
    return out.take();
}

std::optional<OpenrtsWeather> decode_openrts_weather(const ByteView body) {
    OpenrtsWeather w;
    const bool ok = parse_body(body, [&](WireReader& r) {
        w.dynamic = r.u8();
        w.weather = r.u8();
        const std::uint64_t low = r.u32();
        const std::uint64_t high = r.u32();
        w.seed = low | (high << 32);
        w.clock = r.f64();
        w.by_season = r.u8();
        w.cloudiness = r.f32();
        w.wet = r.f32();
        w.storms = r.f32();
        w.spell_minutes = r.f32();
        w.change_minutes = r.f32();
    });
    if (!ok) return std::nullopt;
    return w;
}

std::optional<ConsolePlace> decode_console_place(const ByteView body) {
    ConsolePlace p;
    const bool ok = parse_body(body, [&](WireReader& r) {
        p.consist = r.text_u32();
        p.max_velocity = r.f32();
        p.number = r.u32();
        p.path = r.text_u32();
        p.node = r.u32();
        p.offset = r.f64();
        p.forward = r.u8() != 0;
        const auto count = r.u32();
        for (std::uint32_t i = 0; i < count && r.ok() && i < 1024; ++i) {
            ConsoleCar car;
            car.folder = r.text_u32();
            car.name = r.text_u32();
            const auto flags = r.u8();
            car.engine = (flags & 1) != 0;
            car.flipped = (flags & 2) != 0;
            car.cold = (flags & 4) != 0;
            car.chocks = static_cast<std::uint8_t>((flags >> 3) & 3u);
            car.handbrake = r.f32();
            p.cars.push_back(std::move(car));
        }
        if (r.ok() && !r.at_end()) {
            p.name = r.text_u32();
            p.kind = r.u16();
            p.state = r.u8();
            if (!r.at_end()) p.notes = r.text_u32();
        }
    });
    if (!ok || p.cars.empty()) return std::nullopt;
    return p;
}

Bytes encode_player_pos(const PlayerPosition& p) {
    WireWriter w;
    w.u32(p.value_0);
    w.u32(p.train);
    w.u32(p.value_8);
    w.i16(p.tile_x);
    w.i16(p.tile_z);
    w.f32(p.x);
    w.f32(p.y);
    w.f32(p.z);
    for (const auto v : p.view) w.f32(v);
    return w.take();
}

std::optional<OtherPlayer> decode_other_player(const ByteView body) {
    // u32 user, u32 train, u32, i16 tile x, i16 tile z, f32 x y z, 16 bytes
    // of view, then the name as UTF-16 up to its first zero (rts_server.exe
    // pads it; the snapshot's records have 32 bytes of it).
    WireReader r(body);
    OtherPlayer p;
    p.user = r.u32();
    p.train = r.u32();
    r.u32();
    p.tile_x = r.i16();
    p.tile_z = r.i16();
    p.x = r.f32();
    p.y = r.f32();
    p.z = r.f32();
    r.skip(16);
    if (!r.ok()) return std::nullopt;
    while (r.remaining() >= 2) {
        const auto unit = static_cast<char16_t>(r.u16());
        if (unit == u'\0') break;
        p.nick.push_back(unit);
    }
    return p;
}

Bytes encode_openrts_hello(const std::uint16_t features, const bool storm, const std::optional<OpenrtsDiscord> discord) {
    WireWriter w;
    w.u16(features);
    w.u8(storm ? 1 : 0);
    if (discord) {
        w.u8(1);
        w.text_u16(discord->address);
        w.u16(discord->players);
        w.u16(discord->places);
    }
    return w.take();
}

std::optional<OpenrtsDiscord> openrts_hello_discord(const ByteView body) {
    WireReader r(body);
    (void)r.u16();
    (void)r.u8();
    if (!r.ok() || r.at_end() || r.u8() != 1) return std::nullopt;
    OpenrtsDiscord discord;
    discord.address = r.text_u16();
    discord.players = r.u16();
    discord.places = r.u16();
    if (!r.ok()) return std::nullopt;
    return discord;
}

bool openrts_hello_storm(const ByteView body) {
    WireReader r(body);
    (void)r.u16();
    const auto storm = r.u8();
    return r.ok() && storm == 1;
}

std::optional<std::uint16_t> decode_openrts_hello(const ByteView body) {
    // Later versions may add to it.
    WireReader r(body);
    const auto features = r.u16();
    if (!r.ok()) return std::nullopt;
    return features;
}

Bytes encode_helper_note(const HelperNote& n) {
    WireWriter w;
    w.u8(static_cast<std::uint8_t>(n.kind));
    w.u32(n.train);
    w.u32(n.user);
    w.u8(n.value);
    w.text_u32(n.nick);
    return w.take();
}

std::optional<HelperNote> decode_helper_note(const ByteView body) {
    HelperNote n;
    const bool ok = parse_body(body, [&](WireReader& r) {
        n.kind = static_cast<HelperKind>(r.u8());
        n.train = r.u32();
        n.user = r.u32();
        n.value = r.u8();
        n.nick = r.text_u32();
    });
    const auto kind = static_cast<std::uint8_t>(n.kind);
    if (!ok || kind < 1 || kind > 6 || n.nick.size() > 64) return std::nullopt;
    return n;
}

Bytes encode_helper_cab(const HelperCab& c) {
    WireWriter w;
    w.u32(c.train);
    w.u32(c.car);
    w.u32(static_cast<std::uint32_t>(c.packed.size()));
    w.bytes(c.packed);
    return w.take();
}

std::optional<HelperCab> decode_helper_cab(const ByteView body) {
    HelperCab c;
    const bool ok = parse_body(body, [&](WireReader& r) {
        c.train = r.u32();
        c.car = r.u32();
        const auto size = r.u32();
        const auto bytes = r.bytes(size);
        c.packed.assign(bytes.begin(), bytes.end());
    });
    if (!ok) return std::nullopt;
    return c;
}

Bytes encode_helper_input(const HelperInput& i) {
    WireWriter w;
    w.u8(static_cast<std::uint8_t>(i.kind));
    w.u32(i.train);
    w.u32(i.car);
    w.u32(i.id);
    w.u32(i.state);
    w.f32(i.delta);
    w.u8(i.release ? 1 : 0);
    return w.take();
}

std::optional<HelperInput> decode_helper_input(const ByteView body) {
    HelperInput i;
    const bool ok = parse_body(body, [&](WireReader& r) {
        i.kind = static_cast<HelperInputKind>(r.u8());
        i.train = r.u32();
        i.car = r.u32();
        i.id = r.u32();
        i.state = r.u32();
        i.delta = r.f32();
        i.release = r.u8() != 0;
    });
    const auto kind = static_cast<std::uint8_t>(i.kind);
    if (!ok || kind < 1 || kind > 3 || !std::isfinite(i.delta)) return std::nullopt;
    return i;
}

double front_distance(const SnapshotTrain& train) {
    if (train.cars.empty()) return 0.0;
    if (train.pos && !train.pos->cars.empty()) {
        return train.pos->cars.front().offset + 0.5 * train.cars.front().length;
    }
    double length = 0.0;
    for (const auto& car : train.cars) length += car.length;
    return length;
}

} // namespace openrts::net
