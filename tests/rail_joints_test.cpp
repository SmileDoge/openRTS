// The rail joints along the track (simulation/rail_joints): RTS's layout as
// before, openRTS's jointed track every 25 m and welded strings with
// equalizing spans of two or three 12.5 m rails, the same for a node every
// time; where a bogie stands among them either way it goes.
#include <cmath>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

#include <openrts/simulation/rail_joints.hpp>

namespace {

int failures = 0;

void check(const bool condition, const std::string& what) {
    if (!condition) {
        ++failures;
        std::cerr << "FAILED: " << what << '\n';
    }
}

bool near(const double a, const double b) { return std::abs(a - b) < 1e-6; }

} // namespace

int main() {
    using namespace openrts::simulation;

    {
        // RTS: every 50 m; over 2000 m four every 600 m (0, 12.5, 25, 37.5).
        const RailJoints rts;
        check(rts.layout() == RailJointLayout::rts, "RTS's layout unless set");
        check(near(rts.place(1, 1500.0, 75.0, true).index, 1.0) && near(rts.place(1, 1500.0, 75.0, true).since, 25.0),
              "RTS: 75 m on a short node is past the joint at 50 m");
        check(near(rts.place(1, 1500.0, 75.0, false).since, 25.0), "RTS: the distance counts towards greater offsets either way");
        const double offsets[] = {5.0, 13.0, 26.0, 38.0, 599.0, 601.0};
        const double indices[] = {0.0, 1.0, 2.0, 3.0, 3.0, 4.0};
        const double since[] = {5.0, 0.5, 1.0, 0.5, 561.5, 1.0};
        for (int k = 0; k < 6; ++k) {
            const auto place = rts.place(1, 5000.0, offsets[k], true);
            check(near(place.index, indices[k]) && near(place.since, since[k]), "RTS on a long node at " + std::to_string(offsets[k]));
        }
    }
    {
        // openRTS, jointed: 25 m rails from the node's start.
        const auto joints = RailJoints::lay_out(7, 500.0);
        bool every_25 = joints.size() == 19;
        for (std::size_t k = 0; every_25 && k < joints.size(); ++k) every_25 = near(joints[k], 25.0 * static_cast<double>(k + 1));
        check(every_25, "openRTS: a 500 m node has a joint every 25 m, not at its ends");
    }
    {
        // openRTS, welded: groups 12.5 m apart, strings between them.
        const auto joints = RailJoints::lay_out(7, 5000.0);
        std::vector<std::size_t> groups{1};
        double shortest_string = 1e9, longest_string = 0.0;
        bool gaps_ok = true;
        for (std::size_t k = 1; k < joints.size(); ++k) {
            const double gap = joints[k] - joints[k - 1];
            if (near(gap, 12.5)) {
                ++groups.back();
            } else {
                gaps_ok = gaps_ok && gap > 500.0;
                shortest_string = (std::min)(shortest_string, gap);
                longest_string = (std::max)(longest_string, gap);
                groups.push_back(1);
            }
        }
        std::cerr << "  welded 5000 m: " << joints.size() << " joints in " << groups.size() << " groups, strings "
                  << shortest_string << " to " << longest_string << " m\n";
        check(gaps_ok, "openRTS: joints 12.5 m apart in a span, strings of over 500 m between spans");
        bool spans_ok = groups.size() >= 5;
        for (std::size_t g = 1; g + 1 < groups.size(); ++g) spans_ok = spans_ok && (groups[g] == 3 || groups[g] == 4);
        check(spans_ok, "openRTS: the spans between strings have 3 or 4 joints (2 or 3 short rails)");
        check(shortest_string > 550.0 && longest_string < 1100.0, "openRTS: strings of about 800 m");
        check(joints.front() > 0.25 && joints.back() < 5000.0 - 0.25, "openRTS: not at the node's ends (its junction's)");
        check(RailJoints::lay_out(7, 5000.0) == joints, "openRTS: a node's joints the same every time");
        check(RailJoints::lay_out(8, 5000.0) != joints, "openRTS: another node's not the same");
    }
    {
        // Where a bogie stands, going either way.
        RailJoints joints;
        joints.set_layout(RailJointLayout::openrts, {0.0, 500.0, 5000.0});
        check(joints.layout() == RailJointLayout::openrts, "openRTS set");
        const auto up = joints.place(1, 500.0, 30.0, true);
        check(near(up.index, 1.0) && near(up.since, 5.0), "openRTS: going up, 5 m past the joint at 25 m");
        check(near(joints.place(1, 500.0, 30.0, false).since, 20.0), "openRTS: going down, 20 m past the joint at 50 m");
        check(std::isinf(joints.place(1, 500.0, 10.0, true).since), "openRTS: none passed yet");
        // Stepping along the welded node: one change of stretch a joint.
        double index = joints.place(2, 5000.0, 0.0, true).index;
        std::size_t passed = 0;
        for (double at = 0.5; at < 5000.0; at += 0.5) {
            const double now = joints.place(2, 5000.0, at, true).index;
            if (now != index) ++passed;
            index = now;
        }
        check(passed == RailJoints::lay_out(2, 5000.0).size(), "openRTS: every joint is passed once along the node");
        joints.set_layout(RailJointLayout::rts, {});
        check(near(joints.place(1, 1500.0, 75.0, true).index, 1.0), "back to RTS's");
    }
    if (failures == 0) std::cout << "rail joints: all checks passed\n";
    return failures == 0 ? 0 : 1;
}
