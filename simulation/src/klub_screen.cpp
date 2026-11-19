#include <openrts/simulation/klub_screen.hpp>

#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>

// updateKLUB's drawing for the КЛУБ-У (docs/reverse/brakes-dll.md §11,
// "Its screen"), in its order; the strings as it formats them.
namespace openrts::simulation {

namespace {

namespace f = klub_flag;
namespace d = klub_device;
using Obj = KlubScreenObject;

void hide(Obj& o) {
    o.state = -1;
    o.state_set = true;
}
void show(Obj& o, const std::int32_t state = 0) {
    o.state = state;
    o.state_set = true;
}
void text(Obj& o, std::u16string t) {
    o.text = std::move(t);
    o.text_set = true;
}
void value(Obj& o, const float v) {
    o.value = v;
    o.value_set = true;
}
void blink(Obj& o, const float period) {
    o.blink = period;
    o.blink_set = true;
}

std::u16string ascii(const char* format, ...) {
    char buffer[64];
    va_list args;
    va_start(args, format);
    std::vsnprintf(buffer, sizeof buffer, format, args);
    va_end(args);
    std::u16string out;
    for (const char* c = buffer; *c; ++c) out.push_back(static_cast<char16_t>(static_cast<unsigned char>(*c)));
    return out;
}

// CharUpperW for the names: Latin and Cyrillic.
std::u16string upper(std::u16string s) {
    for (auto& c : s) {
        if (c >= u'a' && c <= u'z') c = static_cast<char16_t>(c - 32);
        else if (c >= u'а' && c <= u'я') c = static_cast<char16_t>(c - 32);
        else if (c == u'ё') c = u'Ё';
    }
    return s;
}

bool starts(const std::u16string& s, const std::u16string_view with) { return s.size() >= with.size() && std::u16string_view(s).substr(0, with.size()) == with; }

// ChangeStanc.bto: the first entry the name starts with.
const std::pair<std::u16string, std::u16string>* renamed(const KlubScreenInputs& in, const std::u16string& name) {
    for (const auto& entry : in.station_names)
        if (starts(name, entry.first)) return &entry;
    return nullptr;
}
std::u16string shown_name(const KlubScreenInputs& in, const std::u16string& name) {
    const auto* entry = renamed(in, name);
    return entry ? entry->second : name;
}
bool skipped(const KlubScreenInputs& in, const std::u16string& name) {
    const auto* entry = renamed(in, name);
    return entry && starts(entry->second, u"SKIP");
}
bool a_stop(const std::u16string& name) { return starts(name, u"ПЛ.") || starts(name, u"О.П.") || starts(name, u"ОП "); }

std::uint32_t whole(const double x) { return static_cast<std::uint32_t>(static_cast<std::int64_t>(x)); }

} // namespace

void draw_klub(const KlubState& s, const KlubSettings& k, const KlubScreenInputs& in, KlubScreen& screen) {
    auto& b = screen.bil;
    if (b.size() < 45) b.resize(45);
    screen.drawn = true;
    const bool redraw = s.redraw == 0.0f;
    const std::uint32_t aspect = s.signal.aspect[0];
    screen.lamp = 0;
    screen.backlight = 0;
    // Cleared at every redraw, and every step while unpowered.
    if (!in.powered || redraw) {
        for (int j = 0; j < 14; ++j) hide(b[j]);
        for (int j = 29; j < 34; ++j) hide(b[j]);
        for (int j = 14; j < 29; ++j)
            if (j != 25) text(b[j], u"");
        text(b[43], u"");
        text(b[44], u"");
        text(b[34], u"");
        for (auto& o : screen.dpom) hide(o);
        for (auto& o : screen.dals) hide(o);
        hide(b[25]);
        screen.lamps.fill(0);
    }
    if (!in.powered) return;
    screen.lamp = 1;
    screen.backlight = (s.flags & f::backlight) ? 1 : 0;
    // The lines 21 / 22 (kind, name or value) and the message.
    std::u16string kind, what, message = u" ", message_value, signal_name;
    if (!(s.track & 0x7F)) message = u" РЕЖИМ БЕЗ ЭК";
    float next = 100000.0f, signal_at = 0.0f;
    if (in.epk_key && (s.track & 0x7F)) {
        if (!in.signal_name.empty()) what = signal_name = in.signal_name;
        kind = u"СВЕТОФОР";
        next = s.signal_metres;
        signal_at = next;
    }
    // Line 27: the limit in tens, then the station.
    if (s.powered_time >= 20.0f)
        text(b[27], ascii("%d", static_cast<int>(10 * static_cast<std::int64_t>(0.1 * static_cast<double>(s.limit)))) + u"КМ.Ч");
    const std::uint16_t ahead = (s.device & d::reversed) ? 2 : 1;
    for (const auto& item : in.items) {
        if (!(item.pos & ahead) || item.distance > 7000.0f) continue;
        if (item.type == 3) {
            const auto name = upper(item.name);
            if (!a_stop(name) && !skipped(in, name)) {
                if (s.powered_time >= 20.0f) text(b[27], shown_name(in, name));
                break;
            }
        } else if (item.type == 4 && !starts(item.name, u"%")) {
            if (s.powered_time >= 20.0f) text(b[27], shown_name(in, upper(item.name)));
            break;
        }
    }
    for (const auto& item : in.items) {
        if (!(item.pos & 4) || item.distance > 4000.0f) continue;
        if (item.type == 3) {
            const auto name = upper(item.name);
            if (!a_stop(name) && !skipped(in, name) && s.powered_time >= 20.0f) text(b[27], shown_name(in, name));
            break;
        }
        if (item.type == 4 && !starts(item.name, u"%")) {
            if (s.powered_time >= 20.0f) text(b[27], shown_name(in, upper(item.name)));
            break;
        }
    }
    if (!(s.track & 0x7F)) text(b[27], u"");
    // The next object within 500 m (an odd track number: its bit 0).
    if ((s.track & 1) && in.epk_key) {
        bool first = true;
        for (std::size_t i = 0; i < in.items.size(); ++i) {
            const auto& item = in.items[i];
            if (!(item.pos & ahead) || item.distance > 4000.0f) continue;
            if (item.type == 3) {
                const auto name = upper(item.name);
                if ((item.distance < next || first) && item.distance <= 500.0f) {
                    // Its other end: the same name at another distance.
                    bool pair = false;
                    for (std::size_t j = 0; j < in.items.size() && !pair; ++j) {
                        const auto& other = in.items[j];
                        if (j == i || other.type != 3 || !(other.pos & ahead) || other.distance > 4000.0f) continue;
                        pair = other.distance != item.distance && upper(other.name) == name;
                    }
                    if (pair) {
                        next = item.distance;
                        kind = u"ПЛАТФОРМА";
                        if (starts(name, u"ПЛ. ")) what = name.substr(4);
                        else if (starts(name, u"ПЛ.")) what = name.substr(3);
                        else if (starts(name, u"О.П. ")) what = name.substr(5);
                        else if (starts(name, u"О.П.")) what = name.substr(4);
                        else if (starts(name, u"ОП ")) what = name.substr(3);
                        else what = name;
                        first = false;
                    }
                }
            }
            if (item.type == 5 && (item.distance < next || first) && item.distance <= 500.0f) {
                kind = u"ПЕРЕЕЗД";
                what = ascii("%d", static_cast<int>(static_cast<std::int64_t>(in.milepost))) + u" КМ";
                next = item.distance;
                first = false;
            }
            if (item.type == 2 && (item.flags & 6) && first && item.distance <= 500.0f) {
                kind = u"ОГРАНИЧ. СКОР.";
                what = ascii("%d", static_cast<int>(static_cast<std::int64_t>(static_cast<double>(item.value) * 3.6) + 1));
                next = item.distance;
                first = false;
            }
        }
    }
    // Messages over lines 21 / 22.
    if (s.message_time < 2.0f) {
        what.clear();
        switch (s.entry) {
        case 10: message = u"ПРОШИВКА 5.5.5"; break;
        case 123: message = u"ДАВЛЕНИЕ В КГС"; break;
        case 122: message = u"ДАВЛЕНИЕ В МПА"; break;
        case 82: case 83: message = u"ДАВЛЕНИЕ В УР"; break;
        case 81: message = u"ДАВЛЕНИЕ В ТЦ"; break;
        case 80: message = u"ДАВЛЕНИЕ В ТМ"; break;
        case 45: message = in.working_cab ? u"КАБИНА 1" : u"КАБИНА 2"; break;
        case 46: message = (s.flags & f::second_set) ? u"КОМПЛЕКТ 2" : u"КОМПЛЕКТ 1"; break;
        case 47: message = (s.flags & f::dps) ? u"АКТ. ДАТЧИК 1" : u"АКТ. ДАТЧИК 2"; break;
        case 1000:
            if (s.limit_marks & 0x40000)
                message = ascii("%d - %d", static_cast<int>(whole(s.rise_run)), static_cast<int>(s.wagons * (k.ept ? 24u : 14u)))
                              .insert(0, u"ДИСТ. ");
            break;
        case 1036: message = (s.flags & f::dps) ? u"ПЕРЕХ. НА ДПС 1" : u"ПЕРЕХ. НА ДПС 2"; break;
        case 1037: case 1038: message = u"ПРОБЕГ " + ascii("%d", static_cast<int>(whole(0.001 * s.run))) + u"КМ"; break;
        case 3740: message = (s.flags & f::developer) ? u"РАЗРАБ. ВКЛ" : u"РАЗРАБ. ВЫКЛ"; break;
        default: break;
        }
    }
    if (s.flags & f::lamp_test) {
        message = std::u16string(u"    1234657") + ((s.device & d::saut_on) ? u"8" : u"-") + u"9А" + ((s.device & d::tskbm_on) ? u"В" : u"-");
        message_value.clear();
    }
    if ((s.flags & f::tskbm) && std::abs(in.speed) >= 0.1f) {
        message = u"СРЫВ ЭПК ТСКБМ";
        what.clear();
    }
    if (s.device & d::kon_vented) {
        message = u"СРЫВ ЭПК КОН";
        what.clear();
    }
    if (s.flags & f::white_entry) {
        message = u"СКОР. НА БЕЛЫЙ";
        message_value = ascii("%d", static_cast<int>(s.entry));
    }
    if (s.entry == 259) {
        message = u"ДИАГ.САУТ ВКЛ.";
        message_value.clear();
    }
    if (s.flags & f::command_mode) {
        message = u"ВВЕДИТЕ КОМАНДУ";
        message_value = ascii("%d", static_cast<int>(s.entry));
    }
    if (s.flags & f::track_entry) {
        message = u"НОМЕР ПУТИ";
        message_value = ascii("%d", static_cast<int>(s.entry));
    }
    if (s.flags & f::right_entry) {
        message = u"ПРИЗН. ПРАВ.";
        message_value = ascii("%d", static_cast<int>(s.entry));
    }
    static constexpr const char16_t* fields[] = {u"НОМЕР МАШИНИСТА", u"НОМЕР ПОЕЗДА", u"ДЛИНА В ОСЯХ", u"ДЛИНА В ВАГОНАХ", u"МАССА ПОЕЗДА"};
    if (s.field >= 1 && s.field <= 5) {
        message = fields[s.field - 1];
        message_value = ascii("%d", static_cast<int>(s.entry));
    }
    if (s.alsen_table == 5) {
        message = u"ТАБЛИЦА АЛСЕН:";
        message_value = ascii("%d", static_cast<int>(s.entry));
    }
    if (s.slip_time > 0.3f) {
        message = u"  БОКСОВАНИЕ";
        message_value.clear();
    }
    if (s.time_shown > -1.0f) {
        const auto t = static_cast<std::int64_t>(s.time_shown);
        message = u"ВРЕМЯ (ЧЧММСС)";
        message_value = ascii("%d%02d%02d", static_cast<int>(t / 3600), static_cast<int>(std::abs((t - 3600 * (t / 3600)) / 60)),
                              static_cast<int>(std::abs(t - 3600 * (t / 3600) - 60 * ((t - 3600 * (t / 3600)) / 60))));
    }
    if (!redraw) return;
    // The redraw (the КЛУБ-У's objects; the БЛОК's not drawn). The speed as
    // updateKLUB has it this step (whole km/h; + 20 a second of slip).
    std::uint32_t shown = whole(std::abs(static_cast<double>(in.speed) * 3.6));
    if (in.wheelslip) shown = static_cast<std::uint32_t>(static_cast<std::int64_t>(static_cast<float>(shown) + 20.0f * s.slip_time));
    if (message[0] != u' ' || (s.flags & f::lamp_test) || s.slip_time > 0.3f || !(s.track & 0x7F)) {
        kind = message;
        what = message_value;
    }
    text(b[23], ascii("%03d", static_cast<int>(static_cast<std::int64_t>(s.shown_speed))));
    show(b[25]);
    value(b[25], static_cast<float>(whole(s.shown_speed <= 0.0f ? 1.0f : s.shown_speed)));
    if (s.track & 0x7F) {
        text(b[16], u"ЭК");
        text(b[17], ascii("%d", static_cast<int>(s.track & 0x7F)) + ((s.track & 0x80) ? u"ПР" : u"НП"));
    } else {
        text(b[16], (s.alsen_table && s.alsen_table < 5) ? u"ЕН" : in.line_frequency == 0.0f ? u"50ГЦ" : u"25ГЦ");
        text(b[17], u"0");
    }
    if ((s.device & d::saut_on) && (k.setup & 1) && !(s.flags & f::white_799) && (!(s.flags & f::white_809) || aspect != 2) &&
        (s.flags & f::train_mode) && s.saut_rules)
        text(b[16], u"С");
    if (!s.alsen_table || s.alsen_table >= 5) {
        text(b[34], u"Х       ");
    } else {
        if (aspect == 4 || aspect == 5 || aspect == 6) show(b[1]);
        else show(b[0]);
        static constexpr const char16_t* counts[] = {u"1ЕН     ", u"2ЕН     ", u"3ЕН     ", u"4ЕН     "};
        text(b[34], counts[(std::min)(s.alsen_table, 4u) - 1]);
    }
    if (in.working_cab) {
        text(b[21], kind);
        text(b[22], what);
        if (in.epk_key && (s.track & 0x7F) && next != signal_at) {
            text(b[43], signal_name);
            text(b[44], ascii("%1.0f", static_cast<double>(signal_at)) + u" М");
        }
    }
    if (in.epk_key && in.working_cab) {
        blink(b[23], 0.0f);
        text(b[24], ascii("%03.0f", static_cast<double>(s.permitted)));
        if (static_cast<double>(shown) > 0.1) {
            if (static_cast<float>(in.direction) * in.speed > 0.0f) show(b[30]);
            else if (static_cast<float>(in.direction) * in.speed < 0.0f) show(b[31]);
        }
        if (!(s.target_speed >= s.permitted || s.target_distance >= 2000.0f))
            show(b[33], static_cast<std::int32_t>(static_cast<std::int64_t>(0.2 * static_cast<double>(s.target_speed))));
        show(b[32], static_cast<std::int32_t>(static_cast<std::int64_t>(0.2 * static_cast<double>(s.permitted))));
        if (s.drawn_flags & f::vigilance) show(b[29]);
        if (static_cast<double>(s.permitted) - shown <= 3.0 && shown) blink(b[23], 0.5f);
        else blink(b[23], 0.0f);
    } else {
        blink(b[23], 0.5f);
    }
    // The coordinate, km with three decimals, the integer part of four digits.
    {
        const double km = static_cast<double>(in.milepost - 1.0f);
        const double at = km >= 0.0 ? km : 0.0;
        const char* zeros = km < 10.0 ? "000" : km < 100.0 ? "00" : km < 1000.0 ? "0" : "";
        text(b[28], ascii("%s%0.3f", zeros, at));
    }
    {
        const auto t = static_cast<std::int64_t>(in.day_time);
        const auto h = t / 3600, m = std::abs((t % 3600) / 60), sec = std::abs(t - 3600 * h - 60 * m);
        text(b[26], ascii("%02d.%02d.%02d", static_cast<int>(h), static_cast<int>(m), static_cast<int>(sec)));
    }
    text(b[18], ascii("%0.1f", static_cast<double>(std::abs(in.acceleration))));
    if ((k.setup & 1) && in.saut_coefficient >= 0.0f && in.working_cab) text(b[19], ascii("%0.2f", static_cast<double>(in.saut_coefficient)));
    if (in.working_cab) {
        if (!(s.flags & f::command_mode) && !s.field && s.alsen_table != 5 && (s.track & 0x7F) && s.time_shown == -1.0f && !in.wheelslip)
            text(b[20], next >= 10000.0f ? std::u16string() : ascii("%04.0f", static_cast<double>(next)));
        if (s.flags & f::kgf) text(b[15], ascii("0%0.1f", static_cast<double>(in.equalising)));
        else text(b[15], ascii("%0.2f", 0.1 * static_cast<double>(in.equalising)));
        const float line14 = (s.flags & f::line14_cylinder) ? in.cylinder : in.pipe;
        if (s.flags & f::kgf) text(b[14], ascii("0%0.1f", static_cast<double>(line14)));
        else text(b[14], ascii("%0.2f", static_cast<double>(line14) * 0.1));
    }
    // The ALSN's lights.
    if (in.epk_key && in.working_cab) {
        const bool en = s.alsen_table && s.alsen_table < 5;
        const auto lights = [&](auto&& light) {
            switch (aspect) {
            case 0: case 8: light(6, 8, 7); break;
            case 1: light(5, 7, 6); break;
            case 2: light(7, 9, 8); break;
            case 3: case 4: case 5: light(4, 6, 5); break;
            case 6:
                if (en) light(4, 6, 5);
                light(3, 5, 4);
                break;
            case 7:
                light(3, 5, 4);
                if (en) {
                    light(4, 6, 5);
                    if (s.alsen_count >= 2) light(2, 4, 3);
                    if (s.alsen_count >= 3) light(1, 3, 2);
                    if (s.alsen_count >= 4) light(0, 2, 1);
                }
                break;
            default: break;
            }
        };
        // (KLUB_DALS / KLUB_DPOM object, KLUB_DBIL object, KLUB_B<n>POM)
        lights([&](const int dals, const int bil, const int lamp) {
            show(b[static_cast<std::size_t>(bil)]);
            show(screen.dpom[static_cast<std::size_t>(dals)]);
            show(screen.dals[static_cast<std::size_t>(dals)]);
            screen.lamps[static_cast<std::size_t>(lamp - 1)] = 1;
        });
    }
    if (s.limit_marks & 0x100) show(b[13]);
    if (s.flags & f::train_mode) show(b[11]);
    else show(b[10]);
    blink(b[11], (s.flags & f::double_traction) ? 0.5f : 0.0f);
    if (in.cassette_given && in.cassette) show(b[12]);
}

} // namespace openrts::simulation
