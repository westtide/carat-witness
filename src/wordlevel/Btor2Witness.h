#pragma once

#include "model/Btor2IR.h"
#include "WLTrace.h"
#include <string>

namespace car {

// Concrete standard BTOR2 witness. Declaration ordinals resolve through the
// original model. No implicit completion; WLSimulator checks init/next/bad.
WLTrace ReadBtor2Counterexample(const Btor2IR &ir, const std::string &path);

// Deterministically replay and export the complete original-model interface.
// No JSON sidecar, solver call, or persistent derived trace. Refuse overwrite.
bool WriteBtor2Counterexample(const Btor2IR &ir, const WLTrace &trace,
                              const std::string &cexPath, std::string &error);
} // namespace car
