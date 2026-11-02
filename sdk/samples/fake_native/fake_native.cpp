// The native counterpart of dllhost/test/fake_loco.cpp, written against the
// openRTS plugin API (include/openrts/plugin/train_plugin.hpp). The parity
// test (tests/plugin_parity_test.cpp) drives both through one script and
// requires the same observable behaviour: what a legacy DLL does with raw
// ts.h objects, a native plugin does with named controls and host calls.

#include <openrts/plugin/train_plugin.hpp>

namespace {

class FakeNative final : public orts::Locomotive {
public:
    FakeNative(orts::Host host, const OrtsTrainSetup&) : orts::Locomotive(host) {
        throttle_ = this->host().control("sw0");
        locked_ = this->host().control("sw1");
        reverser_ = this->host().control("sw2");
        this->host().set(throttle_, 1.0f); // as Init's SetSwitch(.., SetBoth): no change event
    }

    void step(double, const OrtsTrainInputs& in, OrtsTrainOutputs& out) override {
        out.traction_force = 1000.0f * static_cast<float>(throttle_position_) * static_cast<float>(reverse_);
        out.brake_force = in.speed; // echo, as the fake DLL
        out.equalising = -1.0f;
        out.pantographs = 3;
        out.main_switch = 1;
        if (!greeted_ && (in.flags & ORTS_INPUT_PLAYER)) {
            greeted_ = true;
            host().sound(7, true);
            host().message(2, "fake: init done");
        }
    }

    bool request(OrtsHandle control, float, float to) override { return !(control == locked_ && to == 2.0f); }

    void changed(OrtsHandle control, float) override {
        const float state = host().state(control);
        if (control == throttle_) throttle_position_ = static_cast<int>(state);
        if (control == reverser_) reverse_ = state == 1.0f ? 1 : (state == 2.0f ? -1 : 0);
        host().sound(100u + static_cast<std::uint32_t>(control), true);
    }

private:
    OrtsHandle throttle_ = -1, locked_ = -1, reverser_ = -1;
    int throttle_position_ = 0;
    int reverse_ = 0;
    bool greeted_ = false;
};

} // namespace

ORTS_TRAIN_PLUGIN(FakeNative, "fake native locomotive")
