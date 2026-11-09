#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include <openrts/core/vfs.hpp>
#include <openrts/net/play.hpp>

namespace openrts::viewer {

// A consist of this installation (its name or virtual path) as the console
// sends it: by its cars, so the server needs only the cars' files.
[[nodiscard]] std::optional<net::ConsolePlace> consist_place(const core::Vfs& vfs, const std::string& consist, std::string& error);
}

#ifdef OPENRTS_HAS_IMGUI
namespace openrts::viewer {

class NetSession;
class TrackControl;
class TrainView;

// The dispatcher's console (Пульт ДСП) on an openRTS server (--mp-console,
// docs/server.md): the game as the server holds it (players, trains with
// their owners and routes, the time) and its commands. The map is Track
// control: a junction thrown or a signal's aspect there goes to the server.
class ConsolePanel {
public:
    void draw(NetSession& net, TrainView& trains, TrackControl* map, const core::Vfs& vfs);

private:
    std::uint32_t selected_train_ = 0; // the server's id
    std::uint64_t map_selected_ = 0;   // the map's pick last followed
    std::uint32_t selected_user_ = 0;
    char signal_[64]{};
    char message_[256]{};
    char command_[256]{};
    char time_[16]{};
    char number_[16]{};
    char consist_filter_[64]{};
    std::string consist_;
    std::string path_;
    std::size_t log_seen_ = 0; // log lines shown: scrolls down when more come
};

} // namespace openrts::viewer
#endif
