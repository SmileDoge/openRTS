#pragma once

#include <openrts/simulation/klub.hpp>

// openRTS's САУТ (docs/brakes-dll-replacement.md "The rest", 3), a part
// of the КЛУБ's step (klub.cpp): its keys before the step (CanSwitch), the
// step itself after the ALSN and its delay, before the permitted speed.
namespace openrts::simulation::saut {

void keys(KlubState& s, const KlubSettings& k, const KlubInputs& in, KlubOutput& out);
void step(KlubState& s, const KlubSettings& k, const KlubInputs& in, KlubOutput& out);
// Its part of Switched, after the step.
void switched(KlubState& s, const KlubInputs& in, KlubOutput& out);

} // namespace openrts::simulation::saut
