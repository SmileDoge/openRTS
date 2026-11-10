#include "activity_view.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <limits>

#ifdef OPENRTS_HAS_IMGUI
#include <imgui.h>
#endif

#include <openrts/formats/signal_config.hpp>

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
    for (auto& state : events_) {
        if (armed(state) && has_condition(*state.event, "activitystarts")) fire(state);
    }
    for (const auto service : std::set<std::uint32_t>(services_)) start_service(service);
    step_scripts(world);
    std::cout << "activity: started at " << clock_text(time_of_day) << ", the player's train " << player_.service << ' '
              << player_.train << ", " << events_.size() << " events" << std::endl;
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
    for (auto& state : events_) {
        if (!armed(state)) continue;
        const bool now = holds_now(state, world, couplings, false);
        // Permanent: again each time it comes true, not every frame it holds.
        if (now && (!state.event->permanent || !state.held)) fire(state);
        state.held = now;
    }
    step_scripts(world);
}

std::optional<bool> ActivityView::wagon_state(EventState& state, const formats::ActivityNode& c, const ActivityTrainId own,
                                              ActivityWorld& world, const bool check_once) {
    // Looked at while its train's state is tracked (StartWagonStateTracking),
    // or once (CheckWagonState); the dynamic relations only while tracked.
    const auto train = train_in(c, own);
    if (!check_once && !tracked_.contains(train)) return false;
    ActivityCarRef car{.train = train, .number = static_cast<std::int32_t>(number(c.child("wagonnum"), 0, 0))};
    if (const auto id = c.text("wagonid"); !id.empty()) {
        if (auto parsed = wagon_id(id)) car = *parsed;
    }
    const bool any = c.child("any") != nullptr;
    bool tested = false, result = !any;
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
            return false;
        }
        const double want = number(&test, 4);
        bool ok = false;
        if (dynamic) {
            const auto before = state.last.find(k);
            const bool first = before == state.last.end();
            const double was = first ? *value : before->second;
            state.last[k] = *value;
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
    return tested && result;
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
            if (!at || !world.near(train, *at, number(c.child("radius"), 0, 10.0), c.text("reach"))) return false;
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
        } else if (c.name == "activateontime") {
            const auto at = formats::parse_activity_time(c.values.empty() ? std::string_view{} : std::string_view(c.values.front()));
            if (!at || now_ < *at) return false;
        } else if (c.name == "activatebytimer") {
            if (now_ - state.armed_at < number(&c, 0)) return false;
        } else {
            if (not_run_.insert(c.name).second) std::cout << "activity: condition " << c.name << " not run yet" << std::endl;
            return false;
        }
    }
    return true;
}

void ActivityView::check_wagons(const ActivityTrainId train, ActivityWorld& world) {
    for (auto& state : events_) {
        if (!armed(state) || !has_condition(*state.event, "wagonstate")) continue;
        const auto own = train_of(state.service);
        bool mine = false;
        for (const auto& c : state.event->conditions)
            if (c.name == "wagonstate" && train_in(c, own) == train) mine = true;
        if (mine && holds_now(state, world, {}, true)) fire(state);
    }
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
            } else if (name == "increaseactivationlevel") {
                state.level += static_cast<std::int32_t>(amount);
            } else if (name == "setactivationlevel") {
                state.level = static_cast<std::int32_t>(amount);
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
        if (tracked_.erase(from) > 0) tracked_.insert(to);
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
        if (number(o.child("enable"), 0, 1.0) != 0.0) {
            tracked_.insert(train);
        } else {
            tracked_.erase(train);
        }
        return true;
    }
    if (name == "checkwagonstate") {
        check_wagons(train, world);
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
        // WagonParameters ( n WagonGroup ( Range ( a b ) ... ) ): the end valves.
        if (const auto* parameters = o.child("wagonparameters")) {
            for (const auto& group : parameters->children) {
                if (group.name != "wagongroup") continue;
                const auto* range = group.child("range");
                const auto first = static_cast<std::int32_t>(number(range, 0, -1));
                const auto last = static_cast<std::int32_t>(number(range, 1, -1));
                for (const auto& p : group.children) {
                    if (p.name == "range") continue;
                    if (p.name == "openendvalves" || p.name == "closeendvalves") {
                        if (number(&p, 0, 1.0) != 0.0) world.end_valves(train, first, last, p.name == "openendvalves");
                    } else if (not_run_.insert("ModifyWagons " + p.name).second) {
                        std::cout << "activity: ModifyWagons " << p.name << " not run yet" << std::endl;
                    }
                }
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
        if (ImGui::Begin(title.c_str(), nullptr,
                         ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
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
        }
        ImGui::End();
        if (!pressed.empty()) close_message(pressed);
    }
    if (hint_left_ > 0.0f && !hint_.empty()) {
        ImGui::SetNextWindowPos(ImVec2{display.x * 0.5f, display.y * 0.12f}, ImGuiCond_Always, ImVec2{0.5f, 0.0f});
        ImGui::SetNextWindowBgAlpha(0.75f * (std::min)(hint_left_, 1.0f));
        ImGui::SetNextWindowSizeConstraints(ImVec2{0.0f, 0.0f}, ImVec2{wide, display.y * 0.5f});
        if (ImGui::Begin("###activity_hint", nullptr,
                         ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings
                             | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs)) {
            ImGui::PushTextWrapPos(wide - ImGui::GetStyle().WindowPadding.x * 2.0f);
            ImGui::TextUnformatted(hint_.c_str());
            ImGui::PopTextWrapPos();
        }
        ImGui::End();
    }
    // Top right: a notice for a while, and a report waiting (F8) as long as it waits.
    const auto* waiting = activity_->menu(main_menu_);
    if ((notice_left_ > 0.0f && !notice_.empty()) || (waiting && !menu_open_)) {
        ImGui::SetNextWindowPos(ImVec2{display.x - 10.0f, display.y * 0.2f}, ImGuiCond_Always, ImVec2{1.0f, 0.0f});
        ImGui::SetNextWindowBgAlpha(0.75f);
        if (ImGui::Begin("###activity_notice", nullptr,
                         ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings
                             | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs)) {
            if (notice_left_ > 0.0f && !notice_.empty()) ImGui::TextUnformatted(notice_.c_str());
            if (waiting && !menu_open_ && notice_left_ <= 0.0f) ImGui::Text(tr("Report: F8 (%s)"), waiting->name.c_str());
        }
        ImGui::End();
    }
    if (menu_open_) {
        // F8 (RTS): the menu SetMainMenu gave, its visible items to pick.
        ImGui::SetNextWindowPos(ImVec2{display.x * 0.5f, display.y * 0.55f}, ImGuiCond_Appearing, ImVec2{0.5f, 0.5f});
        const std::string title = std::string(waiting ? waiting->name : std::string(tr("Scenario"))) + "###activity_menu";
        if (ImGui::Begin(title.c_str(), &menu_open_, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
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
            ImGui::TextDisabled("%s", tr("F8 closes this menu."));
        }
        ImGui::End();
    }
    if (journal_open_) {
        // F11 (RTS): the scenario and what it has said.
        ImGui::SetNextWindowPos(ImVec2{display.x - 10.0f, 60.0f}, ImGuiCond_FirstUseEver, ImVec2{1.0f, 0.0f});
        ImGui::SetNextWindowSize(ImVec2{wide, display.y * 0.5f}, ImGuiCond_FirstUseEver);
        if (ImGui::Begin((std::string(tr("Scenario")) + "###activity_journal").c_str(), &journal_open_, ImGuiWindowFlags_NoCollapse)) {
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
        }
        ImGui::End();
    }
#endif
}

} // namespace openrts::viewer
