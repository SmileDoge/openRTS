#include <iostream>

#include <openrts/simulation/simulation.hpp>

int main() {
    std::cout << "openRTS game skeleton; simulation: "
              << openrts::simulation::module_name() << '\n';
    return 0;
}
