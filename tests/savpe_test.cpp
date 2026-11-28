// openRTS's САВПЭ, stage 1 (simulation/savpe.hpp): a line's voices read from
// a САВПЭ script's timetables, both ways the installed one makes them (of
// parts, of whole phrases), and the stops given the line that fits them.
#include <iostream>
#include <string>

#include <openrts/formats/sound.hpp>
#include <openrts/simulation/savpe.hpp>

namespace {

int failures = 0;

void check(const bool ok, const std::string& what) {
    if (!ok) {
        ++failures;
        std::cerr << "FAIL: " << what << std::endl;
    }
}

// Two lines: 20 of parts (as Moscow's), 33 of whole phrases (as Kuzbass's).
// (The parser folds the files' Latin letters, as RTS finds files.)
const char* const script = R"(SIMISA@@@@@@@@@@JINX0x1t______
Tr_SMS (
    NamedTriggersTable (
        "MDD_INFO_R20_6501_NUMBER" 6501
        "MDD_INFO_R20_6501_COUNT" 4
        "MDD_INFO_R20_6501_WEL_SOUND" 100
        "MDD_INFO_R20_6501_S1_SOUND" 101
        "MDD_INFO_R20_6501_S1_POS" 0
        "MDD_INFO_R20_6501_S2_SOUND" 102
        "MDD_INFO_R20_6501_S2_POS" 62
        "MDD_INFO_R20_6501_S3_SOUND" 103
        "MDD_INFO_R20_6501_S3_POS" 0
        "MDD_INFO_R20_6501_S4_SOUND" 104
        "MDD_INFO_R20_6501_S4_POS" 80
        "MDD_INFO_R33_6872_NUMBER" 6872
        "MDD_INFO_R33_6872_COUNT" 2
        "MDD_INFO_R33_6872_S1_SOUND" 201
        "MDD_INFO_R33_6872_S1_POS" 0
        "MDD_INFO_R33_6872_S2_SOUND" 202
        "MDD_INFO_R33_6872_S2_POS" 3443
    )
    ScalabiltyGroup( 5
        Streams ( 1
            Stream ( Triggers ( 7
                Discrete_Trigger ( 100 PlayOneShot ( 3 File ( "m/welk.wav" -1 ) File ( "m/aprel.wav" -1 ) File ( "m/vsemi.wav" -1 ) SelectionMethod ( SequentialSelection ) ) )
                Discrete_Trigger ( 101 PlayOneShot ( 3 File ( "m/sled.wav" -1 ) File ( "m/matveev.wav" -1 ) File ( "m/dver.wav" -1 ) SelectionMethod ( SequentialSelection ) ) )
                Discrete_Trigger ( 102 PlayOneShot ( 3 File ( "m/matveev.wav" -1 ) File ( "m/sled.wav" -1 ) File ( "m/ochakovo.wav" -1 ) SelectionMethod ( SequentialSelection ) ) )
                Discrete_Trigger ( 103 PlayOneShot ( 3 File ( "m/sled.wav" -1 ) File ( "m/ochakovo.wav" -1 ) File ( "m/dver.wav" -1 ) SelectionMethod ( SequentialSelection ) ) )
                Discrete_Trigger ( 104 PlayOneShot ( 2 File ( "m/ochakovo.wav" -1 ) File ( "m/konech.wav" -1 ) SelectionMethod ( SequentialSelection ) ) )
                Discrete_Trigger ( 201 PlayOneShot ( 1 File ( "k/slZenkovo.wav" -1 ) SelectionMethod ( SequentialSelection ) ) )
                Discrete_Trigger ( 202 PlayOneShot ( 1 File ( "k/Zenkovo.wav" -1 ) SelectionMethod ( SequentialSelection ) ) )
            ) )
        )
    )
)
)";

} // namespace

int main() {
    using namespace openrts;
    const auto parsed = formats::parse_sound_script(script);
    check(parsed.ok(), "the script parses");
    const auto lines = simulation::savpe_voice_lines(parsed);
    check(lines.size() == 2, "two lines");
    if (lines.size() == 2) {
        const auto& moscow = lines[0];
        check(moscow.route == 20 && moscow.made_of_parts(), "line 20 is made of parts");
        check(moscow.next == "m/sled.wav" && moscow.doors == "m/dver.wav" && moscow.welcome == "m/welk.wav" && moscow.all_stops == "m/vsemi.wav"
                  && moscow.terminus == "m/konech.wav",
              "its common phrases");
        check(moscow.points.size() == 2 && moscow.points[0].km == 6.2f && moscow.points[0].name == "m/matveev.wav"
                  && moscow.points[1].name == "m/ochakovo.wav" && moscow.points[1].approach.size() == 3,
              "its stops: the name, what is said leaving the stop before and arriving");
        const auto& kuzbass = lines[1];
        check(kuzbass.route == 33 && !kuzbass.made_of_parts(), "line 33 is of whole phrases");
        check(kuzbass.points.size() == 1 && kuzbass.points[0].approach == std::vector<std::string>{"k/slzenkovo.wav"}
                  && kuzbass.points[0].arrival == std::vector<std::string>{"k/zenkovo.wav"} && kuzbass.points[0].name.empty(),
              "its stop: the phrases as they are");
    }
    // The stops: the line with most of them near its points, by km.
    std::vector<simulation::SavpeStop> stops(3);
    stops[0].km = 6.3f;
    stops[1].km = 7.95f;
    stops[2].km = 12.0f; // none near
    const int line = simulation::match_savpe_voices(stops, lines);
    check(line == 0, "line 20 fits the stops");
    check(stops[0].voice == "m/matveev.wav" && stops[1].voice == "m/ochakovo.wav" && stops[2].arrival.empty(),
          "each stop its voice within 0.3 km, none further");
    std::vector<simulation::SavpeStop> one(1);
    one[0].km = 344.3f;
    check(simulation::match_savpe_voices(one, lines) == -1, "a single stop fits no line (two at least)");
    if (failures == 0) std::cout << "savpe: ok" << std::endl;
    return failures == 0 ? 0 : 1;
}
