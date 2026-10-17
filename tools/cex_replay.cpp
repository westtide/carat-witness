#include "Btor2Frontend.h"
#include "Btor2Witness.h"
#include "WLSimulator.h"

#include <iostream>
#include <stdexcept>

// Internal diagnostic: independently parse and replay the standard artifact.
// Does not solve, complete missing choices, or produce another artifact.
int main(int argc, char **argv) {
    try {
        if (argc != 3) throw std::runtime_error("usage: carat-replay MODEL CEX");
        const auto ir = car::Btor2Frontend::LoadIR(argv[1]);
        const auto trace = car::ReadBtor2Counterexample(ir, argv[2]);
        const auto result = car::WLSimulator(ir).Verify(trace);
        if (result.kind != car::WLSimulator::VerificationKind::Confirmed)
            throw std::runtime_error("frame " + std::to_string(result.time) +
                                     " node " + std::to_string(result.nodeId) + ": " + result.reason);
        std::cout << "Confirmed original BTOR2 bad at frame " << result.time << '\n';
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "carat-replay: " << error.what() << '\n';
        return 1;
    }
}
