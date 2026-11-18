#include "types.hpp"

#include <algorithm>
#include <cstddef>
#include <utility>

namespace epq {
namespace {

// A token denotes either integers, one function literal, or values created at
// one list allocation site. The element slot for a site is shared by all its
// aliases, even when an alias has travelled through a list or an indirect call.
using Token = int;
using SlotId = int;
constexpr Token integer = 0;

enum class RuleKind { Load, Store, Negate, Call };

struct Rule {
    RuleKind kind;
    SlotId value; // Store's source, otherwise the result receiving values.
    std::vector<SlotId> args;
};

struct Slot {
    std::vector<Token> values;
    std::vector<SlotId> outputs;
    std::vector<Rule> rules;
};

template<class T>
bool insert(std::vector<T>& members, T value) {
    if (std::find(members.begin(), members.end(), value) != members.end()) return false;
    members.push_back(value);
    return true;
}

bool may_fall_through(const Node& statement) {
    const auto& c = statement.children;
    switch (statement.op) {
        case Op::RET:
            return false;
        case Op::BLK:
        case Op::SEQ:
            return std::all_of(c.begin(), c.end(), may_fall_through);
        case Op::IF:
            return may_fall_through(c[1]) || may_fall_through(c[2]);
        case Op::WHL:
            return c[0].op != Op::INT || c[0].number == 0;
        case Op::FOR:
            return may_fall_through(c[0]) && (c[1].op != Op::INT || c[1].number == 0);
        default:
            return true;
    }
}

class Flow {
    const Resolution& resolution;
    const Token first_list;
    std::vector<Slot> slots;
    std::vector<SlotId> list_elements;
    std::vector<std::pair<SlotId, Token>> pending;

    SlotId new_slot() {
        slots.emplace_back();
        return static_cast<SlotId>(slots.size()) - 1;
    }

    void seed(SlotId slot, Token token) {
        if (insert(slots[slot].values, token)) pending.emplace_back(slot, token);
    }

    void connect(SlotId from, SlotId to) {
        if (from == to || !insert(slots[from].outputs, to)) return;
        for (Token value : slots[from].values) seed(to, value);
    }

    void apply(const Rule& rule, Token value) {
        if (rule.kind == RuleKind::Call) {
            if (value <= integer || value >= first_list) return;
            const int function = value - 1;
            const auto& params = resolution.functions[function]->children[0].children;
            if (params.size() != rule.args.size()) return; // This call would raise an arity error.
            for (std::size_t i = 0; i < params.size(); ++i)
                connect(rule.args[i], resolution.id.at(&params[i]));
            connect(return_slots[function], rule.value);
            return;
        }
        if (value >= first_list) {
            SlotId elements = list_elements[value - first_list];
            if (rule.kind == RuleKind::Store) connect(rule.value, elements);
            else connect(elements, rule.value); // Indexing or popping the list.
        } else if (value == integer && rule.kind == RuleKind::Negate) {
            seed(rule.value, integer);
        }
    }

    SlotId visit(const Node& node, int function) {
        const auto& c = node.children;
        if (node.op == Op::ID) {
            SlotId slot = resolution.id.at(&node);
            nodes.emplace(&node, slot);
            return slot;
        }
        SlotId dst = new_slot();
        nodes.emplace(&node, dst);
        switch (node.op) {
            case Op::INT:
            case Op::INP:
                seed(dst, integer);
                break;
            case Op::ADD: case Op::SUB: case Op::MUL: case Op::DIV:
            case Op::ABS: case Op::NOT: case Op::EQ: case Op::NE:
            case Op::LT: case Op::GT: case Op::LE: case Op::GE:
                for (const auto& child : c) visit(child, function);
                seed(dst, integer);
                break;
            case Op::AND:
            case Op::OR:
                for (const auto& child : c) connect(visit(child, function), dst);
                break;
            case Op::DEF: {
                int nested = resolution.function.at(&node);
                seed(dst, 1 + nested);
                for (const Node& param : c[0].children) visit(param, nested);
                visit(c[1], nested);
                if (may_fall_through(c[1])) seed(return_slots[nested], integer);
                break;
            }
            case Op::LST: {
                Token site = first_list + static_cast<int>(list_elements.size());
                SlotId elements = new_slot();
                list_elements.push_back(elements);
                seed(dst, site);
                for (const auto& child : c) connect(visit(child, function), elements);
                break;
            }
            case Op::CAL: {
                std::vector<SlotId> args;
                for (const auto& arg : c[1].children) args.push_back(visit(arg, function));
                SlotId target = visit(c[0], function);
                slots[target].rules.push_back({RuleKind::Call, dst, std::move(args)});
                break;
            }
            case Op::NEG: {
                SlotId operand = visit(c[0], function);
                slots[operand].rules.push_back({RuleKind::Negate, dst, {}});
                break;
            }
            case Op::IND: {
                SlotId base = visit(c[0], function);
                visit(c[1], function);
                slots[base].rules.push_back({RuleKind::Load, dst, {}});
                break;
            }
            case Op::SET: {
                if (c[0].op == Op::GET || c[0].op == Op::SGET) break;
                SlotId lhs = visit(c[0], function);
                SlotId rhs = visit(c[1], function);
                if (c[0].op == Op::ID) connect(rhs, lhs);
                else {
                    SlotId base = nodes.at(&c[0].children[0]);
                    slots[base].rules.push_back({RuleKind::Store, rhs, {}});
                }
                connect(rhs, dst);
                break;
            }
            case Op::APP: {
                SlotId base = visit(c[0], function);
                SlotId rhs = visit(c[1], function);
                slots[base].rules.push_back({RuleKind::Store, rhs, {}});
                connect(base, dst);
                break;
            }
            case Op::RNG: {
                SlotId iterator = visit(c[0], function);
                SlotId base = visit(c[1], function);
                // Only an existing binding can observe zero from an empty range.
                if (!resolution.fresh_iterators.count(&node)) seed(iterator, integer);
                slots[base].rules.push_back({RuleKind::Load, iterator, {}});
                visit(c[2], function);
                break;
            }
            case Op::RET: {
                SlotId value = visit(c[0], function);
                if (function >= 0) connect(value, return_slots[function]);
                break;
            }
            case Op::CLS: case Op::GET: case Op::SGET:
                // These grammar productions have no defined runtime semantics.
                break;
            default:
                for (const auto& child : c) visit(child, function);
                break;
        }
        return dst;
    }

    Kind kind(Token value) const {
        if (value == integer) return Kind::Int;
        return value < first_list ? Kind::Function : Kind::List;
    }

public:
    std::unordered_map<const Node*, SlotId> nodes;
    std::vector<SlotId> return_slots;

    Flow(const Node& root, const Resolution& resolution)
        : resolution(resolution), first_list(1 + static_cast<int>(resolution.functions.size())),
          slots(resolution.bindings.size()) {
        for (std::size_t i = 0; i < resolution.functions.size(); ++i) return_slots.push_back(new_slot());
        visit(root, -1);
        for (std::size_t i = 0; i < pending.size(); ++i) {
            const auto [from, value] = pending[i];
            for (SlotId to : slots[from].outputs) seed(to, value);
            for (const Rule& rule : slots[from].rules) apply(rule, value);
        }
    }

    Type type(SlotId slot) const {
        const auto& values = slots[slot].values;
        if (values.empty()) return {};
        Kind first = kind(values.front());
        for (Token token : values) if (kind(token) != first) return {};
        int target = first == Kind::Function && values.size() == 1 ? values.front() - 1 : -1;
        return {first, target};
    }

    Type element(SlotId slot) const {
        const auto& values = slots[slot].values;
        if (values.empty() || values.front() < first_list) return {};
        Type common = type(list_elements[values.front() - first_list]);
        for (Token site : values) {
            if (site < first_list) return {};
            Type item = type(list_elements[site - first_list]);
            if (item.kind != common.kind) return {};
            if (item.function != common.function) common.function = -1;
        }
        return common;
    }
};

} // namespace

Types infer(const Node& root, const Resolution& resolution) {
    Flow flow(root, resolution);
    Types types;
    for (const auto& [node, slot] : flow.nodes) {
        Type type = flow.type(slot);
        types.expressions.emplace(node, type);
        if (type.kind == Kind::List) types.elements.emplace(node, flow.element(slot));
    }
    for (std::size_t i = 0; i < resolution.bindings.size(); ++i)
        types.bindings.push_back(flow.type(static_cast<SlotId>(i)));
    for (SlotId slot : flow.return_slots) types.results.push_back(flow.type(slot));
    return types;
}

Type Types::expression(const Node& node) const {
    auto it = expressions.find(&node);
    return it == expressions.end() ? Type{} : it->second;
}

Type Types::element(const Node& node) const {
    auto it = elements.find(&node);
    return it == elements.end() ? Type{} : it->second;
}

} // namespace epq
