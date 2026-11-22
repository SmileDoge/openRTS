#include <openrts/server/session.hpp>

#include <chrono>
#include <cstdio>

#include <openrts/core/core.hpp>
#include <openrts/core/user_data.hpp>
#include <openrts/core/virtual_path.hpp>
#include <openrts/server/console.hpp>

namespace openrts::server {

std::unique_ptr<ServerSession> ServerSession::start(SessionSettings settings, std::string& error, std::vector<std::string>& lines) {
    if (settings.data.empty()) settings.data = core::find_user_data_directory();
    std::unique_ptr<ServerSession> session(new ServerSession());
    auto& s = *session;
    s.settings_ = settings;
    const auto loading = std::chrono::steady_clock::now();
    s.route_ = ServerRoute::load(settings.root, settings.route, error, settings.data);
    if (!s.route_) return nullptr;
    auto& route = *s.route_;
    for (const auto& note : route.notes()) lines.push_back("note: " + note);
    const auto& counts = route.counts();
    char text[400];
    std::snprintf(text, sizeof text, "route %s (RouteID %s): %u track nodes, %u track items, %u substations, %u electrosections; %s, %zu paths; %.1f s",
                  route.folder().c_str(), route.route_id().c_str(), counts.nodes, counts.items, counts.substations, counts.sections,
                  route.signals() ? (std::to_string(route.signals()->heads().size()) + " signal heads").c_str() : "no signals",
                  route.paths().size(), std::chrono::duration<double>(std::chrono::steady_clock::now() - loading).count());
    lines.emplace_back(text);
    if (!route.signal_modes().empty()) lines.push_back("signals: " + route.signal_modes());

    s.tcp_ = std::make_unique<TcpServer>();
    if (!s.tcp_->listen(settings.port, error, settings.local)) return nullptr;
    ServerOptions options;
    options.route_id = core::fold_case(route.route_id());
    options.description = game_description(route.route_id(), settings.season);
    options.season = settings.season;
    // A thunderstorm is rain to RTS (the login's weather), a storm to openRTS.
    options.weather = settings.weather == 3 ? std::uint8_t{2} : settings.weather;
    options.storm = settings.weather == 3;
    options.weather_dynamic = settings.weather_dynamic;
    options.counts = counts;
    options.time_of_day = settings.time_of_day;
    options.admit = settings.admit;
    options.consoles = true;
    options.dispatcher_password = settings.dispatcher_password;
    options.discord_join = settings.discord_join;
    options.public_address = settings.public_address;
    options.max_players = settings.max_players;
    s.game_ = std::make_unique<GameServer>(*s.tcp_, options, route.network(), route.signals());
    auto& game = *s.game_;
    // A console's commands are the server's own, but "quit".
    game.set_console_commands([&game, &route](const std::string_view line) {
        const auto words = command_words(line);
        if (!words.empty() && (core::fold_case(words[0]) == "quit" || core::fold_case(words[0]) == "exit")) return std::string("not from a console");
        bool ignored = false;
        return run_command(game, route, line, ignored);
    });
    game.set_console_place([&game, &route](const net::ConsolePlace& place) { return place_cars(game, route, place); });
    game.set_station_routes(&route.station_routes());
    if (settings.autosave_minutes > 0.0) keep_previous_autosave(route);
    lines.push_back("listening on port " + std::to_string(s.tcp_->port()));
    return session;
}

ServerSession::~ServerSession() {
    if (game_ && route_ && settings_.autosave_minutes > 0.0) (void)autosave_placement(*game_, *route_);
    game_.reset();
    if (tcp_) tcp_->stop();
}

void ServerSession::step(const double now) {
    for (auto& event : tcp_->take_events()) {
        switch (event.kind) {
        case NetEvent::Kind::connected: game_->connected(event.connection, std::move(event.address)); break;
        case NetEvent::Kind::package: game_->received(event.connection, event.id, event.body); break;
        case NetEvent::Kind::closed: game_->closed(event.connection); break;
        }
    }
    game_->tick(now);
    if (settings_.autosave_minutes > 0.0 && now - autosaved_at_ >= settings_.autosave_minutes * 60.0) {
        autosaved_at_ = now;
        (void)autosave_placement(*game_, *route_);
    }
}

std::string ServerSession::command(const std::string_view line, bool& quit) {
    return run_command(*game_, *route_, line, quit);
}

std::uint16_t ServerSession::port() const {
    return tcp_->port();
}

} // namespace openrts::server
