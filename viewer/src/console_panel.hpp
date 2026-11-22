#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <openrts/core/vfs.hpp>
#include <openrts/net/placement.hpp>
#include <openrts/net/play.hpp>

namespace openrts::viewer {

// A consist of this installation (its name or virtual path) as the console
// sends it: by its cars, so the server needs only the cars' files.
[[nodiscard]] std::optional<net::ConsolePlace> consist_place(const core::Vfs& vfs, const std::string& consist, std::string& error);
// A new train's kind and state: the lists (ImGui builds), the kind as RTS's.
void new_train_kind_and_state(int& kind, int& state);
// Its chocks: 0 none, 1 under its head, 2 its tail, 3 both (RTS's PlaceDrag).
void new_train_chocks(int& chocks);
// Chocks under a place's head (1) and tail (2), beside the consist's own.
void put_chocks(net::ConsolePlace& place, std::uint8_t chocks);
[[nodiscard]] int new_train_kind(int choice);
}

#ifdef OPENRTS_HAS_IMGUI
namespace openrts::viewer {

class NetSession;
class TrackControl;
class TrainView;

// The dispatcher's console (Пульт ДСП) on an openRTS server (--mp-console,
// docs/server.md): the game as the server holds it (players, trains with
// their owners and routes, the time) and its commands; station routes and
// signals as the server's window has them. The map is Track control: a
// junction thrown or a signal's aspect there goes to the server.
class ConsolePanel {
public:
    void draw(NetSession& net, TrainView& trains, TrackControl* map, const core::Vfs& vfs);

private:
    // "Станционные маршруты" (the server window's, rts_server.exe's
    // "Собрать маршрут"): the picked train's queue, what it can take next
    // (asked from the server), every train's queue.
    void station_routes(NetSession& net, const net::ConsoleState& state);
    void set_picked(NetSession& net, const net::ConsoleNext& next);
    // "Сигналы": every signal of a type, the defaults, the save.
    void signals(NetSession& net, const TrainView& trains);

    bool back_ = false;                                      // the routes looked for the other way
    std::optional<std::pair<std::uint32_t, bool>> picked_;   // a route's ID, from its end
    bool hold_ = false;
    int priority_ = 0;
    std::string next_key_;     // the train and its queue's end the routes were asked for
    double next_asked_ = -10.0; // ImGui time
    std::vector<std::string> types_; // the route's signal types
    bool types_read_ = false;
    int type_ = -1;
    std::uint32_t selected_train_ = 0; // the server's id
    std::uint64_t map_selected_ = 0;   // the map's pick last followed
    std::uint32_t selected_user_ = 0;
    char signal_[64]{};
    char message_[256]{};
    char command_[256]{};
    char time_[16]{};
    char number_[16]{};
    // A new train's name, kind (STATIC, PLAYER, LOOSE, TRAFFIC) and state
    // (cold, on, running): STATIC and cold by default (the organiser,
    // 2026-11-21).
    char new_name_[128]{};
    int new_kind_ = 0;
    int new_state_ = 0;
    int new_chocks_ = 0;
    // Placements: a name; the trains of the one shown (the server's or one
    // kept here) with the ones picked to put; what the next file from the
    // server is for.
    void placements(NetSession& net, TrainView& trains);
    char placement_name_[80]{};
    std::vector<net::PlacementTrain> shown_;
    std::vector<char> placement_picked_;
    std::string shown_from_;
    // Where the one shown is: the server's (by name) or here (by name); its
    // includes; the picked saved as another placement.
    std::string shown_server_, shown_local_;
    std::vector<std::string> shown_includes_;
    char subset_name_[80]{};
    enum class Awaited { none, show, keep } awaited_ = Awaited::none;
    std::string keep_as_;
    bool clear_first_ = false;
    std::vector<std::string> local_;
    bool local_listed_ = false;
    // The chosen train's form: its number, name and notes (0x461A), loaded
    // when another train is chosen; another dispatcher's change taken where
    // this one is not edited.
    std::uint32_t form_train_ = 0;
    char form_number_[16]{};
    char form_name_[256]{};
    char form_notes_[6144]{}; // within RTS's 0x3C00 a package, as UTF-16
    std::string form_name_seen_, form_notes_seen_;
    char consist_filter_[64]{};
    std::string consist_;
    std::string path_;
    std::size_t log_seen_ = 0; // log lines shown: scrolls down when more come
};

} // namespace openrts::viewer
#endif
