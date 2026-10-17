#include "ObservationSidecar.h"
#include "Sha256.h"
#include "WLSimulator.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <unistd.h>

namespace car {
namespace {

std::string Json(const std::string &value) {
    std::ostringstream output;
    output << '"';
    for (unsigned char c : value) {
        if (c == '"' || c == '\\') output << '\\' << c;
        else if (c < 32) {
            static const char hex[] = "0123456789abcdef";
            output << "\\u00" << hex[c >> 4] << hex[c & 15];
        } else output << c;
    }
    output << '"';
    return output.str();
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

struct Descriptor {
    int64_t node, valueNode;
    uint64_t event;
    unsigned width;
    std::string name, kind;
};
struct Event {
    std::optional<Descriptor> exec, value, bad;
};

class Observer {
  public:
    explicit Observer(const Btor2IR &ir) {
        std::optional<int64_t> namedPc;
        std::set<int64_t> statePcs;
        for (const auto &node : ir.Nodes()) {
            if (node.tag == BTOR2_TAG_state && (node.symbol == "pc" || node.symbol == "__pc"))
                statePcs.insert(node.id);
            if (node.tag != BTOR2_TAG_output) continue;
            const std::string &name = node.symbol.empty() ? ir.Node(node.args[0]).symbol : node.symbol;
            if (name == "__c2btor_pc") {
                if (namedPc) throw std::runtime_error("duplicate __c2btor_pc output");
                namedPc = node.args[0];
            }
            for (const std::string kind : {"exec", "value", "bad"}) {
                const std::string prefix = "__c2btor_" + kind + "_";
                if (name.compare(0, prefix.size(), prefix)) continue;
                const uint64_t event = Decimal(name.substr(prefix.size()));
                const int64_t valueId = node.args[0];
                const auto &sort = ir.Sort(ir.Node(valueId < 0 ? -valueId : valueId).sortId);
                // Array-valued summary outputs stay sparse in the original cex.
                if (sort.tag == BTOR2_TAG_SORT_array && kind == "value") continue;
                if (sort.tag != BTOR2_TAG_SORT_bitvec || (kind != "value" && sort.width != 1))
                    throw std::runtime_error("observation output has an invalid sort: " + name);
                Descriptor descriptor{node.id, valueId, event, sort.width, name, kind};
                auto &group = events[event];
                auto &slot = kind == "exec" ? group.exec : kind == "value" ? group.value : group.bad;
                if (slot) throw std::runtime_error("duplicate observation kind/event");
                slot = descriptor;
                outputs.push_back(std::move(descriptor));
            }
        }
        if (namedPc) pc = namedPc;
        else if (statePcs.size() == 1) pc = *statePcs.begin();
        else if (statePcs.size() > 1) throw std::runtime_error("ambiguous pc/__pc state declarations");
        if (pc) {
            const int64_t id = *pc < 0 ? -*pc : *pc;
            const auto &sort = ir.Sort(ir.Node(id).sortId);
            if (sort.tag != BTOR2_TAG_SORT_bitvec) throw std::runtime_error("PC observation must be a bitvector");
            pcWidth = sort.width;
        }
    }

    void Header(std::ostream &out, const std::string &modelHash, const std::string &cexHash, size_t frames) const {
        out << "{\"record\":\"header\",\"format\":\"c2btor-observations-1\",\"model_sha256\":" << Json(modelHash)
            << ",\"cex_sha256\":" << Json(cexHash) << ",\"frames\":" << frames << ",\"pc\":";
        if (pc) out << "{\"node\":" << *pc << ",\"width\":" << pcWidth << '}'; else out << "null";
        out << ",\"outputs\":[";
        bool first = true;
        for (const auto &descriptor : outputs) {
            if (!first) out << ',';
            first = false;
            out << "{\"node\":" << descriptor.node << ",\"value_node\":" << descriptor.valueNode
                << ",\"name\":" << Json(descriptor.name) << ",\"kind\":" << Json(descriptor.kind)
                << ",\"event\":" << descriptor.event << ",\"width\":" << descriptor.width << '}';
        }
        out << "]}\n";
    }

    void Frame(std::ostream &out, size_t time, const WLSimulator::Frame &frame) const {
        out << "{\"record\":\"frame\",\"frame\":" << time << ",\"pc\":";
        if (pc) out << Json(frame.Scalar(*pc).ToBinary()); else out << "null";
        out << ",\"events\":[";
        bool first = true;
        for (const auto &[event, group] : events) {
            if (!group.exec || !frame.Scalar(group.exec->valueNode).IsOne()) continue;
            if (!first) out << ',';
            first = false;
            out << "{\"event\":" << event << ",\"exec\":\"1\"";
            if (group.value) out << ",\"value\":" << Json(frame.Scalar(group.value->valueNode).ToBinary());
            if (group.bad) out << ",\"bad\":" << Json(frame.Scalar(group.bad->valueNode).ToBinary());
            out << '}';
        }
        out << "]}\n";
        if (!out) throw std::runtime_error("observation sidecar write failed");
    }

  private:
    std::optional<int64_t> pc;
    unsigned pcWidth{0};
    std::vector<Descriptor> outputs;
    std::map<uint64_t, Event> events;
};

void Serialize(const Btor2IR &ir, const WLTrace &trace, std::ostream &output) {
    const Ports ports(ir);
    const auto writeValues = [&](const std::vector<int64_t> &ids, const auto &values, const auto &arrays) {
        for (size_t position = 0; position < ids.size(); ++position) {
            if (ir.Sort(ir.Node(ids[position]).sortId).tag == BTOR2_TAG_SORT_array) {
                auto found = arrays.find(ids[position]);
                if (found == arrays.end()) continue;
                const auto &array = found->second;
                if (array.defaultValue) output << position << " [*] " << array.defaultValue->ToBinary() << '\n';
                std::vector<const ArrayEntry *> entries;
                for (const auto &entry : array.entries) entries.push_back(&entry);
                std::sort(entries.begin(), entries.end(), [](const auto *a, const auto *b) {
                    return a->index.ToBinary() < b->index.ToBinary();
                });
                for (const auto *entry : entries)
                    output << position << " [" << entry->index.ToBinary() << "] " << entry->value.ToBinary() << '\n';
            } else {
                auto value = values.find(ids[position]);
                if (value != values.end()) output << position << ' ' << value->second.ToBinary() << '\n';
            }
        }
    };
    output << "sat\nb0\n";
    for (size_t time = 0; time < trace.steps.size(); ++time) {
        const auto &step = trace.steps[time];
        output << '#' << time << '\n';
        writeValues(ports.states, step.stateValues, step.arrayStateValues);
        output << '@' << time << '\n';
        writeValues(ports.inputs, step.inputValues, step.arrayInputValues);
    }
    output << ".\n";
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
                if (line != (header == 0 ? "sat" : "b0")) throw std::runtime_error("expected sat/b0 header");
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

bool WriteObservedCounterexample(const Btor2IR &ir, const WLTrace &trace,
                                const std::string &modelPath, const std::string &cexPath,
                                std::string &error, const std::string &originalCex,
                                const std::string &originalCexSha256) {
    bool publishedCex = false;
    const std::string observationPath = cexPath + ".obs.jsonl";
    try {
        if (std::filesystem::exists(cexPath) || std::filesystem::exists(observationPath))
            throw std::runtime_error("counterexample/observation output already exists");
        Temporary cex(cexPath), observation(observationPath);
        {
            std::ofstream output(cex.path, std::ios::binary);
            if (!output) throw std::runtime_error("cannot open counterexample temporary");
            if (originalCex.empty()) Serialize(ir, trace, output);
            else {
                std::ifstream input(originalCex, std::ios::binary);
                if (!input) throw std::runtime_error("cannot copy original counterexample");
                output << input.rdbuf();
                if (!input.eof() && input.fail()) throw std::runtime_error("original counterexample copy failed");
            }
            output.close();
            if (!output) throw std::runtime_error("counterexample temporary write failed");
        }
        const std::string modelHash = Sha256::File(modelPath), cexHash = Sha256::File(cex.path);
        if (ir.SourceSha256().empty() || ir.SourceSha256() != modelHash)
            throw std::runtime_error("model bytes do not match parsed SourceIR");
        if (!originalCex.empty() && (originalCexSha256.empty() || originalCexSha256 != cexHash ||
                                     Sha256::File(originalCex) != cexHash))
            throw std::runtime_error("original counterexample bytes do not match parsed trace");
        int64_t badDeclaration = 0;
        for (const auto &node : ir.Nodes())
            if (node.tag == BTOR2_TAG_bad) badDeclaration = node.id;
        Observer observer(ir);
        std::ofstream output(observation.path, std::ios::binary);
        if (!output) throw std::runtime_error("cannot open observation temporary");
        observer.Header(output, modelHash, cexHash, trace.steps.size());
        const auto verified = WLSimulator(ir).Verify(trace, [&](size_t time, const WLSimulator::Frame &frame) {
            observer.Frame(output, time, frame);
        });
        if (verified.kind != WLSimulator::VerificationKind::Confirmed)
            throw std::runtime_error("SourceIR Verify rejected frame " + std::to_string(verified.time) +
                                     " node " + std::to_string(verified.nodeId) + ": " + verified.reason);
        if (Sha256::File(modelPath) != modelHash)
            throw std::runtime_error("model changed during SourceIR Verify");
        output.flush();
        if (!output) throw std::runtime_error("observation temporary flush failed");
        const std::string observationsHash = Sha256::File(observation.path);
        output << "{\"record\":\"footer\",\"confirmed\":true,\"observations_sha256\":"
               << Json(observationsHash) << ",\"frames\":" << trace.steps.size()
               << ",\"badframe\":" << verified.time << ",\"bad_node\":" << badDeclaration
               << ",\"bad_expression\":" << verified.nodeId << "}\n";
        output.close();
        if (!output) throw std::runtime_error("observation temporary close failed");
        // Atomic no-replace publication. The complete sidecar is the final
        // receipt; no observer can see a successful footer before verification.
        std::filesystem::create_hard_link(cex.path, cexPath);
        publishedCex = true;
        std::filesystem::create_hard_link(observation.path, observationPath);
        return true;
    } catch (const std::exception &failure) {
        if (publishedCex) std::remove(cexPath.c_str());
        error = failure.what();
        return false;
    }
}
} // namespace car
