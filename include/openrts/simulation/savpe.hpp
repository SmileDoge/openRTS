#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <openrts/formats/sound.hpp>
#include <openrts/simulation/train_route.hpp>

// openRTS's own САВПЭ (docs/savpe.md), stage 1: what it knows of the way.
// The real one keeps a base of stops and the distances between them, counts
// the way by its path sensor and announces the next stop; it does not know
// the train's coordinate. Here its stops come from the train's route (the
// platforms it passes, in order), its voices from the installed САВПЭ's
// sound script (MaDaDev's Usavp.sms: its announcements are made of a
// station's name file and common phrases, so any timetable can be voiced).
namespace openrts::simulation {

class TrackNetwork;

// A stop the route passes: a platform's far end (where the head stops).
struct SavpeStop {
    std::string name;  // the platform's station name (TDB)
    bool station = false; // a station (else a halt, о.п.: a name with "пл." / "о.п." / "км")
    std::size_t leg{};
    double far{};         // leg distance of the far end
    double length{};      // the platform's length, m
    std::uint32_t item = 0xFFFFFFFFu; // that end's TDB item
    float km = -1.0f;     // its milepost (simulation/milepost.hpp), -1 none
    // Its voices (SavpeVoicePoint), empty: none.
    std::string voice;                 // its name alone (lines made of parts)
    std::vector<std::string> approach; // "the next stop is ..." (leaving the stop before)
    std::vector<std::string> arrival;  // at it
};

// The platforms on the route, in order, each once (both ends' passes merged:
// a station's several platforms the route passes count once each).
[[nodiscard]] std::vector<SavpeStop> savpe_stops(const TrackNetwork& network, const TrainRoute& route);

// A line's voices in the САВПЭ's script (from the timetables of its trains:
// MDD_INFO_R<route>_<train>_S<n>_SOUND / _POS): each stop at its milepost,
// with what is said leaving the stop before (S odd, POS 0) and arriving
// (S even, its POS). Lines are made two ways: of parts (the Moscow line 20:
// "следующая" + the name + "двери закрываются"; then `name` is the name's
// file alone), or of whole phrases a stop (line 33: "слЗеньково.wav",
// "Зеньково.wav"; line 37 numbered).
struct SavpeVoicePoint {
    float km{};
    std::string name;
    std::vector<std::string> approach;
    std::vector<std::string> arrival;
};
struct SavpeVoiceLine {
    std::uint32_t route{}; // the script's route number (MDD_INFO_R<route>_...)
    std::vector<std::uint32_t> trains;
    std::string next;      // "следующая остановка"
    std::string doors;     // "осторожно, двери закрываются"
    std::string welcome;   // "добро пожаловать, поезд следует до"
    std::string all_stops; // "со всеми остановками"
    std::string terminus;  // "конечная"
    std::vector<SavpeVoicePoint> points; // by km
    [[nodiscard]] bool made_of_parts() const noexcept { return !next.empty() && !doors.empty(); }
};
[[nodiscard]] std::vector<SavpeVoiceLine> savpe_voice_lines(const formats::SoundScript& script);

// Each stop its name's file from the line that fits the stops best (most
// stops within `tolerance` km of one of its points); the line's index, or
// -1 when none fits at least two stops.
int match_savpe_voices(std::vector<SavpeStop>& stops, const std::vector<SavpeVoiceLine>& lines, float tolerance = 0.3f);

} // namespace openrts::simulation
