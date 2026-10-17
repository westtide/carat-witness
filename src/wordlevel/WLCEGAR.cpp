#include "WLCEGAR.h"
#include "CheckerFactory.h"

#include "Log.h"
#include "Model.h"
#include "model/WLModel.h"
#include "MemoryBMC.h"
#include "WLSimulator.h"
#include "model/Bitblastor.h"
#include "model/PackageResize.h"
#include "profiler/Profiler.h"

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <vector>

namespace car {

struct WLCEGAR::AbstractionContext {
    explicit AbstractionContext(ArrayAbstraction::BuildResult build)
        : build(std::move(build)) {}
    WordLayout layout;
    ArrayAbstraction::BuildResult build;
    std::unique_ptr<PackageResize> resize;
    // Destroy checker before Model, which owns the AIG; metadata outlives both.
    std::unique_ptr<Model> model;
    std::unique_ptr<BaseAlg> checker;
};

WLCEGAR::WLCEGAR(const Settings &settings,
                 Log &log,
                 WLModel &model)
    : m_settings(settings),
      m_log(log),
      m_model(model), m_abstraction(model.PropertyIR()) {
    if (GetMCAlgorithmProperty(m_settings.alg) != MCAlgorithmProperty::Safety)
        throw std::runtime_error("shared-array CEGAR requires a safety checker");
    m_abstractionContext = BuildAbstractionContext({});
}

std::unique_ptr<WLCEGAR::AbstractionContext> WLCEGAR::BuildAbstractionContext(
    ArrayAbstraction::Precision precision) {
    const auto start = std::chrono::steady_clock::now();
    if (!m_settings.wlBitblastOutputPath.empty())
        throw std::runtime_error("checking build cannot be used for AIG export");
    auto context = std::make_unique<AbstractionContext>(m_abstraction.Build(precision));
    if (context->build.IR().HasArrays())
        throw std::runtime_error("array abstraction produced an array-valued word-level model");
    {
        prof::ScopedSpan resize_span(prof::Stage::MODEL_PACKAGE_RESIZE);
        if (!m_settings.wlDisablePackageResize)
            context->resize = std::make_unique<PackageResize>(context->build.IR());
    }
    const auto &bitblastIR = context->resize ? context->resize->IR() : context->build.IR();
    {
        prof::ScopedSpan bitblast_span(prof::Stage::MODEL_BITBLAST);
        auto aig = GenerateWLAig(bitblastIR, context->layout);
        prof::ModelStat(prof::MSTAT_AIG_INPUTS, aig->num_inputs);
        prof::ModelStat(prof::MSTAT_AIG_LATCHES, aig->num_latches);
        prof::ModelStat(prof::MSTAT_AIG_ANDS, aig->num_ands);
        prof::ModelStat(prof::MSTAT_ARRAY_SELECTORS,
                        m_abstraction.SelectorCount(context->build));
        prof::ModelStat(prof::MSTAT_ARRAY_SLOTS,
                        m_abstraction.SlotCount(context->build));
        context->model = std::make_unique<Model>(m_settings, m_log, std::move(aig));
    }
    LOG_L(m_log, 1, "word-level model build: ms=",
          std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count(),
          " selectors=", m_abstraction.SelectorCount(context->build),
          " slots=", m_abstraction.SlotCount(context->build));
    {
        prof::ScopedSpan checker_span(prof::Stage::MODEL_INIT_CHECKER);
        context->checker = CreateBitLevelChecker(m_settings, *context->model, m_log);
        if (!context->checker) throw std::runtime_error("word-level CEGAR requires a bit-level checker.");
    }
    return context;
}

WLTrace WLCEGAR::RecoverChoices(
    const std::vector<std::pair<Cube, Cube>> &trace) const {
    const auto &context = *m_abstractionContext;
    const auto &bitblastIR = context.resize ? context.resize->IR() : context.build.IR();
    auto choices = RecoverWLCheckerChoices(bitblastIR, *context.model->GetAiger(),
        context.model->GetEquivalenceMap(), context.model->TrueId(),
        context.layout, trace, m_settings.wlValidateAigTrace);
    if (context.resize) choices = context.resize->RestoreTrace(choices);
    return choices;
}

WLCEGAR::~WLCEGAR() = default;

unsigned WLCEGAR::MaxDelay() const {
    return m_abstraction.MaxDelay(m_abstractionContext->build);
}

bool WLCEGAR::ReloadModel(const std::vector<ArrayAbstraction::TrackingTarget> &targets) {
    try {
        auto precision = m_abstraction.ExtendPrecision(
            m_abstractionContext->build, targets);
        if (!precision) return false;
        auto replacement = BuildAbstractionContext(std::move(*precision));
        m_abstractionContext.swap(replacement);
    } catch (const std::exception &error) {
        LOG_L(m_log, 0, "word-level refinement reload failed: ", error.what());
        return false;
    }
    return true;
}

CheckResult WLCEGAR::Run() {
    CheckResult res = CheckResult::Unknown;
    unsigned refinements = 0;
    unsigned cegar_round = 0;
    m_trace = {};

    const auto confirm = [&](WLTrace candidate) {
        m_model.RestoreSourceTrace(candidate);
        const auto verified = WLSimulator(m_model.SourceIR()).Verify(candidate);
        if (verified.kind != WLSimulator::VerificationKind::Confirmed) {
            LOG_L(m_log, 0, "word-level SourceIR verification Unknown at frame ",
                  verified.time, " node ", verified.nodeId, ": ", verified.reason);
            return CheckResult::Unknown;
        }
        LOG_L(m_log, 1, "word-level concrete counterexample confirmed by SourceIR Verify at depth ", candidate.steps.size() - 1);
        m_trace = std::move(candidate);
        return CheckResult::Unsafe;
    };
    const auto elapsed = [](auto start) {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    };
    while (true) {
        // Each iteration proves or refutes the current finite abstraction.
        prof::ScopedSpan round_span(prof::Stage::CEGAR_ROUND, cegar_round);
        if (!m_abstractionContext->checker) return CheckResult::Unknown;
        const auto checkerStart = std::chrono::steady_clock::now();
        {
            prof::ScopedSpan check_span(prof::Stage::CEGAR_CHECK, cegar_round);
            res = m_abstractionContext->checker->Run();
        }
        LOG_L(m_log, 1, "word-level checker: ms=", elapsed(checkerStart));

        if (res == CheckResult::Unsafe) {
            // Recover the current abstract AIG choices for unified IR simulation.
            // Only a verified concrete SourceIR trace is Unsafe.
            try {
                prof::ScopedSpan analyze_span(prof::Stage::CEGAR_CEX_ANALYZE,
                                              cegar_round);
                auto trace = m_abstractionContext->checker->GetCexTrace();
                auto simulationStart = std::chrono::steady_clock::now();
                auto choices = RecoverChoices(trace);
                auto analysis = m_abstraction.AnalyzeCounterexample(m_abstractionContext->build, choices);
                LOG_L(m_log, 1, "word-level greedy: ms=", elapsed(simulationStart),
                      " read_corrections=", analysis.readCorrections,
                      " comparison_corrections=", analysis.comparisonCorrections,
                      " trials=", analysis.trials);
                if (analysis.kind == ArrayAbstraction::AnalysisResult::Kind::ConcreteCandidate)
                    return confirm(std::move(analysis.trace));
                if (analysis.kind == ArrayAbstraction::AnalysisResult::Kind::Unknown) {
                    LOG_L(m_log, 0, "word-level greedy Unknown: ", analysis.reason);
                    return CheckResult::Unknown;
                }
                {
                    prof::ScopedSpan refine_span(prof::Stage::CEGAR_REFINE,
                                                 cegar_round);
                    if (!ReloadModel(analysis.targets)) return CheckResult::Unknown;
                }
                ++refinements;
                LOG_L(m_log, 1, "word-level array refinement ", refinements,
                      " targets=", analysis.targets.size(),
                      " selectors=", m_abstraction.SelectorCount(m_abstractionContext->build),
                      " slots=", m_abstraction.SlotCount(m_abstractionContext->build), " max_delay=", MaxDelay());
            } catch (const std::exception &error) {
                LOG_L(m_log, 0, "word-level simulation/analysis Unknown: ", error.what());
                return CheckResult::Unknown;
            }
            ++cegar_round;
            continue;
        }

        if (res == CheckResult::Safe) {
            const unsigned delay = MaxDelay();
            if (delay == 0) break;
            // Close the finite prefix not covered by the delayed abstraction guards.
            MemoryBMC boundedChecker(m_model, m_log);
            try {
                LOG_L(m_log, 1, "word-level guard prefix check through ", delay - 1);
                const auto prefixStart = std::chrono::steady_clock::now();
                decltype(boundedChecker.CheckThrough(0, 0)) bounded;
                {
                    prof::ScopedSpan prefix_span(prof::Stage::CEGAR_PREFIX_BMC,
                                                 cegar_round);
                    bounded = boundedChecker.CheckThrough(delay - 1, delay);
                }
                LOG_L(m_log, 1, "word-level guard prefix: ms=", elapsed(prefixStart));
                if (bounded.status == MemoryBMC::Status::Counterexample) {
                    m_trace = boundedChecker.GetTrace();
                    res = CheckResult::Unsafe;
                } else if (bounded.status == MemoryBMC::Status::PrefixSafe &&
                           bounded.checkedThrough && *bounded.checkedThrough >= delay - 1) {
                    res = CheckResult::Safe;
                } else {
                    LOG_L(m_log, 0, "WL memory BMC incomplete: ", bounded.reason);
                    res = CheckResult::Unknown;
                }
            } catch (const std::exception &error) {
                LOG_L(m_log, 0, "WL memory BMC failed: ", error.what());
                res = CheckResult::Unknown;
            }
        }
        break;
    }

    return res;
}

} // namespace car
