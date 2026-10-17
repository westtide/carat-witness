#pragma once

#include "model/Btor2IR.h"
#include "WLTrace.h"
#include <string>

namespace car {

// Strict original-declaration-ordinal, concrete BTOR2 witness reader. No zero
// completion or solver invocation; Verify checks transitions/constraints/bad.
WLTrace ReadBtor2Counterexample(const Btor2IR &ir, const std::string &path);

// Export only after the same SourceIR Verify that produces the observations.
// If originalCex is present, preserve its bytes rather than reserializing it.
// Both final paths must be absent; temporary files are removed on any failure.
// The sidecar is published last, with a footer only after Confirmed.
bool WriteObservedCounterexample(const Btor2IR &ir, const WLTrace &trace,
                                const std::string &modelPath,
                                const std::string &cexPath, std::string &error,
                                const std::string &originalCex = {},
                                const std::string &originalCexSha256 = {});
} // namespace car
