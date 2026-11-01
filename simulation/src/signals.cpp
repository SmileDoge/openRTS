#include <openrts/simulation/signals.hpp>

#include <algorithm>
#include <cmath>
#include <set>

#include <openrts/simulation/track_traveller.hpp>
#include <openrts/simulation/train.hpp>

namespace openrts::simulation {
namespace {

using formats::SignalBuiltin;
using formats::SignalInstruction;
using formats::SignalOp;
using formats::SignalOperandKind;

constexpr double walk_limit = 30000.0;   // m; RTS walks are unbounded
constexpr std::size_t chain_limit = 16;  // objects per train chain (0x508520)
constexpr std::uint32_t no_index = 0xFFFFFFFFu;

struct Interval {
    double low{};
    double high{};
};

// One object of a train's chain (RTS 56-byte chain node).
struct ChainNode {
    std::uint32_t object{};
    std::vector<std::uint32_t> heads;
    double distance{};
    int min_function = 5;
    float aspect{}; // obeyed aspect, filled from the far end
    float speed = -1.0f;
};

struct TrainState {
    std::vector<ChainNode> chain;
    SignalAhead ahead;
    std::uint32_t nearest_object = no_index;
    double nearest_distance{};
    float nearest_aspect{};
};

[[nodiscard]] bool same(const std::vector<Interval>& a, const std::vector<Interval>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::abs(a[i].low - b[i].low) > 1e-3 || std::abs(a[i].high - b[i].high) > 1e-3) return false;
    }
    return true;
}

// The walk mask of block_state / route_set (0x48106C): which functions end it.
[[nodiscard]] std::uint32_t walk_mask(const int argument, const int function) {
    std::uint32_t mask = 2;
    const auto lower = [&](std::uint32_t value) {
        if (function > 1) value |= 4u;
        if (function > 2) value |= 8u;
        if (function > 3) value |= 0x10u;
        if (function > 4) value |= 0x20u;
        return value;
    };
    switch (argument) {
    case 1: mask = (1u << function) | 2u; break;
    case 2: mask = 1u << function; break;
    case 3: mask = 0xFFFFu; break;
    case 4: mask = lower(2u); break;
    case 5: mask = lower((1u << function) | 2u); break;
    default: break;
    }
    return mask;
}

} // namespace

formats::SignalAspect SignalHead::aspect() const noexcept {
    const auto value = static_cast<int>(state);
    return static_cast<formats::SignalAspect>(std::clamp(value, 0, 9));
}

struct SignalSystem::Impl {
    TrackNetwork* network = nullptr;
    std::vector<std::vector<std::uint32_t>> node_heads; // by node: heads sorted by offset
    std::map<const formats::SignalScript*, std::vector<float>> locals;
    std::map<std::uint32_t, std::vector<Interval>> occupancy, previous_occupancy;
    std::vector<std::pair<std::uint32_t, bool>> junctions; // by node: selected, occupied
    std::vector<std::vector<std::uint32_t>> dep_nodes, dep_junctions;
    std::vector<char> dirty, in_chain, was_in_chain, always, stale;
    std::function<std::optional<double>(const Train&)> authority;
    std::map<std::uint64_t, TrainState> trains;
    std::vector<SignalPass> passes;
    // The head being run, for dependency recording.
    std::uint32_t running = no_index;
};

SignalSystem::SignalSystem(TrackNetwork& network, std::shared_ptr<const formats::SignalConfig> config,
                           std::shared_ptr<const formats::SignalScriptFile> scripts,
                           std::vector<formats::WorldObjectPlacement> signals)
    : impl_(std::make_unique<Impl>()), config_(std::move(config)), scripts_(std::move(scripts)) {
    auto& impl = *impl_;
    impl.network = &network;
    impl.node_heads.resize(network.node_count());
    // TDB item -> (node, offset).
    std::map<std::uint32_t, std::pair<std::uint32_t, double>> item_place;
    for (std::uint32_t n = 1; n < network.node_count(); ++n) {
        if (const auto* node = network.node(n)) {
            for (const auto& entry : node->items) item_place.emplace(entry.item, std::pair{n, entry.offset});
        }
    }
    const auto& items = network.database().items;
    for (auto& placement : signals) {
        if (placement.kind != formats::WorldObjectKind::signal) continue;
        const auto object_index = static_cast<std::uint32_t>(objects_.size());
        SignalObject object{.placement = placement};
        const auto* shape = config_->find_shape(placement.shape_file.filename().string());
        std::uint32_t features = 0;
        if (shape) {
            for (const auto& sub : shape->sub_objects) {
                if (sub.index >= 32 || !(placement.signal_sub_objects & (1u << sub.index))) continue;
                if (sub.kind == formats::SignalSubObjectKind::user && sub.user + 5 < 32) features |= 1u << (sub.user + 5);
                if (sub.kind == formats::SignalSubObjectKind::number_plate) features |= 0x10u;
                if (sub.kind == formats::SignalSubObjectKind::gradient_plate) features |= 0x20u;
            }
        }
        for (const auto& unit : placement.signal_units) {
            SignalHead head{.object = object_index, .sub_object = unit.sub_object, .track_item = unit.track_item, .features = features};
            if (unit.track_item < items.size() && items[unit.track_item].kind == formats::TrackItemKind::signal) {
                const auto& item = items[unit.track_item];
                head.type = config_->find_type(item.name);
                if (head.type) head.script = scripts_->find(head.type->name);
                head.forward = item.signal_direction == 0;
                for (const auto& link : item.signal_directions) head.links.emplace_back(link.junction, link.values[1]);
                if (const auto it = item_place.find(unit.track_item); it != item_place.end()) {
                    head.placed = true;
                    head.node = it->second.first;
                    head.offset = it->second.second;
                }
            }
            if (!head.type) messages_.push_back("signal head on item " + std::to_string(unit.track_item) + ": unknown type");
            object.heads.push_back(static_cast<std::uint32_t>(heads_.size()));
            heads_.push_back(head);
        }
        objects_.push_back(std::move(object));
    }
    for (std::uint32_t h = 0; h < heads_.size(); ++h) {
        const auto& head = heads_[h];
        if (head.placed && head.node < impl.node_heads.size()) impl.node_heads[head.node].push_back(h);
        if (!head.script) ++stats_.without_script;
        if (!head.placed) ++stats_.unplaced;
    }
    for (auto& list : impl.node_heads) {
        std::sort(list.begin(), list.end(), [&](std::uint32_t a, std::uint32_t b) { return heads_[a].offset < heads_[b].offset; });
    }
    const auto n = heads_.size();
    impl.dep_nodes.resize(n);
    impl.dep_junctions.resize(n);
    impl.dirty.assign(n, char{1});
    impl.in_chain.assign(n, char{0});
    impl.was_in_chain.assign(n, char{0});
    impl.always.assign(n, char{0});
    impl.stale.assign(n, char{0});
    for (std::size_t h = 0; h < n; ++h) {
        const auto* script = heads_[h].script;
        if (!script) continue;
        // Scripts reading neighbouring heads run every step; a run that read
        // a local before writing it (RTS keeps locals) is repeated too.
        impl.always[h] = std::any_of(script->builtins_used.begin(), script->builtins_used.end(), [](SignalBuiltin b) {
            return b == SignalBuiltin::this_sig_lr || b == SignalBuiltin::this_sig_mr;
        });
        impl.locals.try_emplace(script, std::vector<float>(script->locals.size(), 0.0f));
    }
    impl.junctions.assign(network.node_count(), {no_index, false});
    stats_.heads = n;
}

SignalSystem::~SignalSystem() = default;

namespace {

// The walks of block_state and route_set over the network.
struct Walker {
    const TrackNetwork& network;
    const std::vector<SignalHead>& heads;
    const std::vector<std::vector<std::uint32_t>>& node_heads;
    const std::map<std::uint32_t, std::vector<Interval>>& occupancy;
    std::vector<std::uint32_t>* nodes = nullptr;
    std::vector<std::uint32_t>* junctions = nullptr;

    // The nearest head on `node` strictly within (low, high) along the walk
    // direction whose function is in `mask`; returns its offset.
    [[nodiscard]] std::optional<double> stop_on(std::uint32_t node, bool forward, double low, double high, std::uint32_t mask) const {
        std::optional<double> best;
        if (node >= node_heads.size()) return best;
        for (const auto h : node_heads[node]) {
            const auto& other = heads[h];
            if (other.forward != forward || !other.type) continue;
            if (!(mask & (1u << static_cast<int>(other.type->function)))) continue;
            if (other.offset <= low || other.offset >= high) continue;
            if (forward) {
                high = other.offset;
            } else {
                low = other.offset;
            }
            best = other.offset;
        }
        return best;
    }

    [[nodiscard]] bool occupied(std::uint32_t node, std::optional<std::pair<double, double>> range) const {
        const auto it = occupancy.find(node);
        if (it == occupancy.end()) return false;
        if (!range) return !it->second.empty();
        // Strictly inside: a train whose end only touches a bound (a head
        // standing exactly at the signal) is not in the block yet; otherwise
        // the signal drops in the very step the head reaches it and the pass
        // reads as one at danger.
        constexpr double touch = 0.01;
        for (const auto& interval : it->second) {
            if (interval.high > range->first + touch && interval.low < range->second - touch) return true;
        }
        return false;
    }

    void record_node(std::uint32_t node) const {
        if (nodes && std::find(nodes->begin(), nodes->end(), node) == nodes->end()) nodes->push_back(node);
    }

    // 0x48106C.
    [[nodiscard]] int block_state(const SignalHead& head, int argument, bool shunting) const {
        if (!head.placed || !head.type) return 0;
        const auto mask = walk_mask(argument, static_cast<int>(head.type->function));
        auto node = head.node;
        bool forward = head.forward;
        bool first = true;
        bool found_train = false;
        double walked = 0.0;
        std::set<std::pair<std::uint32_t, bool>> visited;
        while (true) {
            const auto* info = network.node(node);
            if (!info || !visited.insert({node, forward}).second) return found_train ? 1 : 0;
            record_node(node);
            double low = -1.0;
            double high = info->length + 1.0;
            if (first) {
                if (forward) {
                    low = head.offset;
                } else {
                    high = head.offset;
                }
            }
            const auto stop = stop_on(node, forward, low, high, mask);
            if (stop) {
                if (forward) {
                    high = *stop;
                } else {
                    low = *stop;
                }
            }
            if (!found_train && !shunting) {
                const bool bounded = first || stop.has_value();
                found_train = occupied(node, bounded ? std::optional{std::pair{low, high}} : std::nullopt);
            }
            if (stop) return found_train ? 1 : 0;
            walked += first ? (forward ? info->length - head.offset : head.offset) : info->length;
            if (walked > walk_limit) return found_train ? 1 : 0;
            MoveStop result = MoveStop::completed;
            const auto step = next_vector_node(network, node, forward, result, junctions);
            if (!step) return result == MoveStop::junction_obstructed ? 2 : (found_train ? 1 : 0);
            node = step->node;
            forward = step->forward;
            first = false;
        }
    }

    // 0x480CCC.
    [[nodiscard]] int route_set(const SignalHead& head, int argument) const {
        if (!head.placed || !head.type) return 0;
        const auto mask = walk_mask(argument, static_cast<int>(head.type->function));
        auto node = head.node;
        bool forward = head.forward;
        bool first = true;
        bool found = false;
        int count = 0;
        double walked = 0.0;
        std::set<std::pair<std::uint32_t, bool>> visited;
        while (true) {
            const auto* info = network.node(node);
            if (!info || !visited.insert({node, forward}).second) return count;
            record_node(node);
            double low = -1.0;
            double high = info->length + 1.0;
            if (first) {
                if (forward) {
                    low = head.offset;
                } else {
                    high = head.offset;
                }
            }
            if (stop_on(node, forward, low, high, mask)) found = true;
            if (found) {
                if (head.links.empty()) return 1;
                if (count) return count;
            }
            walked += first ? (forward ? info->length - head.offset : head.offset) : info->length;
            if (walked > walk_limit) return count;
            MoveStop result = MoveStop::completed;
            const auto step = next_vector_node(network, node, forward, result, junctions);
            if (!step) {
                if (result == MoveStop::junction_obstructed) return 0;
                return head.links.empty() ? count + 1 : count;
            }
            for (const auto& pass : step->junctions) {
                const auto* state = network.junction(pass.junction);
                if (!state) continue;
                for (const auto& [junction, branch] : head.links) {
                    if (junction == pass.junction && branch == state->selected) {
                        ++count;
                        if (found) return count;
                    }
                }
            }
            node = step->node;
            forward = step->forward;
            first = false;
        }
    }
};

} // namespace

int SignalSystem::block_state(const std::uint32_t head, const int mask) const {
    if (head >= heads_.size()) return 0;
    Walker walker{*impl_->network, heads_, impl_->node_heads, impl_->occupancy};
    return walker.block_state(heads_[head], mask, false);
}

int SignalSystem::route_set(const std::uint32_t head, const int mask) const {
    if (head >= heads_.size()) return 0;
    Walker walker{*impl_->network, heads_, impl_->node_heads, impl_->occupancy};
    return walker.route_set(heads_[head], mask);
}

std::vector<std::pair<std::string, float>> SignalSystem::locals(const std::uint32_t head) const {
    std::vector<std::pair<std::string, float>> result;
    if (head >= heads_.size() || !heads_[head].script) return result;
    const auto* script = heads_[head].script;
    const auto it = impl_->locals.find(script);
    for (std::size_t i = 0; i < script->locals.size(); ++i) {
        result.emplace_back(script->locals[i], it != impl_->locals.end() && i < it->second.size() ? it->second[i] : 0.0f);
    }
    return result;
}

namespace {

// The draw state and speed of an aspect in the type's table (def_draw_state,
// 0x47A138); -1 draw state when the aspect is not listed.
[[nodiscard]] std::pair<int, float> aspect_entry(const formats::SignalType& type, int aspect) {
    for (const auto& entry : type.aspects) {
        if (static_cast<int>(entry.aspect) != aspect) continue;
        // The draw state's own number (scripts also assign these numbers).
        const auto draw = entry.draw_state_index >= 0 && static_cast<std::size_t>(entry.draw_state_index) < type.draw_states.size()
            ? static_cast<int>(type.draw_states[static_cast<std::size_t>(entry.draw_state_index)].index)
            : -1;
        return {draw, entry.speed};
    }
    return {-1, -1.0f};
}

} // namespace

// Runs head `index` (0x499948). `chain` and `self`: its train chain and its
// node there, or none for a free run.
// Returns whether the run read a local before writing it: its result then
// depends on earlier runs, so the head runs again next step.
static bool run_head(SignalSystem::Impl& impl, std::vector<SignalHead>& heads, const std::uint32_t index,
                     const std::vector<ChainNode>* chain, const std::size_t self, const formats::SignalConfig& config,
                     const Walker& walker_template) {
    auto& head = heads[index];
    if (!head.script || !head.script->ok() || !head.type) return false;
    head.speed = -1.0f;
    const auto mode = static_cast<int>(head.mode);
    if (mode == 2 || mode == 3) {
        head.state = 9.0f;
        const auto [draw, speed] = aspect_entry(*head.type, 9);
        head.draw_state = static_cast<float>((std::max)(draw, 0));
        head.speed = speed;
        return false;
    }
    auto& locals = impl.locals[head.script];
    std::vector<char> written(locals.size(), 0);
    bool stale = false;
    const auto read = [&](const formats::SignalOperand& o) {
        if (o.kind == SignalOperandKind::local && o.index < written.size() && !written[o.index]) stale = true;
    };
    const auto write = [&](const formats::SignalOperand& o) {
        if (o.kind == SignalOperandKind::local && o.index < written.size()) written[o.index] = 1;
    };
    impl.dep_nodes[index].clear();
    impl.dep_junctions[index].clear();
    Walker walker = walker_template;
    walker.nodes = &impl.dep_nodes[index];
    walker.junctions = &impl.dep_junctions[index];

    float registers[16]{};
    float arguments[8]{};
    std::size_t argument_count = 0;
    float immediate_a{};
    float immediate_b{};
    const auto operand = [&](const formats::SignalOperand& o, float& immediate) -> float& {
        switch (o.kind) {
        case SignalOperandKind::local:
            if (o.index < locals.size()) return locals[o.index];
            break;
        case SignalOperandKind::head:
            switch (o.index) {
            case 7: return head.state;
            case 8: return head.draw_state;
            case 9: return head.enabled;
            case 13: return head.mode;
            default: break;
            }
            break;
        case SignalOperandKind::immediate:
            immediate = o.value;
            return immediate;
        case SignalOperandKind::reg:
            if (o.index < 16) return registers[o.index];
            break;
        default:
            break;
        }
        immediate = 0.0f;
        return immediate;
    };
    const auto chain_heads = [&](std::size_t k) -> const std::vector<std::uint32_t>& { return (*chain)[k].heads; };
    const auto function_of = [&](std::uint32_t h) { return heads[h].type ? static_cast<int>(heads[h].type->function) : 0; };

    const auto call = [&](SignalBuiltin builtin) -> float {
        const float first = arguments[0];
        const auto fn = static_cast<int>(static_cast<std::int64_t>(first));
        switch (builtin) {
        case SignalBuiltin::block_state: {
            const int argument = argument_count ? static_cast<int>(static_cast<std::int64_t>(first)) : 0;
            const bool shunting = !config.shunting_auto_mode && static_cast<int>(head.mode) == 1;
            return static_cast<float>(walker.block_state(head, argument, shunting));
        }
        case SignalBuiltin::route_set: {
            const int argument = argument_count ? static_cast<int>(static_cast<std::int64_t>(first)) : 0;
            return static_cast<float>(walker.route_set(head, argument));
        }
        case SignalBuiltin::next_sig_lr: {
            if (!chain) return 0.0f;
            float result = 0.0f;
            for (auto k = self + 1; k < chain->size(); ++k) {
                bool found = false;
                if (fn == 1) {
                    if ((*chain)[k].min_function == 1) {
                        result = (*chain)[k].aspect;
                        found = true;
                    }
                } else {
                    for (const auto h : chain_heads(k)) {
                        if (function_of(h) != fn) continue;
                        found = true;
                        result = (std::max)(result, heads[h].state);
                    }
                }
                if (found) break;
            }
            return result;
        }
        case SignalBuiltin::next_sig_mr: {
            if (!chain) return 0.0f;
            float result = 0.0f;
            for (auto k = self + 1; k < chain->size(); ++k) {
                bool found = false;
                for (const auto h : chain_heads(k)) {
                    if (function_of(h) != fn) continue;
                    if (!found) result = heads[h].state;
                    found = true;
                    result = (std::min)(result, heads[h].state);
                }
                if (found) break;
            }
            return result;
        }
        case SignalBuiltin::def_draw_state: {
            const auto [draw, speed] = aspect_entry(*head.type, fn);
            if (draw < 0) return 0.0f;
            head.speed = speed;
            return static_cast<float>(draw);
        }
        case SignalBuiltin::sig_feature: {
            if (first >= 32.0f) {
                if (fn == 32) return (head.features & 0x10u) ? 1.0f : 0.0f;
                if (fn == 33) return (head.features & 0x20u) ? 1.0f : 0.0f;
                return 1.0f;
            }
            const auto bit = static_cast<int>(static_cast<std::int64_t>(first + 5.0f));
            return bit >= 0 && bit < 32 && (head.features & (1u << bit)) ? 1.0f : 0.0f;
        }
        case SignalBuiltin::dist_multi_sig_mr: {
            if (!chain) return 0.0f;
            float result = 7.0f;
            bool any = false;
            for (auto k = self + 1; k < chain->size() && !any; ++k) {
                for (const auto h : chain_heads(k)) {
                    bool listed = false;
                    for (std::size_t a = 0; a < argument_count; ++a) {
                        if (function_of(h) == static_cast<int>(static_cast<std::int64_t>(arguments[a]))) listed = true;
                    }
                    if (!listed) continue;
                    any = true;
                    result = (std::min)(result, heads[h].state);
                }
            }
            return any ? result : 0.0f;
        }
        case SignalBuiltin::this_sig_mr: {
            if (!chain) return -1.0f;
            float result = 0.0f;
            bool found = false;
            for (const auto h : chain_heads(self)) {
                if (h == index) break;
                if (function_of(h) != fn) continue;
                if (!found) result = heads[h].state;
                found = true;
                result = (std::min)(result, heads[h].state);
            }
            return result;
        }
        case SignalBuiltin::this_sig_lr: {
            if (!chain) return -1.0f;
            float result = -1.0f;
            for (const auto h : chain_heads(self)) {
                if (h != index && function_of(h) == fn && heads[h].state > result) result = heads[h].state;
            }
            return result;
        }
        default:
            return 0.0f;
        }
    };

    const auto& code = head.script->code;
    for (std::size_t pc = 0, guard = 0; pc < code.size() && guard < 100000; ++guard) {
        const SignalInstruction& in = code[pc];
        switch (in.op) {
        case SignalOp::mov:
            read(in.b);
            write(in.a);
            break;
        case SignalOp::call:
            write(in.a);
            break;
        case SignalOp::jmp:
        case SignalOp::jz:
        case SignalOp::ret:
            break;
        default:
            read(in.a);
            read(in.b);
            write(in.a);
            break;
        }
        auto& a = operand(in.a, immediate_a);
        const auto truncated = [](float v) { return static_cast<std::int64_t>(v); };
        switch (in.op) {
        case SignalOp::mov: a = operand(in.b, immediate_b); break;
        case SignalOp::call: a = call(in.builtin); argument_count = 0; break;
        case SignalOp::push:
            if (argument_count < 8) arguments[argument_count++] = a;
            break;
        case SignalOp::jmp: pc = in.target; continue;
        case SignalOp::jz:
            if (registers[0] == 0.0f) {
                pc = in.target;
                continue;
            }
            break;
        case SignalOp::cmpe: a = truncated(a) == truncated(operand(in.b, immediate_b)) ? 1.0f : 0.0f; break;
        case SignalOp::cmpne: a = truncated(a) == truncated(operand(in.b, immediate_b)) ? 0.0f : 1.0f; break;
        case SignalOp::cmpg: a = truncated(a) <= truncated(operand(in.b, immediate_b)) ? 0.0f : 1.0f; break;
        case SignalOp::cmpge: a = truncated(a) < truncated(operand(in.b, immediate_b)) ? 0.0f : 1.0f; break;
        case SignalOp::cmpl: a = truncated(a) >= truncated(operand(in.b, immediate_b)) ? 0.0f : 1.0f; break;
        case SignalOp::cmple: a = truncated(a) > truncated(operand(in.b, immediate_b)) ? 0.0f : 1.0f; break;
        case SignalOp::cmpef: a = a == operand(in.b, immediate_b) ? 1.0f : 0.0f; break;
        case SignalOp::cmpnef: a = a == operand(in.b, immediate_b) ? 0.0f : 1.0f; break;
        case SignalOp::cmpgf: a = a <= operand(in.b, immediate_b) ? 0.0f : 1.0f; break;
        case SignalOp::cmpgef: a = a < operand(in.b, immediate_b) ? 0.0f : 1.0f; break;
        case SignalOp::cmplf: a = a >= operand(in.b, immediate_b) ? 0.0f : 1.0f; break;
        case SignalOp::cmplef: a = a > operand(in.b, immediate_b) ? 0.0f : 1.0f; break;
        case SignalOp::op_and: a = (a == 0.0f || operand(in.b, immediate_b) == 0.0f) ? 0.0f : 1.0f; break;
        case SignalOp::op_or: a = (a == 0.0f && operand(in.b, immediate_b) == 0.0f) ? 0.0f : 1.0f; break;
        case SignalOp::op_xor: a = truncated(operand(in.b, immediate_b)) != truncated(a) ? 1.0f : 0.0f; break;
        case SignalOp::op_not: a = a == 0.0f ? 1.0f : 0.0f; break;
        case SignalOp::neg: a = -a; break;
        case SignalOp::round:
            if (a != 0.0f) {
                const auto whole = truncated(a);
                a = static_cast<float>(a - static_cast<float>(whole) < 0.5f ? whole : whole + 1);
            }
            break;
        case SignalOp::ret: pc = code.size(); continue;
        }
        ++pc;
    }
    // SHUNT mode without RTS_ShuntingAutoMode: proceed aspects become RESTRICTING.
    if (!config.shunting_auto_mode && static_cast<int>(head.mode) == 1) {
        const auto aspect = static_cast<std::int64_t>(head.state);
        if (aspect >= 3 && aspect <= 7) {
            head.state = 2.0f;
            const auto [draw, speed] = aspect_entry(*head.type, 2);
            head.speed = speed;
            if (draw >= 0) head.draw_state = static_cast<float>(draw);
        }
    }
    return stale;
}

void SignalSystem::update(const std::span<const Train* const> trains, const double dt) {
    auto& impl = *impl_;
    const auto& network = *impl.network;
    stats_.chain_runs = 0;
    stats_.free_runs = 0;

    // 1. Occupancy: every train's extent, walked from the rear to the front.
    impl.previous_occupancy = std::move(impl.occupancy);
    impl.occupancy.clear();
    for (const auto* train : trains) {
        if (!train || !train->placed()) continue;
        const auto rear = train->rear_end();
        if (!rear) continue;
        auto node = rear->node();
        auto offset = rear->offset();
        bool forward = rear->forward();
        double remaining = train->length();
        for (std::size_t guard = 0; guard < 1000 && remaining > 1e-6; ++guard) {
            const auto* info = network.node(node);
            if (!info) break;
            const double available = forward ? info->length - offset : offset;
            const double take = (std::min)(available, remaining);
            impl.occupancy[node].push_back(forward ? Interval{offset, offset + take} : Interval{offset - take, offset});
            remaining -= take;
            if (remaining <= 1e-6) break;
            MoveStop stop = MoveStop::completed;
            const auto step = next_vector_node(network, node, forward, stop);
            if (!step) break;
            node = step->node;
            forward = step->forward;
            const auto* next = network.node(node);
            offset = forward ? 0.0 : (next ? next->length : 0.0);
        }
    }
    for (auto& [node, intervals] : impl.occupancy) {
        std::sort(intervals.begin(), intervals.end(), [](const Interval& a, const Interval& b) { return a.low < b.low; });
    }
    std::set<std::uint32_t> changed_nodes;
    for (const auto& [node, intervals] : impl.occupancy) {
        const auto it = impl.previous_occupancy.find(node);
        if (it == impl.previous_occupancy.end() || !same(it->second, intervals)) changed_nodes.insert(node);
    }
    for (const auto& [node, intervals] : impl.previous_occupancy) {
        if (!impl.occupancy.contains(node)) changed_nodes.insert(node);
    }
    std::set<std::uint32_t> changed_junctions;
    for (std::uint32_t n = 1; n < network.node_count(); ++n) {
        const auto* state = network.junction(n);
        if (!state) continue;
        const std::pair now{state->selected, state->occupied};
        if (impl.junctions[n] != now) {
            changed_junctions.insert(n);
            impl.junctions[n] = now;
        }
    }
    for (std::size_t h = 0; h < heads_.size(); ++h) {
        if (impl.dirty[h]) continue;
        for (const auto node : impl.dep_nodes[h]) {
            if (changed_nodes.contains(node)) {
                impl.dirty[h] = 1;
                break;
            }
        }
        if (impl.dirty[h]) continue;
        for (const auto junction : impl.dep_junctions[h]) {
            if (changed_junctions.contains(junction)) {
                impl.dirty[h] = 1;
                break;
            }
        }
    }

    const Walker walker{network, heads_, impl.node_heads, impl.occupancy};
    std::swap(impl.in_chain, impl.was_in_chain);
    std::fill(impl.in_chain.begin(), impl.in_chain.end(), char{0});

    // 2. Train chains (0x508520).
    std::set<std::uint64_t> alive;
    for (const auto* train : trains) {
        if (!train || !train->placed()) continue;
        alive.insert(train->id());
        auto& state = impl.trains[train->id()];
        state.chain.clear();
        const auto lead = train->leading_end();
        if (!lead) continue;
        // Collect objects ahead.
        auto node = lead->node();
        double offset = lead->offset();
        bool forward = lead->forward();
        double walked = 0.0;
        std::uint32_t normal_count = 0;
        std::uint32_t needed = 1; // train +164 (0 for player trains, PROBABLE) + 1
        std::set<std::pair<std::uint32_t, bool>> visited;
        bool done = false;
        while (!done && state.chain.size() < chain_limit) {
            const auto* info = network.node(node);
            if (!info || !visited.insert({node, forward}).second) break;
            std::vector<std::uint32_t> here;
            if (node < impl.node_heads.size()) {
                for (const auto h : impl.node_heads[node]) {
                    const auto& head = heads_[h];
                    if (head.forward != forward) continue;
                    if (forward ? head.offset < offset : head.offset > offset) continue;
                    here.push_back(h);
                }
            }
            if (!forward) std::reverse(here.begin(), here.end());
            for (const auto h : here) {
                const auto& head = heads_[h];
                const double distance = walked + std::abs(head.offset - offset);
                if (!state.chain.empty() && state.chain.back().object == head.object) {
                    state.chain.back().heads.push_back(h);
                    continue;
                }
                if (state.chain.size() >= chain_limit || normal_count >= needed) {
                    done = true;
                    break;
                }
                state.chain.push_back({.object = head.object, .heads = {h}, .distance = distance});
            }
            // Recount NORMAL objects and the look-ahead length (heads of one
            // object can sit at the same item position in any order).
            normal_count = 0;
            for (std::size_t k = 0; k < state.chain.size(); ++k) {
                auto& chain_node = state.chain[k];
                chain_node.min_function = 5;
                bool normal = false;
                for (const auto h : chain_node.heads) {
                    const auto fn = heads_[h].type ? static_cast<int>(heads_[h].type->function) : 5;
                    if (fn == 1) {
                        chain_node.min_function = 1;
                        normal = true;
                    } else if (fn < chain_node.min_function) {
                        chain_node.min_function = fn;
                    }
                }
                if (normal) ++normal_count;
                if ((normal_count == 1 || k == 0) && normal) {
                    for (const auto h : chain_node.heads) {
                        if (heads_[h].type) needed = (std::max)(needed, heads_[h].type->num_clear_ahead + 2);
                    }
                }
            }
            if (done) break;
            walked += forward ? info->length - offset : offset;
            if (walked > walk_limit) break;
            MoveStop stop = MoveStop::completed;
            const auto step = next_vector_node(network, node, forward, stop);
            if (!step) break;
            node = step->node;
            forward = step->forward;
            const auto* next = network.node(node);
            offset = forward ? 0.0 : (next ? next->length : 0.0);
        }
        // Objects whose first head is EXCL are left out (0x508520).
        std::erase_if(state.chain, [&](const ChainNode& c) { return static_cast<int>(heads_[c.heads.front()].mode) == 3; });
        const bool count_reached = normal_count >= needed;

        // Evaluate from the farthest to the nearest.
        const double passing = std::abs(train->speed()) * dt * 1.5;
        const auto authority = impl.authority ? impl.authority(*train) : std::nullopt;
        bool seen_normal = false;
        for (std::size_t k = state.chain.size(); k-- > 0;) {
            auto& chain_node = state.chain[k];
            chain_node.aspect = 0.0f;
            chain_node.speed = -1.0f;
            float enabled = 2.0f;
            if (chain_node.min_function == 1) {
                if (!seen_normal && count_reached) enabled = 0.0f;
                seen_normal = true;
            }
            if (authority && chain_node.distance > *authority + 1.0) enabled = 0.0f;
            for (const auto h : chain_node.heads) {
                auto& head = heads_[h];
                head.enabled = enabled;
                impl.stale[h] = run_head(impl, heads_, h, &state.chain, k, *config_, walker);
                ++stats_.chain_runs;
                head.enabled = chain_node.distance > passing ? 2.0f : 0.0f;
                impl.in_chain[h] = 1;
                impl.dirty[h] = 0;
                if (head.type && head.type->function == formats::SignalFunction::normal && chain_node.aspect < head.state) {
                    chain_node.aspect = static_cast<float>(static_cast<std::int64_t>(head.state));
                    if (head.speed > chain_node.speed || head.speed < 0.0f) chain_node.speed = head.speed;
                }
            }
        }
        // What the train sees, and passes.
        state.ahead = {};
        for (const auto& chain_node : state.chain) {
            if (chain_node.min_function != 1) continue;
            state.ahead = {.valid = true,
                           .object = chain_node.object,
                           .distance = chain_node.distance,
                           .aspect = static_cast<formats::SignalAspect>(std::clamp(static_cast<int>(chain_node.aspect), 0, 9)),
                           .speed = chain_node.speed};
            break;
        }
        const auto nearest = state.ahead.valid ? state.ahead.object : no_index;
        if (state.nearest_object != no_index && nearest != state.nearest_object
            && state.nearest_distance < std::abs(train->speed()) * dt * 4.0 + 1.0) {
            impl.passes.push_back({.train = train->id(),
                                   .object = state.nearest_object,
                                   .aspect = static_cast<formats::SignalAspect>(std::clamp(static_cast<int>(state.nearest_aspect), 0, 9))});
        }
        // The aspect a pass is judged by is the one shown while the train was
        // still beyond the passing distance (RTS 0x508520: an object being
        // passed now is no longer run for the train); at the signal itself
        // the train's own front may already count in its block.
        const bool new_object = nearest != state.nearest_object;
        state.nearest_object = nearest;
        state.nearest_distance = state.ahead.valid ? state.ahead.distance : 0.0;
        if (new_object || state.nearest_distance > passing + 0.5) {
            state.nearest_aspect = state.ahead.valid ? static_cast<float>(state.ahead.aspect) : 0.0f;
        }
    }
    std::erase_if(impl.trains, [&](const auto& entry) { return !alive.contains(entry.first); });

    // 3. Free-running heads (0x479AFC): no train chain this step.
    stats_.always_run_heads = 0;
    for (std::uint32_t h = 0; h < heads_.size(); ++h) {
        if (impl.in_chain[h]) continue;
        auto& head = heads_[h];
        if (impl.was_in_chain[h]) impl.dirty[h] = 1;
        const int mode = static_cast<int>(head.mode);
        if (mode != 2 && mode != 3) head.mode = 0.0f;
        head.enabled = 0.0f;
        if (!full_evaluation_ && !impl.dirty[h] && !impl.always[h] && !impl.stale[h]) continue;
        impl.stale[h] = run_head(impl, heads_, h, nullptr, 0, *config_, walker);
        impl.dirty[h] = 0;
        ++stats_.free_runs;
    }
    for (std::size_t h = 0; h < heads_.size(); ++h) {
        if (impl.always[h] || impl.stale[h]) ++stats_.always_run_heads;
    }
    // Flash timers.
    for (auto& head : heads_) {
        if (!head.type || head.type->flash_on <= 0.0f) continue;
        head.flash_timer += static_cast<float>(dt);
        if (head.flash_timer > head.type->flash_on + head.type->flash_off) head.flash_timer = 0.0f;
    }
}

void SignalSystem::set_authority(std::function<std::optional<double>(const Train&)> authority) {
    impl_->authority = std::move(authority);
}

bool SignalSystem::reads_stale(const std::uint32_t head) const {
    return head < impl_->stale.size() && (impl_->stale[head] || impl_->always[head]);
}

SignalAhead SignalSystem::ahead(const Train& train) const {
    const auto it = impl_->trains.find(train.id());
    return it == impl_->trains.end() ? SignalAhead{} : it->second.ahead;
}

std::vector<SignalPass> SignalSystem::take_passes() {
    return std::exchange(impl_->passes, {});
}

} // namespace openrts::simulation
