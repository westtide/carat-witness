#include "Btor2Witness.h"
#include "WLSimulator.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <unistd.h>

namespace car {
namespace {

// The frontend/checkers support one bad. Derive the declaration ordinal from
// that contract and reject multiple properties instead of labeling an arbitrary
// violated property b0. The witness reader checks the same original ordering.
size_t BadOrdinal(const Btor2IR &ir) {
    size_t count = 0;
    for (const auto &node : ir.Nodes())
        if (node.tag == BTOR2_TAG_bad) ++count;
    if (count != 1) throw std::runtime_error("BTOR2 witness requires exactly one bad declaration");
    return count - 1;
}

uint64_t Decimal(const std::string &text) {
    if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos)
        throw std::runtime_error("expected an unsigned decimal ordinal");
    size_t used = 0;
    const uint64_t value = std::stoull(text, &used);
    if (used != text.size()) throw std::runtime_error("invalid decimal ordinal");
    return value;
}

BitVector Bits(const std::string &text, unsigned width) {
    if (text.size() != width || text.find_first_not_of("01") != std::string::npos)
        throw std::runtime_error("concrete binary value width/content mismatch");
    return BitVector::FromBinary(width, text);
}

struct Ports {
    std::vector<int64_t> states, inputs;
    explicit Ports(const Btor2IR &ir) {
        for (const auto &node : ir.Nodes()) {
            if (node.tag == BTOR2_TAG_state) states.push_back(node.id);
            if (node.tag == BTOR2_TAG_input) inputs.push_back(node.id);
        }
    }
};

class Temporary {
  public:
    explicit Temporary(const std::string &target) {
        std::string pattern = target + ".tmp.XXXXXX";
        std::vector<char> bytes(pattern.begin(), pattern.end());
        bytes.push_back(0);
        const int fd = mkstemp(bytes.data());
        if (fd < 0) throw std::system_error(errno, std::generic_category(), "create witness temporary");
        close(fd);
        path = bytes.data();
    }
    ~Temporary() { if (!path.empty()) std::remove(path.c_str()); }
    std::string path;
};

void WritePorts(std::ostream &output, const Btor2IR &ir,
                const std::vector<int64_t> &ids, const WLSimulator::Frame &frame) {
    for (size_t ordinal = 0; ordinal < ids.size(); ++ordinal) {
        const int64_t id = ids[ordinal];
        if (ir.Sort(ir.Node(id).sortId).tag == BTOR2_TAG_SORT_array) {
            const auto array = frame.Array(id);
            if (array.defaultValue)
                output << ordinal << " [*] " << array.defaultValue->ToBinary() << '\n';
            std::vector<const ArrayEntry *> entries;
            for (const auto &entry : array.entries) entries.push_back(&entry);
            std::sort(entries.begin(), entries.end(), [](const auto *a, const auto *b) {
                return a->index.ToBinary() < b->index.ToBinary();
            });
            for (const auto *entry : entries)
                output << ordinal << " [" << entry->index.ToBinary() << "] "
                       << entry->value.ToBinary() << '\n';
        } else {
            output << ordinal << ' ' << frame.Scalar(id).ToBinary() << '\n';
        }
    }
    if (!output) throw std::runtime_error("counterexample write failed");
}
} // namespace

WLTrace ReadBtor2Counterexample(const Btor2IR &ir, const std::string &path) {
    std::ifstream input(path);
    if (!input) throw std::runtime_error("cannot open counterexample: " + path);
    const Ports ports(ir);
    WLTrace trace;
    enum class Section { None, States, Inputs } section = Section::None;
    unsigned header = 0;
    bool ended = false;
    size_t lineNumber = 0, time = 0;
    std::string line;
    std::set<std::pair<int64_t, std::string>> arrayAddresses;
    while (std::getline(input, line)) {
        ++lineNumber;
        const size_t first = line.find_first_not_of(" \t\r");
        if (first == std::string::npos) continue;
        line = line.substr(first, line.find_last_not_of(" \t\r") - first + 1);
        try {
            if (ended) throw std::runtime_error("data after witness terminator");
            if (header < 2) {
                if (header == 0) {
                    if (line != "sat") throw std::runtime_error("expected sat header");
                } else {
                    if (line.empty() || line[0] != 'b' || Decimal(line.substr(1)) != BadOrdinal(ir))
                        throw std::runtime_error("claimed bad ordinal does not match the model");
                }
                ++header;
                continue;
            }
            if (line == ".") { ended = true; continue; }
            if (line[0] == '#' || line[0] == '@') {
                arrayAddresses.clear();
                const auto frame = Decimal(line.substr(1));
                if (frame > std::numeric_limits<unsigned>::max()) throw std::runtime_error("frame exceeds simulator range");
                if (line[0] == '#') {
                    if (frame != trace.steps.size() || (!trace.steps.empty() && section != Section::Inputs))
                        throw std::runtime_error("state frames must be contiguous and follow an input section");
                    time = static_cast<size_t>(frame);
                    trace.steps.emplace_back();
                    section = Section::States;
                } else {
                    if (trace.steps.empty() || frame != time || section != Section::States)
                        throw std::runtime_error("input section must match preceding state frame");
                    section = Section::Inputs;
                }
                continue;
            }
            if (section == Section::None) throw std::runtime_error("assignment before frame header");
            std::istringstream row(line);
            std::vector<std::string> tokens;
            for (std::string token; row >> token;) tokens.push_back(token);
            if (tokens.size() < 2) throw std::runtime_error("malformed assignment");
            const auto &ids = section == Section::States ? ports.states : ports.inputs;
            const auto ordinal = Decimal(tokens[0]);
            if (ordinal >= ids.size()) throw std::runtime_error("port ordinal outside model declarations");
            const int64_t id = ids[ordinal];
            const auto &sort = ir.Sort(ir.Node(id).sortId);
            auto &step = trace.steps[time];
            if (sort.tag == BTOR2_TAG_SORT_array) {
                if (tokens.size() != 3 || tokens[1].size() < 3 || tokens[1].front() != '[' || tokens[1].back() != ']')
                    throw std::runtime_error("array assignment requires [address] and concrete bits");
                auto &arrays = section == Section::States ? step.arrayStateValues : step.arrayInputValues;
                auto &array = arrays[id];
                const auto value = Bits(tokens[2], ir.Sort(sort.elementSort).width);
                const auto address = tokens[1].substr(1, tokens[1].size() - 2);
                if (address == "*") {
                    if (array.defaultValue) throw std::runtime_error("duplicate array default");
                    array.defaultValue = value;
                } else {
                    const auto index = Bits(address, ir.Sort(sort.indexSort).width);
                    if (!arrayAddresses.emplace(id, address).second)
                        throw std::runtime_error("duplicate array address");
                    array.entries.push_back({index, value});
                }
            } else {
                if (tokens.size() != 2) throw std::runtime_error("scalar assignment requires concrete bits");
                auto &values = section == Section::States ? step.stateValues : step.inputValues;
                if (!values.emplace(id, Bits(tokens[1], sort.width)).second)
                    throw std::runtime_error("duplicate scalar assignment");
            }
        } catch (const std::exception &error) {
            throw std::runtime_error("cex line " + std::to_string(lineNumber) + ": " + error.what());
        }
    }
    if (!input.eof()) throw std::runtime_error("counterexample read failed");
    if (!ended || trace.steps.empty() || section != Section::Inputs)
        throw std::runtime_error("counterexample lacks complete frames/terminator");
    return trace;
}

bool WriteBtor2Counterexample(const Btor2IR &ir, const WLTrace &trace,
                              const std::string &cexPath, std::string &error) {
    try {
        if (std::filesystem::exists(cexPath))
            throw std::runtime_error("counterexample output already exists");
        Temporary temporary(cexPath);
        std::ofstream output(temporary.path, std::ios::binary);
        if (!output) throw std::runtime_error("cannot open counterexample temporary");
        const Ports ports(ir);
        output << "sat\nb" << BadOrdinal(ir) << '\n';
        // GetTrace has already restored original-model choices eliminated by
        // COI. Verify never guesses retained values or runs a second search.
        // Serialize the simulator's full original interface, including counters
        // outside the property COI and the final frame's inputs.
        const auto verified = WLSimulator(ir).Verify(
            trace, [&](size_t time, const WLSimulator::Frame &frame) {
                output << '#' << time << '\n';
                WritePorts(output, ir, ports.states, frame);
                output << '@' << time << '\n';
                WritePorts(output, ir, ports.inputs, frame);
            });
        if (verified.kind != WLSimulator::VerificationKind::Confirmed)
            throw std::runtime_error("SourceIR replay rejected frame " + std::to_string(verified.time) +
                                     " node " + std::to_string(verified.nodeId) + ": " + verified.reason);
        output << ".\n";
        output.close();
        if (!output) throw std::runtime_error("counterexample close failed");
        // Publish one complete standard witness only after original-model replay.
        std::filesystem::create_hard_link(temporary.path, cexPath);
        return true;
    } catch (const std::exception &failure) {
        error = failure.what();
        return false;
    }
}
} // namespace car
