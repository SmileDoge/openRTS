#include "activity_view.hpp"

#include <algorithm>
#include <iomanip>
#include <sstream>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <limits>

#ifdef OPENRTS_HAS_IMGUI
#include <imgui.h>
#endif

#include <openrts/formats/signal_config.hpp>

#include "hud_style.hpp"
#include "ui_text.hpp"

namespace openrts::viewer {
namespace {

constexpr float hint_seconds = 12.0f;
constexpr float notice_seconds = 6.0f;
constexpr double forever = std::numeric_limits<double>::infinity();

[[nodiscard]] std::string clock_text(const double time_of_day) {
    const auto minutes = static_cast<long>(std::floor(time_of_day / 60.0));
    char text[16];
    std::snprintf(text, sizeof text, "%02ld:%02ld", (minutes / 60) % 24, minutes % 60);
    return text;
}

[[nodiscard]] std::string lower(std::string text) {
    for (auto& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

[[nodiscard]] bool truthy(const std::string& value) { return !value.empty() && value != "0"; }

[[nodiscard]] double number(const formats::ActivityNode* node, const std::size_t index, const double fallback = 0.0) {
    if (!node || index >= node->values.size()) return fallback;
    char* end = nullptr;
    const double value = std::strtod(node->values[index].c_str(), &end);
    return end == node->values[index].c_str() ? fallback : value;
}

[[nodiscard]] std::optional<formats::ActivityPoint> point(const formats::ActivityNode* node) {
    if (!node || node->values.size() < 5) return std::nullopt;
    return formats::ActivityPoint{.tile_x = static_cast<std::int32_t>(number(node, 0)),
                                  .tile_z = static_cast<std::int32_t>(number(node, 1)),
                                  .x = number(node, 2),
                                  .y = number(node, 3),
                                  .z = number(node, 4)};
}

[[nodiscard]] bool has_condition(const formats::ActivityEvent& event, const std::string_view name) {
    return std::any_of(event.conditions.begin(), event.conditions.end(),
                       [&](const formats::ActivityNode& condition) { return condition.name == name; });
}

// A TrainID ( s t ) child: negative numbers (any / the event's own) and
// none mean `own`.
[[nodiscard]] ActivityTrainId train_in(const formats::ActivityNode& node, const ActivityTrainId own) {
    const auto* id = node.child("trainid");
    if (!id || id->values.size() < 2 || number(id, 0) < 0.0 || number(id, 1) < 0.0) return own;
    return {static_cast<std::uint32_t>(number(id, 0)), static_cast<std::uint32_t>(number(id, 1))};
}

// A WagonID (ssss-tttt-nnnn): the service and train the car started in, its UiD.
[[nodiscard]] std::optional<ActivityCarRef> wagon_id(const std::string& text) {
    char* end = nullptr;
    const auto s = std::strtoul(text.c_str(), &end, 10);
    if (!end || *end != '-') return std::nullopt;
    const auto t = std::strtoul(end + 1, &end, 10);
    if (!end || *end != '-') return std::nullopt;
    const auto n = std::strtoul(end + 1, &end, 10);
    return ActivityCarRef{.train = {static_cast<std::uint32_t>(s), static_cast<std::uint32_t>(t)},
                          .number = static_cast<std::int32_t>(n),
                          .by_id = true};
}

// A message's buttons (MB_OK, MB_OKCANCEL, MB_YESNO, MB_YESNOCANCEL).
[[nodiscard]] std::vector<std::string> buttons_of(const std::string& written) {
    const auto key = lower(written);
    if (key == "mb_okcancel") return {"ID_OK", "ID_CANCEL"};
    if (key == "mb_yesno") return {"ID_YES", "ID_NO"};
    if (key == "mb_yesnocancel") return {"ID_YES", "ID_NO", "ID_CANCEL"};
    return {"ID_OK"};
}

[[nodiscard]] const char* button_label(const std::string& id) {
    if (id == "ID_CANCEL") return tr("Cancel");
    if (id == "ID_YES") return tr("Yes");
    if (id == "ID_NO") return tr("No");
    return tr("OK");
}

} // namespace

ActivityView::ActivityView(std::shared_ptr<const formats::Activity> activity) : activity_(std::move(activity)) {
    const auto add = [&](const std::uint32_t service, const formats::ActivityEvent& event) {
        events_.push_back({.service = service, .event = &event, .enabled = event.enabled, .level = event.activation_level});
    };
    for (const auto& event : activity_->global_events) add(0, event);
    for (const auto& service : activity_->services)
        for (const auto& event : service.events) add(service.id, event);
}

ActivityTrainId ActivityView::train_of(const std::uint32_t service) const {
    if (service == 0) return player_;
    if (player_.service == service) return player_;
    const auto* s = activity_->service(service);
    return {service, s && !s->trains.empty() ? s->trains.front().id : 1u};
}

std::uint32_t ActivityView::service_in(const double written, const std::uint32_t own) const {
    if (written < -1.5) return own; // -2: the current service
    if (written < -0.5) return 0;   // -1: the global events
    return static_cast<std::uint32_t>(written);
}

bool ActivityView::armed(const EventState& state) const {
    return state.enabled && state.level > 0 && (state.service == 0 || services_.contains(state.service));
}

void ActivityView::fire(EventState& state) {
    std::cout << "activity: event " << state.service << '/' << state.event->id << " at " << clock_text(now_) << std::endl;
    if (!state.event->permanent) state.enabled = false;
    scripts_.push_back({.event = state.event, .service = state.service, .train = train_of(state.service), .next = 0, .wait = 0.0});
}

void ActivityView::start_service(const std::uint32_t service) {
    services_.insert(service);
    for (auto& state : events_) {
        if (state.service == service && armed(state) && has_condition(*state.event, "onstart")) fire(state);
    }
}

void ActivityView::start(const double time_of_day, ActivityWorld& world) {
    if (started_) return;
    started_ = true;
    now_ = time_of_day;
    for (auto& state : events_) state.armed_at = now_;
    if (const auto* player = activity_->player_service()) {
        player_ = activity_->player_train ? ActivityTrainId{activity_->player_train->service, activity_->player_train->train}
                                          : train_of(player->id);
        if (player_.service != player->id) player_ = train_of(player->id);
        services_.insert(player->id);
    }
    // The menu marked MainActivityMenu ( 1 ) is the report menu from the
    // start, before any SetMainMenu (testers 2026-11-21: Novokuznetsk's
    // "Смена на Грузовом (Вторая Часть)" has its first report, the brake
    // test at Томусинская, only there; F8 said there was nothing to report).
    for (const auto& menu : activity_->menus) {
        if (!menu.main) continue;
        main_menu_ = menu.id;
        std::cout << "activity: menu " << main_menu_ << " (MainActivityMenu)" << std::endl;
        break;
    }
    for (auto& state : events_) {
        if (armed(state) && has_condition(*state.event, "activitystarts")) fire(state);
    }
    for (const auto service : std::set<std::uint32_t>(services_)) start_service(service);
    step_scripts(world);
    std::cout << "activity: started at " << clock_text(time_of_day) << ", the player's train " << player_.service << ' '
              << player_.train << ", " << events_.size() << " events" << std::endl;
}

namespace {

[[nodiscard]] std::string escape_line(const std::string& text) {
    std::string out;
    for (const char c : text) {
        if (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else if (c == '\t') out += "\\t";
        else if (c != '\r') out += c;
    }
    return out;
}

[[nodiscard]] std::string unescape_line(const std::string& text) {
    std::string out;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\\' && i + 1 < text.size()) {
            const char n = text[++i];
            out += n == 'n' ? '\n' : n == 't' ? '\t' : n;
        } else {
            out += text[i];
        }
    }
    return out;
}

[[nodiscard]] std::vector<std::string> split_tabs(const std::string& value) {
    std::vector<std::string> fields;
    std::size_t start = 0;
    for (;;) {
        const auto tab = value.find('\t', start);
        fields.push_back(value.substr(start, tab == std::string::npos ? std::string::npos : tab - start));
        if (tab == std::string::npos) break;
        start = tab + 1;
    }
    return fields;
}

} // namespace

std::string ActivityView::save() const {
    std::ostringstream out;
    out << std::setprecision(17);
    out << "now=" << now_ << "\noutcome=" << static_cast<int>(outcome_) << "\nmain_menu=" << main_menu_ << "\nplayer=" << player_.service
        << ' ' << player_.train << '\n';
    for (std::size_t i = 0; i < events_.size(); ++i) {
        const auto& e = events_[i];
        out << "event=" << i << ' ' << (e.enabled ? 1 : 0) << ' ' << e.level << ' ' << e.armed_at << ' ' << (e.held ? 1 : 0) << ' '
            << (e.level_changed ? 1 : 0) << '\n';
        for (const auto& [where, value] : e.last) {
            out << "last=" << i << ' ' << (std::get<0>(where) ? 1 : 0) << ' ' << std::get<1>(where) << ' ' << std::get<2>(where) << ' '
                << std::get<3>(where) << ' ' << std::get<4>(where) << ' ' << value << '\n';
        }
    }
    for (const auto& script : scripts_) {
        std::size_t index = events_.size();
        for (std::size_t i = 0; i < events_.size(); ++i)
            if (events_[i].event == script.event && events_[i].service == script.service) index = i;
        if (index == events_.size()) continue;
        out << "script=" << index << ' ' << script.service << ' ' << script.train.service << ' ' << script.train.train << ' ' << script.next
            << ' ' << script.wait << '\n';
    }
    for (const auto service : services_) out << "service=" << service << '\n';
    for (const auto& [train, set] : tracked_) {
        out << "tracked=" << train.service << ' ' << train.train << ' ' << (set.cars ? 1 : 0);
        if (set.cars)
            for (const auto& car : *set.cars) out << ' ' << car.train.service << ' ' << car.train.train << ' ' << car.number << ' ' << (car.by_id ? 1 : 0);
        out << '\n';
    }
    for (const auto& entry : journal_) out << "journal=" << entry.time << '\t' << escape_line(entry.title) << '\t' << escape_line(entry.text) << '\n';
    for (const auto& window : windows_) {
        std::string buttons;
        for (const auto& b : window.buttons) buttons += (buttons.empty() ? "" : ",") + b;
        out << "window=" << window.id << ' ' << (window.modal ? 1 : 0) << ' ' << window.expires << '\t' << window.entry.time << '\t'
            << escape_line(window.entry.title) << '\t' << escape_line(window.entry.text) << '\t' << buttons << '\n';
    }
    return out.str();
}

void ActivityView::load(const std::string& text) {
    started_ = true;
    scripts_.clear();
    services_.clear();
    tracked_.clear();
    journal_.clear();
    windows_.clear();
    std::istringstream input(text);
    for (std::string line; std::getline(input, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto equals = line.find('=');
        if (equals == std::string::npos) continue;
        const auto key = line.substr(0, equals);
        const auto value = line.substr(equals + 1);
        std::istringstream fields(value);
        if (key == "now") {
            fields >> now_;
        } else if (key == "outcome") {
            int outcome = 0;
            if (fields >> outcome) outcome_ = static_cast<Outcome>(std::clamp(outcome, 0, 2));
        } else if (key == "main_menu") {
            fields >> main_menu_;
        } else if (key == "player") {
            fields >> player_.service >> player_.train;
        } else if (key == "event") {
            std::size_t i{};
            int enabled = 1, held = 0, changed = 0;
            std::int32_t level = 1;
            double armed_at = 0.0;
            if (fields >> i >> enabled >> level >> armed_at >> held >> changed && i < events_.size()) {
                auto& e = events_[i];
                e.enabled = enabled != 0;
                e.level = level;
                e.armed_at = armed_at;
                e.held = held != 0;
                e.level_changed = changed != 0;
                e.last.clear();
            }
        } else if (key == "last") {
            std::size_t i{}, relation{};
            int by_id = 0;
            std::uint32_t service{}, train{};
            std::int32_t number{};
            double last = 0.0;
            if (fields >> i >> by_id >> service >> train >> number >> relation >> last && i < events_.size())
                events_[i].last[{by_id != 0, service, train, number, relation}] = last;
        } else if (key == "script") {
            std::size_t i{};
            Script script;
            if (fields >> i >> script.service >> script.train.service >> script.train.train >> script.next >> script.wait && i < events_.size()) {
                script.event = events_[i].event;
                scripts_.push_back(script);
            }
        } else if (key == "service") {
            std::uint32_t service{};
            if (fields >> service) services_.insert(service);
        } else if (key == "tracked") {
            ActivityTrainId train;
            int has_cars = 0;
            if (!(fields >> train.service >> train.train >> has_cars)) continue;
            CarSet set;
            if (has_cars) {
                set.cars.emplace();
                ActivityCarRef car;
                int by_id = 0;
                while (fields >> car.train.service >> car.train.train >> car.number >> by_id) {
                    car.by_id = by_id != 0;
                    set.cars->push_back(car);
                }
            }
            tracked_[train] = std::move(set);
        } else if (key == "journal") {
            const auto parts = split_tabs(value);
            if (parts.size() < 3) continue;
            journal_.push_back({.time = std::atof(parts[0].c_str()), .title = unescape_line(parts[1]), .text = unescape_line(parts[2])});
        } else if (key == "window") {
            const auto parts = split_tabs(value);
            if (parts.size() < 5) continue;
            Window window;
            std::istringstream head(parts[0]);
            int modal = 0;
            head >> window.id >> modal >> window.expires;
            window.modal = modal != 0;
            window.entry = {.time = std::atof(parts[1].c_str()), .title = unescape_line(parts[2]), .text = unescape_line(parts[3])};
            std::size_t start = 0;
            while (start <= parts[4].size() && !parts[4].empty()) {
                const auto comma = parts[4].find(',', start);
                window.buttons.push_back(parts[4].substr(start, comma == std::string::npos ? std::string::npos : comma - start));
                if (comma == std::string::npos) break;
                start = comma + 1;
            }
            windows_.push_back(std::move(window));
        }
    }
    std::cout << "activity: put back at " << clock_text(now_) << ", " << scripts_.size() << " scripts waiting, " << services_.size()
              << " services in play" << std::endl;
}

void ActivityView::choose(const std::int32_t menu, const std::int32_t item) {
    choices_.emplace_back(menu, item);
    menu_open_ = false;
}

bool ActivityView::holds() const { return !windows_.empty() && windows_.front().modal; }

void ActivityView::close_message(const std::string_view button) {
    if (windows_.empty()) return;
    closed_.emplace_back(windows_.front().id, std::string(button));
    windows_.erase(windows_.begin());
}

void ActivityView::update(const float elapsed, const double time_of_day, ActivityWorld& world) {
    if (!started_) return;
    now_ = time_of_day;
    hint_left_ = (std::max)(hint_left_ - elapsed, 0.0f);
    notice_left_ = (std::max)(notice_left_ - elapsed, 0.0f);
    if (!windows_.empty() && windows_.front().expires > 0.0f) {
        windows_.front().expires -= elapsed;
        if (windows_.front().expires <= 0.0f) close_message("ID_OK");
    }
    // A modal message holds the scenario as it holds the game.
    if (holds() || outcome_ != Outcome::running) return;
    // Traffic trains started since the last update: their services' events
    // are in play, OnStart fires.
    for (const auto started : world.take_started()) {
        std::cout << "activity: traffic train " << started.service << ' ' << started.train << " started at " << clock_text(now_) << std::endl;
        start_service(started.service);
    }
    for (auto& script : scripts_) script.wait -= elapsed;
    // The messages closed (OnCloseMessageBox) and the menu items picked (OnMenuSelect).
    for (const auto& [id, button] : std::exchange(closed_, {})) {
        for (auto& state : events_) {
            if (!armed(state)) continue;
            for (const auto& condition : state.event->conditions) {
                if (condition.name != "onclosemessagebox") continue;
                const auto want = condition.text("buttonid");
                if (static_cast<std::int32_t>(number(condition.child("msgid"), 0, -2)) == id && (want.empty() || lower(want) == lower(button))) {
                    fire(state);
                    break;
                }
            }
        }
    }
    for (const auto& [menu, item] : std::exchange(choices_, {})) {
        for (auto& state : events_) {
            if (!armed(state)) continue;
            for (const auto& condition : state.event->conditions) {
                if (condition.name != "onmenuselect") continue;
                if (static_cast<std::int32_t>(number(condition.child("menuid"), 0, -1)) == menu
                    && static_cast<std::int32_t>(number(condition.child("menuitemid"), 0, -1)) == item) {
                    fire(state);
                    break;
                }
            }
        }
    }
    const auto couplings = world.take_couplings();
    uncouplings_ = world.take_uncouplings();
    for (const auto& u : uncouplings_)
        std::cout << "activity: train " << u.train.service << ' ' << u.train.train << " uncoupled: " << u.head << " + " << u.tail << " cars"
                  << std::endl;
    for (auto& state : events_) {
        // A level change is looked at once (OnActivationLevel).
        const bool changed = std::exchange(state.level_changed, false);
        if (!armed(state)) continue;
        state.level_changed = changed;
        const bool now = holds_now(state, world, couplings, false);
        state.level_changed = false;
        // Permanent: again each time it comes true, not every frame it holds.
        if (now && (!state.event->permanent || !state.held)) fire(state);
        state.held = now;
    }
    uncouplings_.clear();
    step_scripts(world);
}

std::optional<bool> ActivityView::wagon_state(EventState& state, const formats::ActivityNode& c, const ActivityTrainId own,
                                              ActivityWorld& world, const bool check_once) {
    // Looked at while its train's state is tracked (StartWagonStateTracking),
    // or once (CheckWagonState); the dynamic relations only while tracked.
    const auto train = train_in(c, own);
    if (!check_once && !tracked_.contains(train)) return false;
    // activity.pdf 27: with WagonID or WagonNum the relations are checked
    // for that car only, else for each car looked at (the tracking's or the
    // check's WagonRange / WagonList, else every car): the event comes when
    // one car meets them (all of them, or one with Any).
    std::vector<ActivityCarRef> cars;
    if (const auto id = c.text("wagonid"); !id.empty()) {
        if (auto parsed = wagon_id(id)) cars.push_back(*parsed);
    } else if (const auto* num = c.child("wagonnum")) {
        cars.push_back({.train = train, .number = static_cast<std::int32_t>(number(num, 0, 0))});
    } else {
        cars = cars_of(train, check_once, world);
    }
    const bool any = c.child("any") != nullptr;
    bool met = false;
    for (const auto& car : cars) {
        bool tested = false, result = !any, unknown = false;
        for (std::size_t k = 0; k < c.children.size(); ++k) {
            const auto& test = c.children[k];
            const auto& op = test.name;
            const bool dynamic = op == "incpast" || op == "decpast" || op == "setto" || op == "setnotto";
            const bool fixed = op == "equal" || op == "notequal" || op == "above" || op == "aboveorequal" || op == "below" || op == "beloworequal";
            if (!dynamic && !fixed) continue;
            if (test.values.size() < 5) continue;
            const auto value = world.car_value(car, test.values[0], test.values[1], static_cast<int>(number(&test, 2)), static_cast<int>(number(&test, 3)));
            if (!value) {
                if (not_run_.insert("WagonState " + test.values[1]).second)
                    std::cout << "activity: WagonState " << test.values[1] << " not known here" << std::endl;
                // Unknown: never met (Any looks at the others).
                unknown = true;
                continue;
            }
            const double want = number(&test, 4);
            bool ok = false;
            if (dynamic) {
                auto& last = state.last;
                const auto key = std::make_tuple(car.by_id, car.train.service, car.train.train, car.number, k);
                const auto before = last.find(key);
                const bool first = before == last.end();
                const double was = first ? *value : before->second;
                last[key] = *value;
                if (check_once || first) {
                    ok = false;
                } else if (op == "incpast") {
                    ok = was < want && *value >= want;
                } else if (op == "decpast") {
                    ok = was > want && *value <= want;
                } else if (op == "setto") {
                    ok = std::abs(was - want) >= 1e-3 && std::abs(*value - want) < 1e-3;
                } else {
                    ok = std::abs(was - want) < 1e-3 && std::abs(*value - want) >= 1e-3;
                }
            } else {
                ok = op == "equal"          ? std::abs(*value - want) < 1e-3
                   : op == "notequal"       ? std::abs(*value - want) >= 1e-3
                   : op == "above"          ? *value > want
                   : op == "aboveorequal"   ? *value >= want
                   : op == "below"          ? *value < want
                                            : *value <= want;
            }
            tested = true;
            result = any ? (result || ok) : (result && ok);
        }
        // Every car is looked at: the dynamic relations keep each car's last value.
        if (tested && result && (any || !unknown)) met = true;
    }
    return met;
}

ActivityView::CarSet ActivityView::car_set(const formats::ActivityNode& node, const ActivityTrainId train) {
    CarSet set;
    if (const auto* range = node.child("wagonrange"); range && range->values.size() >= 2) {
        std::vector<ActivityCarRef> cars;
        const auto a = static_cast<std::int32_t>(number(range, 0)), b = static_cast<std::int32_t>(number(range, 1));
        for (auto n = (std::min)(a, b); n <= (std::max)(a, b); ++n) cars.push_back({.train = train, .number = n});
        set.cars = std::move(cars);
    } else if (const auto* list = node.child("wagonlist")) {
        std::vector<ActivityCarRef> cars;
        for (const auto& wagon : list->children)
            if (wagon.name == "wagon" || wagon.name == "wagonid")
                if (!wagon.values.empty())
                    if (auto id = wagon_id(wagon.values.front())) cars.push_back(*id);
        set.cars = std::move(cars);
    }
    return set;
}

std::vector<ActivityCarRef> ActivityView::cars_of(const ActivityTrainId train, const bool check_once, ActivityWorld& world) const {
    const CarSet* set = check_once ? checking_ : nullptr;
    if (!check_once)
        if (const auto tracked = tracked_.find(train); tracked != tracked_.end()) set = &tracked->second;
    if (set && set->cars) return *set->cars;
    std::vector<ActivityCarRef> cars;
    const auto count = world.car_count(train); // 0: the train is not in play
    for (std::size_t n = 0; n < count; ++n) cars.push_back({.train = train, .number = static_cast<std::int32_t>(n)});
    return cars;
}

bool ActivityView::holds_now(EventState& state, ActivityWorld& world,
                             const std::vector<std::pair<ActivityTrainId, ActivityTrainId>>& couplings, const bool check_once) {
    const auto& conditions = state.event->conditions;
    if (conditions.empty()) return false;
    const auto own = train_of(state.service);
    for (const auto& c : conditions) {
        // Fired by their moments, not polled.
        if (c.name == "activitystarts" || c.name == "onstart" || c.name == "onmenuselect" || c.name == "onclosemessagebox") return false;
        if (c.name == "playerboardstrain") {
            const auto area = lower(c.text("area"));
            if (area == "exterior") {
                not_run_.insert("PlayerBoardsTrain EXTERIOR");
                return false;
            }
            if (!world.in_cab(train_in(c, own))) return false;
        } else if (c.name == "reachlocation") {
            const auto at = point(c.child("location"));
            const auto train = train_in(c, own);
            // activity.pdf 3: ReachWith ( WAGON_ANY ... PLAYER_POSITION );
            // Reach read too.
            auto reach = c.text("reachwith");
            if (reach.empty()) reach = c.text("reach");
            if (lower(reach) == "player_position") reach = "PLAYER";
            if (!at || !world.near(train, *at, number(c.child("radius"), 0, 10.0), reach)) return false;
            const auto* not_above = c.child("speednotabove");
            const auto* not_below = c.child("speednotbelow");
            if (not_above || not_below) {
                const auto speed = world.car_value({.train = train}, "WAGON", "Velocity", 0, 0);
                if (!speed) return false;
                if (not_above && *speed > number(not_above, 0)) return false;
                if (not_below && *speed < number(not_below, 0)) return false;
            }
        } else if (c.name == "couplecars") {
            const auto* other_id = c.child("conjtrainid");
            if (!other_id || other_id->values.size() < 2) return false;
            const ActivityTrainId other{static_cast<std::uint32_t>(number(other_id, 0)), static_cast<std::uint32_t>(number(other_id, 1))};
            const auto mine = train_in(c, own);
            const bool met = std::any_of(couplings.begin(), couplings.end(), [&](const auto& pair) {
                return (pair.first == mine && pair.second == other) || (pair.first == other && pair.second == mine);
            });
            if (!met) return false;
        } else if (c.name == "wagonstate") {
            const auto met = wagon_state(state, c, own, world, check_once);
            if (!met || !*met) return false;
        } else if (c.name == "onactivationlevel") {
            // activity.pdf 11: as its level changes, above WhenAbove or
            // below WhenBelow (the exams' failures: SetActivationLevel ( -1 n 1 )).
            if (!state.level_changed) return false;
            const auto* above = c.child("whenabove");
            const auto* below = c.child("whenbelow");
            const bool up = above && state.level > number(above, 0);
            const bool down = below && state.level < number(below, 0);
            if (!up && !down) return false;
        } else if (c.name == "reachvelocity") {
            // activity.pdf 21: SpeedNotAbove, the speed it comes below (0:
            // standing); SpeedNotBelow, the speed it comes above.
            const auto speed = world.car_value({.train = train_in(c, own)}, "WAGON", "Velocity", 0, -1);
            if (!speed) return false;
            const auto* not_above = c.child("speednotabove");
            const auto* not_below = c.child("speednotbelow");
            if (!not_above && !not_below) return false;
            if (not_above && std::abs(*speed) > number(not_above, 0) + 0.01) return false;
            if (not_below && std::abs(*speed) <= number(not_below, 0)) return false;
        } else if (c.name == "uncouplecars") {
            // activity.pdf 2: the train cut in two. WagonRange ( StartNum
            // EndNum ): StartNum cars at its head (the head part goes on as
            // the train), or EndNum at its tail (the tail part does);
            // WagonList: the cars that must be in the cut.
            const auto mine = train_in(c, own);
            const auto* range = c.child("wagonrange");
            const auto* list = c.child("wagonlist");
            const auto wanted = list ? car_set(c, mine).cars : std::nullopt;
            const bool met = std::any_of(uncouplings_.begin(), uncouplings_.end(), [&](const ActivityUncoupling& u) {
                if (u.train != mine) return false;
                if (range && range->values.size() >= 2) {
                    const auto head = static_cast<std::size_t>((std::max)(0.0, number(range, 0)));
                    const auto tail = static_cast<std::size_t>((std::max)(0.0, number(range, 1)));
                    if (head > 0 && u.head != head) return false;
                    if (head == 0 && tail > 0 && u.tail != tail) return false;
                }
                if (wanted) {
                    for (const auto& w : *wanted) {
                        const bool in = std::any_of(u.cut.begin(), u.cut.end(), [&](const ActivityCarRef& car) {
                            return car.train == w.train && car.number == w.number;
                        });
                        if (!in) return false;
                    }
                }
                return true;
            });
            if (!met) return false;
        } else if (c.name == "reachreversepoint") {
            // activity.pdf 16: PointID, the reverse point's number on the
            // player's path from 0 (an AssignService's path counts anew).
            const auto reached = world.reverse_points_reached(train_in(c, own));
            const auto point = static_cast<std::size_t>((std::max)(0.0, number(c.child("pointid"), 0, 0.0)));
            if (!reached || *reached <= point) return false;
        } else if (c.name == "activateontime" || c.name == "activatebytimer") {
            // Both keep their time in Time ( ... ), as all of RTS's
            // installed activities write them (ActivateByTimer ( Time ( 30.0 ) )).
            // Read as a bare value, the timers fired at once and
            // ActivateOnTime never; a bare ( ... ) still works.
            const auto* time = c.child("time");
            const auto* holder = time ? time : &c;
            if (c.name == "activateontime") {
                const auto at = formats::parse_activity_time(holder->values.empty() ? std::string_view{} : std::string_view(holder->values.front()));
                if (!at || now_ < *at) return false;
            } else if (now_ - state.armed_at < number(holder, 0)) {
                return false;
            }
        } else {
            if (not_run_.insert(c.name).second) std::cout << "activity: condition " << c.name << " not run yet" << std::endl;
            return false;
        }
    }
    return true;
}

void ActivityView::check_wagons(const ActivityTrainId train, const CarSet& cars, ActivityWorld& world) {
    checking_ = &cars;
    for (auto& state : events_) {
        if (!armed(state) || !has_condition(*state.event, "wagonstate")) continue;
        const auto own = train_of(state.service);
        bool mine = false;
        for (const auto& c : state.event->conditions)
            if (c.name == "wagonstate" && train_in(c, own) == train) mine = true;
        if (mine && holds_now(state, world, {}, true)) fire(state);
    }
    checking_ = nullptr;
}

void ActivityView::step_scripts(ActivityWorld& world) {
    // Runs till every script waits or ends; outcomes may start more (an
    // AssignService's OnStart), taken in on the next pass.
    for (int pass = 0; pass < 16; ++pass) {
        bool moved = false;
        for (std::size_t i = 0; i < scripts_.size();) {
            bool done = false;
            while (scripts_[i].wait <= 0.0 && !holds() && outcome_ == Outcome::running) {
                if (scripts_[i].next >= scripts_[i].event->outcomes.size()) {
                    done = true;
                    break;
                }
                Script script = scripts_[i]; // outcomes may add scripts
                const auto& outcome = script.event->outcomes[script.next++];
                const bool goes_on = run_outcome(script, outcome, world);
                scripts_[i] = script;
                moved = true;
                if (!goes_on) break;
            }
            if (done) {
                scripts_.erase(scripts_.begin() + static_cast<std::ptrdiff_t>(i));
            } else {
                ++i;
            }
        }
        if (!moved) break;
    }
}

void ActivityView::say(const formats::ActivityNode& outcome) {
    if (outcome.name == "showmessage") {
        Window window{.entry = {.time = now_, .title = outcome.text("title"), .text = outcome.text("text")},
                      .id = static_cast<std::int32_t>(number(outcome.child("msgid"), 0, -1)),
                      .buttons = buttons_of(outcome.text("buttons")),
                      .modal = truthy(outcome.text("showmodal")),
                      .expires = static_cast<float>(number(outcome.child("expires"), 0, 0.0))};
        journal_.push_back(window.entry);
        windows_.push_back(std::move(window));
    } else if (outcome.name == "postmessage") {
        journal_.push_back({.time = now_, .title = outcome.text("title"), .text = outcome.text("text")});
        notice_ = tr("A new message of the scenario (F11)");
        notice_left_ = notice_seconds;
    } else if (outcome.name == "showhint") {
        hint_ = outcome.text("text");
        hint_left_ = hint_seconds;
        journal_.push_back({.time = now_, .title = {}, .text = hint_});
    }
}

bool ActivityView::run_outcome(Script& script, const formats::ActivityNode& o, ActivityWorld& world) {
    const auto& name = o.name;
    // The train an outcome is for: its TrainID, else the event's.
    const auto train = train_in(o, script.train);
    if (name == "suspend") {
        const double t = number(&o, 0);
        script.wait = t > 0.0 ? t : forever; // <= 0: till ActivateEvent
        return false;
    }
    if (name == "showmessage" || name == "postmessage" || name == "showhint") {
        say(o);
        return true;
    }
    if (name == "setmainmenu") {
        main_menu_ = static_cast<std::int32_t>(number(&o, 0, -1));
        std::cout << "activity: menu " << main_menu_ << std::endl;
        if (const auto* menu = activity_->menu(main_menu_)) {
            notice_ = trf("A report is possible: F8 (%s)", menu->name.c_str());
            notice_left_ = notice_seconds;
        }
        return true;
    }
    if (name == "showactivitymenu") {
        main_menu_ = static_cast<std::int32_t>(number(&o, 0, main_menu_));
        menu_open_ = activity_->menu(main_menu_) != nullptr;
        return true;
    }
    if (name == "enableevent" || name == "disableevent" || name == "activateevent" || name == "increaseactivationlevel"
        || name == "setactivationlevel") {
        // ( ServiceID EventID [n] ); IncreaseActivationLevel ( n ) alone: this event.
        std::uint32_t service = script.service;
        std::uint32_t id = script.event->id;
        double amount = 0.0;
        if (o.values.size() >= 2) {
            service = service_in(number(&o, 0), script.service);
            id = static_cast<std::uint32_t>(number(&o, 1));
            amount = number(&o, 2);
        } else {
            amount = number(&o, 0);
        }
        for (auto& state : events_) {
            if (state.service != service || state.event->id != id) continue;
            if (name == "enableevent" || name == "disableevent") {
                state.enabled = name == "enableevent";
                state.armed_at = now_;
                state.held = false;
            } else if (name == "increaseactivationlevel" || name == "setactivationlevel") {
                const auto was = state.level;
                state.level = name == "setactivationlevel" ? static_cast<std::int32_t>(amount) : state.level + static_cast<std::int32_t>(amount);
                if (state.level != was) state.level_changed = true;
            } else {
                // ActivateEvent: a suspended run goes on; else, enabled, it fires.
                bool resumed = false;
                for (auto& other : scripts_) {
                    if (other.event == state.event && other.wait > 0.0) {
                        other.wait = 0.0;
                        resumed = true;
                    }
                }
                if (!resumed && state.enabled && state.event != script.event) fire(state);
            }
        }
        return true;
    }
    if (name == "signaloverride") {
        const auto at = point(o.child("position"));
        if (!at) return true;
        std::optional<simulation::SignalOverride> value;
        if (!o.child("cancel")) {
            value.emplace();
            if (const auto mode = lower(o.text("signalmode")); !mode.empty()) {
                value->mode = mode == "shunting" || mode == "shunt" ? simulation::SignalMode::shunt
                            : mode == "failed"                      ? simulation::SignalMode::failed
                            : mode == "excluded"                    ? simulation::SignalMode::excluded
                                                                    : simulation::SignalMode::normal;
            }
            if (const auto* aspect = o.child("setaspect"); aspect && aspect->values.size() >= 2)
                value->aspect = formats::signal_aspect_from_name(aspect->values[1]);
            if (const auto* draw = o.child("setdrawstate"); draw && draw->values.size() >= 2)
                value->draw_state = static_cast<int>(number(draw, 1));
        }
        if (world.override_signal(*at, value)) {
            std::cout << "activity: signal at " << at->tile_x << ' ' << at->tile_z << ' ' << at->x << ' ' << at->z << (value ? " held" : " let go")
                      << '\n';
        } else {
            std::cout << "activity: SignalOverride: no signal at " << at->tile_x << ' ' << at->tile_z << ' ' << at->x << ' ' << at->z << '\n';
        }
        return true;
    }
    if (name == "assignservice") {
        const auto* id = o.child("newid");
        if (!id || id->values.size() < 2 || number(id, 0) < 0.0) return true;
        const ActivityTrainId to{static_cast<std::uint32_t>(number(id, 0)),
                                 number(id, 1) < 0.0 ? script.train.train : static_cast<std::uint32_t>(number(id, 1))};
        const auto from = script.train;
        world.reassign(from, to);
        if (player_ == from) player_ = to;
        if (const auto tracked = tracked_.find(from); tracked != tracked_.end()) {
            auto cars = tracked->second;
            tracked_.erase(tracked);
            tracked_[to] = std::move(cars);
        }
        services_.erase(script.service);
        script.train = to;
        std::cout << "activity: train " << from.service << ' ' << from.train << " is now service " << to.service << "'s train "
                  << to.train << std::endl;
        start_service(to.service);
        return true;
    }
    if (name == "releaseblockpoint" || name == "enableblockpoint") {
        // ( ServiceID PointID ): -1 the global points, -2 this service's;
        // point -1 all. EnableBlockPoint's Distance: a train nearer than it
        // whose way runs past the point goes on.
        if (o.values.size() < 2) return true;
        const auto service = service_in(number(&o, 0), script.service);
        const auto point = static_cast<std::int32_t>(number(&o, 1));
        const bool enable = name == "enableblockpoint";
        world.set_block_point(service, point, enable, enable ? number(o.child("distance"), 0, 0.0) : 0.0);
        std::cout << "activity: block point " << service << ' ' << point << (enable ? " enabled" : " released") << std::endl;
        return true;
    }
    if (name == "modifyvelocity") {
        world.modify_velocity(train, static_cast<float>(number(&o, 0, -1.0)), static_cast<float>(number(&o, 1, -1.0)),
                              static_cast<float>(number(&o, 2, -1.0)));
        return true;
    }
    if (name == "waithere") {
        world.wait_here(train, number(&o, 0, 0.0));
        return true;
    }
    if (name == "startwagonstatetracking") {
        // activity.pdf 4.2 48: its WagonRange / WagonList, else every car
        // (the Interval: openRTS looks every frame).
        if (number(o.child("enable"), 0, 1.0) != 0.0) {
            tracked_[train] = car_set(o, train);
        } else {
            tracked_.erase(train);
        }
        return true;
    }
    if (name == "checkwagonstate") {
        check_wagons(train, car_set(o, train), world);
        return true;
    }
    if (name == "setdefaultuncouple") {
        // activity.pdf 4.2 45: what the cuts from this train become.
        ActivityUncoupleRule rule;
        if (const auto* id = o.child("newid"); id && id->values.size() >= 2 && number(id, 0) >= 0.0 && number(id, 1) >= 0.0)
            rule.new_id = ActivityTrainId{static_cast<std::uint32_t>(number(id, 0)), static_cast<std::uint32_t>(number(id, 1))};
        rule.new_type = o.text("newtype");
        if (const auto main = o.text("mainwagon"); !main.empty()) rule.main_wagon = wagon_id(main);
        world.set_default_uncouple(train, rule);
        std::cout << "activity: cuts from train " << train.service << ' ' << train.train << " become "
                  << (rule.new_id ? std::to_string(rule.new_id->service) + ' ' + std::to_string(rule.new_id->train) : std::string("unnamed"))
                  << (rule.new_type.empty() ? std::string() : ' ' + rule.new_type) << std::endl;
        return true;
    }
    if (name == "spawntrain") {
        // activity.pdf 4.2 16: the train starts now (a traffic train waiting
        // for it, without a TrainStartTime of its own).
        const auto service = service_in(number(&o, 0, -2), script.service);
        const auto id = static_cast<std::uint32_t>(number(&o, 1, 1));
        world.spawn_train({service, id});
        std::cout << "activity: spawn train " << service << ' ' << id << " at " << clock_text(now_) << std::endl;
        return true;
    }
    if (name == "changeservicestartuptype") {
        // activity.pdf 4.2 22: NewStartupType SERIAL / SINGLE start the
        // service's trains (at their times), NONE keeps them back. TrainID
        // ( ServiceID TrainID ) names the service.
        std::uint32_t service = script.service;
        if (const auto* id = o.child("trainid"); id && !id->values.empty()) service = service_in(number(id, 0), script.service);
        const auto type = lower(o.text("newstartuptype"));
        world.set_service_startup(service, type != "none");
        std::cout << "activity: service " << service << " starts " << (type.empty() ? std::string("single") : type) << std::endl;
        return true;
    }
    if (name == "setspeedlimit") {
        // activity.pdf 4.2 29: till another; not positive, none (m/s).
        world.set_speed_limit(train, static_cast<float>(number(&o, 0, 0.0)));
        return true;
    }
    if (name == "sendsoundtrigger") {
        // ( ObjectNum TriggerID ( Num ) ): object -1, the sounds of car Num.
        const auto object = static_cast<std::int32_t>(number(&o, 0, -1));
        const auto code = static_cast<std::uint32_t>(number(&o, 1));
        const auto* car = o.child("num");
        world.sound_trigger(object, code, {.train = train, .number = static_cast<std::int32_t>(number(car, 0, 0))});
        return true;
    }
    if (name == "modifywagons") {
        // WagonParameters ( n WagonGroup ( Range ( a b ) ... ) ): as a
        // train's own (RTS 0x474590, applied by 0x4FA374).
        if (const auto* parameters = o.child("wagonparameters")) {
            for (const auto& group : formats::read_wagon_parameters(*parameters)) {
                world.modify_wagons(train, group);
                for (const auto& key : group.other)
                    if (not_run_.insert("ModifyWagons " + key).second) std::cout << "activity: ModifyWagons " << key << " not run yet" << std::endl;
            }
        }
        return true;
    }
    if (name == "successfullycompleteactivity" || name == "activityfailed") {
        const bool won = name == "successfullycompleteactivity";
        outcome_ = won ? Outcome::succeeded : Outcome::failed;
        Window window{.entry = {.time = now_,
                               .title = activity_->name,
                               .text = won ? tr("The scenario is completed.") : tr("The scenario has failed.")},
                      .id = -1,
                      .buttons = {"ID_OK"},
                      .modal = false,
                      .expires = 0.0f};
        journal_.push_back(window.entry);
        windows_.push_back(std::move(window));
        std::cout << "activity: " << (won ? "completed" : "failed") << " at " << clock_text(now_) << std::endl;
        return true;
    }
    if (not_run_.insert(name).second) std::cout << "activity: outcome " << name << " not run yet" << std::endl;
    return true;
}

void ActivityView::draw() {
#ifdef OPENRTS_HAS_IMGUI
    const auto display = ImGui::GetIO().DisplaySize;
    const float wide = std::clamp(display.x * 0.5f, 360.0f, 760.0f);
    if (!windows_.empty()) {
        // The scenario's message, in the middle of the screen.
        const auto& window = windows_.front();
        ImGui::SetNextWindowPos(ImVec2{display.x * 0.5f, display.y * 0.4f}, ImGuiCond_Always, ImVec2{0.5f, 0.5f});
        ImGui::SetNextWindowSizeConstraints(ImVec2{wide, 0.0f}, ImVec2{wide, display.y * 0.8f});
        const std::string title = (window.entry.title.empty() ? activity_->name : window.entry.title) + "###activity_message";
        std::string pressed;
        hud::push_window_style(20.0f, 16.0f, true);
        ImGui::SetNextWindowBgAlpha(0.96f);
        if (ImGui::Begin(title.c_str(), nullptr,
                         ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
            hud::push_window_font();
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wide - ImGui::GetStyle().WindowPadding.x * 2.0f);
            ImGui::TextUnformatted(window.entry.text.c_str());
            ImGui::PopTextWrapPos();
            ImGui::Spacing();
            if (window.modal) ImGui::TextDisabled("%s", tr("(the game waits until the message is closed)"));
            for (std::size_t b = 0; b < window.buttons.size(); ++b) {
                if (b > 0) ImGui::SameLine();
                if (ImGui::Button(button_label(window.buttons[b]), ImVec2{120.0f, 0.0f})) pressed = window.buttons[b];
            }
            if (pressed.empty() && ImGui::IsWindowFocused() && ImGui::IsKeyPressed(ImGuiKey_Enter, false)) pressed = window.buttons.front();
            hud::pop_window_font();
        }
        ImGui::End();
        hud::pop_window_style();
        if (!pressed.empty()) close_message(pressed);
    }
    if (hint_left_ > 0.0f && !hint_.empty()) {
        ImGui::SetNextWindowPos(ImVec2{display.x * 0.5f, display.y * 0.12f}, ImGuiCond_Always, ImVec2{0.5f, 0.0f});
        hud::push_window_style();
        ImGui::SetNextWindowBgAlpha(0.0f);
        ImGui::SetNextWindowSizeConstraints(ImVec2{0.0f, 0.0f}, ImVec2{wide, display.y * 0.5f});
        if (ImGui::Begin("###activity_hint", nullptr,
                         ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings
                             | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs
                             | ImGuiWindowFlags_NoBackground)) {
            hud::push_window_font();
            hud::plate(ImGui::GetWindowDrawList(), ImGui::GetWindowPos(), ImGui::GetWindowSize(), hud::plate_radius,
                       (std::min)(hint_left_, 1.0f));
            ImGui::PushTextWrapPos(wide - ImGui::GetStyle().WindowPadding.x * 2.0f);
            ImGui::TextUnformatted(hint_.c_str());
            ImGui::PopTextWrapPos();
            hud::pop_window_font();
        }
        ImGui::End();
        hud::pop_window_style();
    }
    // Top right: a notice for a while, and a report waiting (F8) as long as it waits.
    const auto* waiting = activity_->menu(main_menu_);
    if ((notice_left_ > 0.0f && !notice_.empty()) || (waiting && !menu_open_)) {
        ImGui::SetNextWindowPos(ImVec2{display.x - 10.0f, display.y * 0.2f}, ImGuiCond_Always, ImVec2{1.0f, 0.0f});
        hud::push_window_style();
        ImGui::SetNextWindowBgAlpha(0.0f);
        if (ImGui::Begin("###activity_notice", nullptr,
                         ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings
                             | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs
                             | ImGuiWindowFlags_NoBackground)) {
            hud::push_window_font();
            hud::plate(ImGui::GetWindowDrawList(), ImGui::GetWindowPos(), ImGui::GetWindowSize());
            if (notice_left_ > 0.0f && !notice_.empty()) ImGui::TextUnformatted(notice_.c_str());
            if (waiting && !menu_open_ && notice_left_ <= 0.0f) ImGui::Text(tr("Report: F8 (%s)"), waiting->name.c_str());
            hud::pop_window_font();
        }
        ImGui::End();
        hud::pop_window_style();
    }
    if (menu_open_) {
        // F8 (RTS): the menu SetMainMenu gave, its visible items to pick.
        ImGui::SetNextWindowPos(ImVec2{display.x * 0.5f, display.y * 0.55f}, ImGuiCond_Appearing, ImVec2{0.5f, 0.5f});
        const std::string title = std::string(waiting ? waiting->name : std::string(tr("Scenario"))) + "###activity_menu";
        hud::push_window_style(20.0f, 16.0f, true);
        ImGui::SetNextWindowBgAlpha(0.96f);
        if (ImGui::Begin(title.c_str(), &menu_open_, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
            hud::push_window_font();
            if (!waiting) {
                ImGui::TextDisabled("%s", tr("Nothing to report now."));
            } else {
                for (const auto& item : waiting->items) {
                    if (!item.visible) continue;
                    ImGui::BeginDisabled(!item.enabled);
                    if (ImGui::Button((item.label + "##" + std::to_string(item.id)).c_str(), ImVec2{(std::max)(wide * 0.6f, 280.0f), 0.0f}))
                        choose(waiting->id, item.id);
                    ImGui::EndDisabled();
                }
            }
            ImGui::Separator();
            ImGui::TextDisabled("%s", tr("F8 closes this menu."));
            hud::pop_window_font();
        }
        ImGui::End();
        hud::pop_window_style();
    }
    if (journal_open_) {
        // F11 (RTS): the scenario and what it has said.
        ImGui::SetNextWindowPos(ImVec2{display.x - 10.0f, 60.0f}, ImGuiCond_FirstUseEver, ImVec2{1.0f, 0.0f});
        ImGui::SetNextWindowSize(ImVec2{wide, display.y * 0.5f}, ImGuiCond_FirstUseEver);
        hud::push_window_style(20.0f, 16.0f, true);
        ImGui::SetNextWindowBgAlpha(0.96f);
        if (ImGui::Begin((std::string(tr("Scenario")) + "###activity_journal").c_str(), &journal_open_, ImGuiWindowFlags_NoCollapse)) {
            hud::push_window_font();
            const auto& a = *activity_;
            ImGui::TextUnformatted(a.name.c_str());
            if (!a.starts_in.empty() || !a.ends_in.empty()) ImGui::Text(tr("From %s to %s"), a.starts_in.c_str(), a.ends_in.c_str());
            ImGui::Text(tr("Starts at %s, lasts %s"), clock_text(a.start_time).c_str(), clock_text(a.duration).c_str());
            if (outcome_ != Outcome::running)
                ImGui::TextUnformatted(outcome_ == Outcome::succeeded ? tr("The scenario is completed.") : tr("The scenario has failed."));
            if (!a.description.empty()) {
                ImGui::PushTextWrapPos(0.0f);
                ImGui::TextDisabled("%s", a.description.c_str());
                ImGui::PopTextWrapPos();
            }
            ImGui::Separator();
            if (journal_.empty()) ImGui::TextDisabled("%s", tr("No messages yet."));
            ImGui::PushTextWrapPos(0.0f);
            for (const auto& entry : journal_) {
                ImGui::TextDisabled("%s", clock_text(entry.time).c_str());
                ImGui::SameLine();
                if (!entry.title.empty()) ImGui::TextUnformatted(entry.title.c_str());
                ImGui::TextUnformatted(entry.text.c_str());
                ImGui::Spacing();
            }
            ImGui::PopTextWrapPos();
            ImGui::Separator();
            ImGui::TextDisabled("%s", tr("F11 closes this window."));
            hud::pop_window_font();
        }
        ImGui::End();
        hud::pop_window_style();
    }
#endif
}

} // namespace openrts::viewer
