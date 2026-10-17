#include "Btor2Frontend.h"
#include "ObservationSidecar.h"
#include "Sha256.h"
#include "WLSimulator.h"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr const char *usage =
    "usage: carat-replay --model MODEL --output-dir NEW_DIR "
    "(--cex CEX | --probe-steps FRAMES [--probe-input INPUT_NODE=BINARY ...] [--probe-input-at FRAME:INPUT_NODE=BINARY ...])";

uint64_t PositiveInteger(const std::string &text, const std::string &name) {
    if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos)
        throw std::runtime_error(name + " must be a positive decimal integer");
    size_t consumed = 0;
    const uint64_t value = std::stoull(text, &consumed);
    if (!value || consumed != text.size())
        throw std::runtime_error(name + " must be a positive decimal integer");
    return value;
}

using TimedSeeds = std::map<std::pair<size_t, int64_t>, car::BitVector>;

std::pair<int64_t, car::BitVector> InputSeed(const car::Btor2IR &ir, const std::string &seed) {
    const auto separator = seed.find('=');
    if (separator == std::string::npos) throw std::runtime_error("probe input must be INPUT_NODE=BINARY");
    const auto rawId = PositiveInteger(seed.substr(0, separator), "probe input node");
    if (rawId > uint64_t(std::numeric_limits<int64_t>::max()))
        throw std::runtime_error("probe input node is out of range");
    const auto id = int64_t(rawId);
    const auto &node = ir.Node(id);
    if (node.tag != BTOR2_TAG_input)
        throw std::runtime_error("probe seed requires an original bit-vector input declaration");
    const auto &sort = ir.Sort(node.sortId);
    if (sort.tag != BTOR2_TAG_SORT_bitvec)
        throw std::runtime_error("probe seed requires an original bit-vector input declaration");
    const std::string bits = seed.substr(separator + 1);
    if (bits.size() != sort.width || bits.find_first_not_of("01") != std::string::npos)
        throw std::runtime_error("probe seed must have exactly the input width in binary bits");
    return {id, car::BitVector::FromBinary(sort.width, bits)};
}

// These are deliberate concrete choices, not completion of a solver result.
// Every original input and genuinely free state choice is explicitly recorded.
car::WLTrace ProbeChoices(const car::Btor2IR &ir, size_t frames,
                         const std::map<int64_t, car::BitVector> &seeds,
                         const TimedSeeds &timedSeeds) {
    std::set<int64_t> initialized, transitioned;
    for (const auto &node : ir.Nodes()) {
        if (node.tag == BTOR2_TAG_init) initialized.insert(node.args[0]);
        if (node.tag == BTOR2_TAG_next) transitioned.insert(node.args[0]);
    }
    car::WLTrace trace;
    trace.steps.resize(frames);
    const auto assign = [&](car::WLTraceStep &step, const car::Btor2IRNode &node, bool state) {
        const auto &sort = ir.Sort(node.sortId);
        if (sort.tag == BTOR2_TAG_SORT_array) {
            car::ArrayValue value;
            value.defaultValue = car::BitVector::Zero(ir.Sort(sort.elementSort).width);
            (state ? step.arrayStateValues : step.arrayInputValues).emplace(node.id, std::move(value));
        } else {
            const auto seed = seeds.find(node.id);
            auto value = seed == seeds.end() ? car::BitVector::Zero(sort.width) : seed->second;
            (state ? step.stateValues : step.inputValues).emplace(node.id, std::move(value));
        }
    };
    for (const auto &node : ir.Nodes()) {
        if (node.tag == BTOR2_TAG_input) {
            for (auto &step : trace.steps) assign(step, node, false);
        } else if (node.tag == BTOR2_TAG_state) {
            if (!initialized.count(node.id)) assign(trace.steps.front(), node, true);
            if (!transitioned.count(node.id))
                for (size_t time = 1; time < frames; ++time) assign(trace.steps[time], node, true);
        }
    }
    for (const auto &[key, value] : timedSeeds)
        trace.steps.at(key.first).inputValues.insert_or_assign(key.second, value);
    return trace;
}

int Probe(const car::Btor2IR &ir, const std::filesystem::path &model,
          const std::filesystem::path &output, size_t frames,
          const std::map<int64_t, car::BitVector> &seeds,
          const TimedSeeds &timedSeeds) {
    try {
        auto trace = ProbeChoices(ir, frames, seeds, timedSeeds);
        // Reject missing choices here as well as in final Verify. No expression
        // overrides, abstract properties, guessed successor states, or SAT calls.
        const auto execution = car::WLSimulator(ir).Simulate(
            trace, {}, car::WLSimulator::MissingChoices::Reject);
        size_t firstBad = frames;
        for (size_t time = 0; time < execution.bad.size(); ++time) {
            if (!execution.constraintsHold.at(time)) break;
            if (execution.bad.at(time)) { firstBad = time; break; }
        }
        if (firstBad == frames) {
            std::cout << "Unknown\nconcrete-probe: no bad with a valid constraint prefix in "
                      << frames << " frames\n";
            return 2;
        }
        trace.steps.resize(firstBad + 1);
        if (!std::filesystem::create_directory(output))
            throw std::runtime_error("cannot create new output directory");
        const auto target = output / (model.filename().string() + ".cexb");
        std::string error;
        // This independently checks every original transition/constraint and
        // final bad before publishing a hash-bound cex and observation footer.
        if (!car::WriteObservedCounterexample(ir, trace, model.string(), target.string(), error))
            throw std::runtime_error(error);
        std::cout << "concrete-probe: first valid bad at frame " << firstBad << '\n';
        std::cout << "Confirmed SourceIR Verify and observations: " << target.string() << ".obs.jsonl\n";
        std::cout << "Unsafe\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "carat-replay concrete-probe: " << error.what() << '\n';
        std::cout << "Unknown\n";
        return 2;
    }
}

} // namespace

int main(int argc, char **argv) {
    try {
        std::map<std::string, std::string> options;
        std::vector<std::string> rawSeeds, rawTimedSeeds;
        for (int i = 1; i < argc; ++i) {
            const std::string key = argv[i];
            if (i + 1 == argc) throw std::runtime_error(usage);
            if (key == "--probe-input") rawSeeds.emplace_back(argv[++i]);
            else if (key == "--probe-input-at") rawTimedSeeds.emplace_back(argv[++i]);
            else if ((key != "--model" && key != "--cex" && key != "--output-dir" && key != "--probe-steps") ||
                     !options.emplace(key, argv[++i]).second)
                throw std::runtime_error(usage);
        }
        const bool probing = options.count("--probe-steps");
        if (!options.count("--model") || !options.count("--output-dir") ||
            (probing == bool(options.count("--cex"))) || (!probing && (!rawSeeds.empty() || !rawTimedSeeds.empty())))
            throw std::runtime_error(usage);
        const auto model = std::filesystem::canonical(options.at("--model"));
        const auto output = std::filesystem::absolute(options.at("--output-dir"));
        if (std::filesystem::exists(output)) throw std::runtime_error("output directory must not exist");
        const auto ir = car::Btor2Frontend::LoadIR(model.string());
        if (probing) {
            const auto frames = PositiveInteger(options.at("--probe-steps"), "probe frame count");
            if (frames > std::numeric_limits<size_t>::max())
                throw std::runtime_error("probe frame count exceeds this platform");
            std::map<int64_t, car::BitVector> seeds;
            for (const std::string &seed : rawSeeds) {
                auto [id, value] = InputSeed(ir, seed);
                if (!seeds.emplace(id, std::move(value)).second)
                    throw std::runtime_error("duplicate probe input node");
            }
            TimedSeeds timedSeeds;
            for (const std::string &seed : rawTimedSeeds) {
                const auto separator = seed.find(':');
                if (separator == std::string::npos)
                    throw std::runtime_error("timed probe input must be FRAME:INPUT_NODE=BINARY");
                const std::string timeText = seed.substr(0, separator);
                if (timeText.empty() || timeText.find_first_not_of("0123456789") != std::string::npos)
                    throw std::runtime_error("probe input frame must be a nonnegative decimal integer");
                const uint64_t time = std::stoull(timeText);
                if (time >= frames) throw std::runtime_error("probe input frame is outside candidate horizon");
                auto [id, value] = InputSeed(ir, seed.substr(separator + 1));
                if (!timedSeeds.emplace(std::make_pair(size_t(time), id), std::move(value)).second)
                    throw std::runtime_error("duplicate probe input at the same frame");
            }
            return Probe(ir, model, output, size_t(frames), seeds, timedSeeds);
        }
        const auto cex = std::filesystem::canonical(options.at("--cex"));
        const auto cexHash = car::Sha256::File(cex.string());
        const auto trace = car::ReadBtor2Counterexample(ir, cex.string());
        if (car::Sha256::File(cex.string()) != cexHash)
            throw std::runtime_error("original counterexample changed during parsing");
        if (!std::filesystem::create_directory(output)) throw std::runtime_error("cannot create new output directory");
        const auto target = output / (model.filename().string() + ".cexb");
        std::string error;
        if (!car::WriteObservedCounterexample(ir, trace, model.string(), target.string(), error, cex.string(), cexHash))
            throw std::runtime_error(error);
        std::cout << "Confirmed SourceIR Verify and observations: " << target.string() << ".obs.jsonl\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "carat-replay: " << error.what() << '\n';
        return 1;
    }
}
