#include "Settings.h"
#include "Carat.h"
#include "profiler/Profiler.h"
#include <cstdlib>
#include <string>
#include <vector>

#ifndef CARAT_GIT_COMMIT
#define CARAT_GIT_COMMIT "unknown"
#endif

namespace {

const char *AlgorithmName(car::MCAlgorithm alg) {
    switch (alg) {
    case car::MCAlgorithm::FCAR: return "fcar";
    case car::MCAlgorithm::BCAR: return "bcar";
    case car::MCAlgorithm::BMC: return "bmc";
    case car::MCAlgorithm::WLBMC: return "wlbmc";
    case car::MCAlgorithm::KIND: return "kind";
    case car::MCAlgorithm::IC3: return "ic3";
    case car::MCAlgorithm::L2S: return "l2s";
    case car::MCAlgorithm::KLIVE: return "klive";
    case car::MCAlgorithm::FAIR: return "fair";
    case car::MCAlgorithm::KFAIR: return "kfair";
    case car::MCAlgorithm::RLIVE: return "rlive";
    default: return "unknown";
    }
}

const char *SolverName(car::MCSATSolver slv) {
    switch (slv) {
    case car::MCSATSolver::minisat: return "minisat";
    case car::MCSATSolver::cadical: return "cadical";
    case car::MCSATSolver::minicore: return "minicore";
    case car::MCSATSolver::kissat: return "kissat";
    default: return "unknown";
    }
}

// Protocol result codes (docs/profiler_protocol.md §3).
uint64_t ResultCode(car::CheckResult res) {
    switch (res) {
    case car::CheckResult::Safe: return 1;
    case car::CheckResult::Unsafe: return 2;
    default: return 0;
    }
}

}  // namespace

int main(int argc, char **argv) {
    car::Settings settings;
    if (!car::ParseSettings(argc, argv, settings)) return EXIT_FAILURE;

    car::prof::ProfilerConfig prof_cfg;
    prof_cfg.enabled =
        !settings.traceSocket.empty() || !settings.traceFile.empty();
    prof_cfg.detail = settings.traceLevel == "detail";
    prof_cfg.socket_path = settings.traceSocket;
    prof_cfg.file_path = settings.traceFile;
    prof_cfg.algorithm = AlgorithmName(settings.alg);
    prof_cfg.algorithm_code = static_cast<uint8_t>(settings.alg);
    prof_cfg.model_path = settings.aigFilePath;
    prof_cfg.sat_backend = SolverName(settings.solver);
    prof_cfg.carat_commit = CARAT_GIT_COMMIT;
    for (int i = 0; i < argc; ++i) prof_cfg.argv.push_back(argv[i]);
    car::prof::Profiler::Get().Start(prof_cfg);

    car::Carat app(settings);
    if (!app.LoadModel()) {
        car::prof::Profiler::Get().Shutdown(0, 1);  // 1 = load failure
        return EXIT_FAILURE;
    }
    if (!settings.wlBitblastOutputPath.empty()) {
        car::prof::Profiler::Get().Shutdown(1, 0);
        return EXIT_SUCCESS;
    }
    const car::CheckResult res = app.Prove();
    car::prof::Profiler::Get().Shutdown(ResultCode(res), 0);
    return 0;
}
