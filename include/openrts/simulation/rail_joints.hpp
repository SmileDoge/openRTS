#pragma once

#include <cstdint>
#include <vector>

// The rail joints along the track's vector nodes: where the wheels knock
// (the wagon sound events 3, the body's vertical knock; Train). See
// docs/reverse/rolling-stock.md.
namespace openrts::simulation {

enum class RailJointLayout : std::uint8_t {
    // RTS (0x4F5B34): every 50 m along a node, or on a node over 2000 m
    // (welded) a group at 0, 12.5, 25 and 37.5 m every 600 m.
    rts,
    // openRTS: a node under 1000 m is jointed track, 25 m rails; a longer
    // one welded strings of about 800 m with equalizing spans of two or
    // three 12.5 m rails between them and at both ends, laid out per node
    // from its index (the same at every run and on every client).
    openrts,
};

// Where a bogie stands among its node's joints.
struct RailJointPlace {
    double index{}; // the stretch between two joints: changes as one is passed
    double since{}; // m since the joint last passed (infinity: none)
};

class RailJoints {
public:
    [[nodiscard]] RailJointLayout layout() const noexcept { return layout_; }
    // The layout for the nodes of these lengths (m, by node index).
    void set_layout(RailJointLayout layout, const std::vector<double>& lengths);

    // A bogie on node `node` (`length` m) at `offset`, going towards greater
    // offsets when `increasing`. RTS counts the distance since a joint
    // towards greater offsets whichever way the bogie goes.
    [[nodiscard]] RailJointPlace place(std::uint32_t node, double length, double offset, bool increasing) const;

    // openRTS's joints along a node of `length` m, by offset (the node's
    // ends, where its junction or end is, left out).
    [[nodiscard]] static std::vector<double> lay_out(std::uint32_t node, double length);

private:
    RailJointLayout layout_ = RailJointLayout::rts;
    std::vector<std::vector<double>> joints_; // openRTS: by node index
};

} // namespace openrts::simulation
