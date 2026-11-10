#include <openrts/formats/activity.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "text_tree.hpp"

namespace openrts::formats {
namespace {

using detail::TextNode;

[[nodiscard]] std::string first(const TextNode* node) { return node && !node->values.empty() ? node->values.front() : std::string(); }

[[nodiscard]] std::uint32_t whole(const TextNode* node, const std::size_t index = 0, const std::uint32_t fallback = 0) {
    if (!node || index >= node->values.size()) return fallback;
    return static_cast<std::uint32_t>(std::strtoul(node->values[index].c_str(), nullptr, 10));
}

[[nodiscard]] std::int32_t integer(const TextNode* node, const std::int32_t fallback) {
    if (!node || node->values.empty()) return fallback;
    return static_cast<std::int32_t>(std::strtol(node->values.front().c_str(), nullptr, 10));
}

// TileX TileZ X Y Z from `index` on.
[[nodiscard]] std::optional<ActivityPoint> point(const TextNode* node, const std::size_t index = 0) {
    if (!node || node->values.size() < index + 5) return std::nullopt;
    return ActivityPoint{.tile_x = static_cast<std::int32_t>(node->number(index)),
                         .tile_z = static_cast<std::int32_t>(node->number(index + 1)),
                         .x = node->number(index + 2),
                         .y = node->number(index + 3),
                         .z = node->number(index + 4)};
}

[[nodiscard]] ActivityNode convert(const TextNode& node) {
    ActivityNode result{.name = node.name, .values = node.values, .children = {}};
    result.children.reserve(node.children.size());
    for (const auto& child : node.children) result.children.push_back(convert(child));
    return result;
}

[[nodiscard]] ActivityEvent read_event(const TextNode& node) {
    ActivityEvent event;
    event.id = whole(node.child("id"));
    event.activation_level = integer(node.child("activationlevel"), 1);
    event.enabled = integer(node.child("enabled"), 1) != 0;
    if (const auto* permanent = node.child("permanent")) event.permanent = permanent->values.empty() || permanent->values.front() != "0";
    if (const auto* action = node.child("action")) {
        for (const auto& condition : action->children) event.conditions.push_back(convert(condition));
    }
    if (const auto* outcome = node.child("outcome")) {
        for (const auto& step : outcome->children) event.outcomes.push_back(convert(step));
    }
    return event;
}

void read_events(const TextNode* list, std::vector<ActivityEvent>& events) {
    if (!list) return;
    for (const auto& node : list->children) {
        if (node.name == "serviceevent" || node.name == "event") events.push_back(read_event(node));
    }
}

[[nodiscard]] ActivityTrain read_train(const TextNode& node) {
    ActivityTrain train;
    train.id = whole(node.child("id"));
    train.consist = first(node.child("consist"));
    if (const auto* placement = node.child("trainplacement")) {
        train.start = point(placement->child("pointstart"));
        train.heading = point(placement->child("pointheading"));
    }
    if (const auto* time = node.child("trainstarttime")) train.start_time = parse_activity_time(first(time));
    if (const auto* time = node.child("trainremovetime")) train.remove_time = parse_activity_time(first(time));
    // Both times given and the start later: neither counts (RTS 0x471B9C).
    if (train.start_time && train.remove_time && *train.start_time > *train.remove_time) {
        train.start_time.reset();
        train.remove_time.reset();
    }
    if (const auto* speed = node.child("trainmaxspeed"); speed && !speed->values.empty())
        train.max_speed = static_cast<float>(speed->number(0));
    if (const auto* rates = node.child("trainacceleration"); rates && !rates->values.empty()) {
        train.acceleration = static_cast<float>(std::abs(rates->number(0)));
        if (rates->values.size() > 1) train.deceleration = static_cast<float>(std::abs(rates->number(1)));
    }
    if (const auto* value = node.child("trainefficiency"); value && !value->values.empty())
        train.efficiency = static_cast<float>(value->number(0));
    train.efficiency = std::clamp(train.efficiency, 0.03f, 2.0f);
    if (const auto* value = node.child("startupspeed"); value && !value->values.empty())
        train.startup_speed = static_cast<float>(value->number(0));
    if (const auto* value = node.child("finishspeed"); value && !value->values.empty())
        train.finish_speed = static_cast<float>(value->number(0));
    if (const auto* value = node.child("trainstartoffset"); value && !value->values.empty())
        train.start_offset = static_cast<float>(value->number(0));
    if (const auto* value = node.child("traindistanceofnoreturn"); value && !value->values.empty())
        train.no_return = (std::max)(static_cast<float>(value->number(0)), 250.0f);
    train.service_type = first(node.child("trainservicetype"));
    if (const auto* flags = node.child("trainflags")) train.flags = flags->values;
    if (const auto* state = node.child("trainstate"); state && !state->values.empty()) train.state = integer(state, 0);
    return train;
}

[[nodiscard]] bool has_flag(const std::vector<std::string>& flags, const std::string_view name) {
    const auto key = detail::lower_ascii(std::string(name));
    return std::any_of(flags.begin(), flags.end(), [&](const std::string& flag) { return detail::lower_ascii(flag) == key; });
}

[[nodiscard]] std::optional<std::pair<std::int32_t, std::int32_t>> id_pair(const TextNode* node) {
    if (!node || node->values.size() < 2) return std::nullopt;
    return std::pair{static_cast<std::int32_t>(node->number(0)), static_cast<std::int32_t>(node->number(1))};
}

[[nodiscard]] ActivityBlockPoint read_block_point(const TextNode& node) {
    ActivityBlockPoint block;
    block.id = integer(node.child("id"), 0);
    block.enabled = integer(node.child("enabled"), 1) != 0;
    // Location ( TileX TileZ X Y Z ): RTS reads the five numbers; what the
    // RTS editor writes after them ("NumPasses: 1") is not read.
    block.location = point(node.child("location"));
    block.passed = integer(node.child("numpassed"), 0);
    if (const auto* affects = node.child("pointaffects"); affects && !affects->values.empty()) {
        const auto count = static_cast<std::size_t>((std::max)(0, integer(affects, 0)));
        for (std::size_t i = 1; i <= count && i < affects->values.size(); ++i)
            block.affects.push_back(static_cast<std::int32_t>(affects->number(i)));
    }
    if (const auto* time = node.child("releaseontime")) block.release_time = parse_activity_time(first(time));
    block.release_on_reach = id_pair(node.child("releaseonreach"));
    block.release_on_approach = id_pair(node.child("releaseonapproach"));
    if (const auto* manual = node.child("releasemanual")) block.release_manual = manual->values.empty() || integer(manual, 1) != 0;
    if (const auto* suppress = node.child("suppresserrors"))
        block.suppress_errors = suppress->values.empty() || integer(suppress, 1) != 0;
    return block;
}

void read_block_points(const TextNode* list, std::vector<ActivityBlockPoint>& points) {
    if (!list) return;
    for (const auto& node : list->children)
        if (node.name == "blockpoint") points.push_back(read_block_point(node));
}

[[nodiscard]] ActivityServiceType service_type(const std::string& name) {
    const auto key = detail::lower_ascii(name);
    if (key == "player") return ActivityServiceType::player;
    if (key == "static") return ActivityServiceType::standing;
    if (key == "traffic") return ActivityServiceType::traffic;
    return ActivityServiceType::other;
}

[[nodiscard]] ActivityService read_service(const TextNode& node) {
    ActivityService service;
    service.id = whole(node.child("id"));
    service.name = first(node.child("name"));
    service.type_name = first(node.child("type"));
    service.type = service_type(service.type_name);
    service.path = first(node.child("path"));
    if (const auto* startup = node.child("servicestartup")) service.starts = first(startup->child("servicestarts"));
    if (const auto* flags = node.child("serviceflags")) service.flags = flags->values;
    read_block_points(node.child("serviceblockpoints"), service.block_points);
    if (const auto* trains = node.child("servicetrains")) {
        for (const auto& train : trains->children)
            if (train.name == "train") service.trains.push_back(read_train(train));
    }
    read_events(node.child("serviceevents"), service.events);
    return service;
}

[[nodiscard]] std::string stem_of(const std::string_view file) {
    const auto dot = file.rfind('.');
    return std::string(dot == std::string_view::npos ? file : file.substr(0, dot));
}

} // namespace

const ActivityNode* ActivityNode::child(const std::string_view key) const {
    for (const auto& node : children)
        if (node.name == key) return &node;
    return nullptr;
}

std::string ActivityNode::text(const std::string_view key) const {
    const auto* node = child(key);
    return node && !node->values.empty() ? node->values.front() : std::string();
}

bool ActivityTrain::flag(const std::string_view key) const { return has_flag(flags, key); }

bool ActivityService::flag(const std::string_view key) const { return has_flag(flags, key); }

const ActivityTrain* ActivityService::train(const std::uint32_t train_id) const {
    for (const auto& train : trains)
        if (train.id == train_id) return &train;
    return nullptr;
}

const ActivityMenu* Activity::menu(const std::int32_t id) const {
    for (const auto& menu : menus)
        if (menu.id == id) return &menu;
    return nullptr;
}

const ActivityService* Activity::service(const std::uint32_t id) const {
    for (const auto& service : services)
        if (service.id == id) return &service;
    return nullptr;
}

const ActivityService* Activity::player_service() const {
    if (player_train) return service(player_train->service);
    const ActivityService* any = nullptr;
    for (const auto& service : services) {
        if (service.type != ActivityServiceType::player) continue;
        if (!any) any = &service;
        if (!service.trains.empty() && service.trains.front().start_time) return &service;
    }
    return any;
}

std::size_t Activity::event_count() const {
    std::size_t count = global_events.size();
    for (const auto& service : services) count += service.events.size();
    return count;
}

std::optional<std::int32_t> parse_activity_time(const std::string_view text) {
    std::int32_t parts[3]{};
    std::size_t count = 0;
    std::size_t at = 0;
    while (at <= text.size() && count < 3) {
        const auto colon = text.find(':', at);
        const auto piece = text.substr(at, colon == std::string_view::npos ? std::string_view::npos : colon - at);
        if (piece.empty() || piece.find_first_not_of("0123456789") != std::string_view::npos) return std::nullopt;
        parts[count++] = std::atoi(std::string(piece).c_str());
        if (colon == std::string_view::npos) break;
        at = colon + 1;
    }
    if (count < 2) return std::nullopt;
    return parts[0] * 3600 + parts[1] * 60 + parts[2];
}

Activity parse_activity(const std::string_view bytes) {
    Activity activity;
    std::string error;
    const auto text = detail::decode_text(bytes, error);
    if (!text) {
        activity.error = error;
        return activity;
    }
    TextNode root;
    if (!detail::read_text_tree(*text, root)) {
        activity.error = "no blocks";
        return activity;
    }
    const auto* header = root.child("tr_activity_header");
    if (!header) {
        activity.error = "no Tr_Activity_Header";
        return activity;
    }
    activity.route_id = first(header->child("routeid"));
    activity.name = first(header->child("name"));
    activity.description = first(header->child("description"));
    activity.starts_in = first(header->child("activitystartsin"));
    activity.ends_in = first(header->child("activityendsin"));
    activity.start_time = parse_activity_time(first(header->child("activitystarts"))).value_or(activity.start_time);
    activity.duration = parse_activity_time(first(header->child("duration"))).value_or(0);
    activity.season = std::clamp(integer(header->child("season"), 1), 0, 3);
    activity.weather = std::clamp(integer(header->child("weather"), 0), 0, 2);
    if (const auto* train = header->child("playerstartsintrain"); train && train->values.size() >= 2) {
        activity.player_train = Activity::PlayerTrain{.service = whole(train, 0), .train = whole(train, 1), .wagon = whole(train, 2),
                                                      .area = train->values.size() > 3 ? train->values[3] : std::string()};
    }
    if (const auto* tile = header->child("playerstartsintile"); tile && tile->values.size() >= 2) {
        if (const auto* position = header->child("playerstartposition"); position && position->values.size() >= 3) {
            activity.player_position = ActivityPoint{.tile_x = static_cast<std::int32_t>(tile->number(0)),
                                                     .tile_z = static_cast<std::int32_t>(tile->number(1)),
                                                     .x = position->number(0),
                                                     .y = position->number(1),
                                                     .z = position->number(2)};
        }
    }
    read_events(root.child("globalevents"), activity.global_events);
    // The scenario's parameters (activity.pdf ch. 2; RTS 0x470A3C).
    const auto parameter = [&](const std::string_view key) -> const TextNode* {
        if (const auto* node = root.child(key)) return node;
        return header->child(key);
    };
    if (const auto* value = parameter("trainsdistanceofnoreturn"); value && !value->values.empty())
        activity.no_return = static_cast<float>(value->number(0));
    activity.no_return = (std::max)(activity.no_return, 250.0f);
    if (const auto* table = parameter("trafficlimitstable")) {
        std::vector<ActivityTrafficLimit> limits;
        for (const auto& limit : table->children) {
            if (limit.name != "trafficlimit" || limit.values.size() < 2 || limits.size() >= 10) continue;
            const auto speed = static_cast<float>(limit.number(1));
            if (speed > 0.0f) limits.push_back({.radius = static_cast<float>(limit.number(0)), .speed = speed});
        }
        activity.traffic_limits = std::move(limits);
    }
    read_block_points(parameter("globalblockpoints"), activity.global_block_points);
    if (const auto* objects = root.child("staticobjects")) {
        for (const auto& node : objects->children) {
            if (node.name != "object") continue;
            ActivityObject object;
            object.type = first(node.child("type"));
            if (const auto* shape = node.child("shape")) {
                object.file = first(shape);
                object.location = shape->values.size() > 1 ? static_cast<int>(shape->number(1, 1.0)) : 1;
            }
            object.visible = integer(node.child("visible"), 1) != 0;
            object.sound_type = first(node.child("soundtype"));
            object.wagon_id = first(node.child("soundwagonid"));
            activity.objects.push_back(std::move(object));
        }
    }
    if (const auto* menus = root.child("activitymenus")) {
        for (const auto& node : menus->children) {
            if (node.name != "activitymenu") continue;
            ActivityMenu menu;
            menu.id = integer(node.child("menuid"), 0);
            menu.name = first(node.child("name"));
            menu.main = integer(node.child("mainactivitymenu"), 0) != 0;
            if (const auto* items = node.child("menuitems")) {
                for (const auto& item : items->children) {
                    if (item.name != "menuitem") continue;
                    menu.items.push_back({.label = first(item.child("itemlabel")),
                                          .id = integer(item.child("menuitemid"), 0),
                                          .enabled = integer(item.child("enabled"), 1) != 0,
                                          .visible = integer(item.child("visible"), 1) != 0});
                }
            }
            activity.menus.push_back(std::move(menu));
        }
    }
    if (const auto* services = root.child("services")) {
        for (const auto& node : services->children)
            if (node.name == "service") activity.services.push_back(read_service(node));
    }
    return activity;
}

std::optional<core::VirtualPath> activity_folder(const core::VirtualPath& route, const std::string_view file) {
    return route.resolve("RTS_ACT/" + stem_of(file));
}

std::vector<ActivityFile> load_activities(const core::Vfs& vfs, const core::VirtualPath& route) {
    std::vector<ActivityFile> found;
    const auto folder = route.resolve(std::string_view("RTS_ACT"));
    if (!folder) return found;
    for (const auto& entry : vfs.list(*folder)) {
        if (entry.directory) continue;
        const std::string name(entry.path.filename());
        if (name.size() < 5 || detail::lower_ascii(name.substr(name.size() - 4)) != ".act") continue;
        const auto bytes = vfs.read(entry.path);
        ActivityFile file{.file = stem_of(name), .path = entry.path, .activity = {}};
        file.activity = bytes ? parse_activity(*bytes) : Activity{.error = "cannot read"};
        found.push_back(std::move(file));
    }
    std::sort(found.begin(), found.end(), [](const ActivityFile& a, const ActivityFile& b) {
        return core::fold_case(a.file) < core::fold_case(b.file);
    });
    return found;
}

std::optional<ActivityFile> load_activity(const core::Vfs& vfs, const core::VirtualPath& route, const std::string_view name) {
    std::optional<core::VirtualPath> path;
    if (name.find('/') != std::string_view::npos || name.find('\\') != std::string_view::npos) {
        std::string text(name);
        std::replace(text.begin(), text.end(), '\\', '/');
        path = core::VirtualPath::parse(text);
    } else {
        auto stem = stem_of(name);
        if (detail::lower_ascii(std::string(name.substr(stem.size()))) != ".act") stem = std::string(name);
        path = route.resolve("RTS_ACT/" + stem + ".act");
    }
    if (!path || !vfs.exists(*path)) return std::nullopt;
    std::string error;
    const auto bytes = vfs.read(*path, &error);
    ActivityFile file{.file = stem_of(path->filename()), .path = *path, .activity = {}};
    file.activity = bytes ? parse_activity(*bytes) : Activity{.error = error};
    return file;
}

} // namespace openrts::formats
