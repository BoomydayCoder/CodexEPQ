#include "quick.hpp"
#include "copyloop.hpp"
#include "display.hpp"
#include "loopopt.hpp"
#include "quick_arm.hpp"
#include "quick_storage.hpp"
#include "resolve.hpp"
#include "types.hpp"
#include "epq_runtime.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <vector>

namespace epq {
namespace {

using rt::V;
using Entry = V (*)(V* globals, const V* args);

// One invocation owns its code, globals, and function identities. Function
// listings are only built if EPQ actually prints a function at runtime.
struct Run {
    const Node& root;
    const Resolution& names;
    std::vector<V> globals;
    Entry main;
    std::vector<Entry> targets;
    std::optional<FunctionDisplays> displays;
};
thread_local Run* active = nullptr;

void enter() { active->main(active->globals.data(), nullptr); }

Entry checked_target(V value, std::size_t arity) {
    auto id = static_cast<std::size_t>(value.asFunction());
    if (id >= active->targets.size()) rt::fail("Invalid function");
    if (active->names.functions[id]->children[0].children.size() != arity) rt::fail("Invalid arity");
    return active->targets[id];
}

void undefined_global() { rt::fail("Undefined global"); }
void undefined_local() { rt::fail("Undefined variable"); }
void invalid_arity() { rt::fail("Invalid arity"); }
void index_error() { rt::fail("Index out of bounds"); }
void empty_error() { rt::fail("List is empty"); }
void division_error() { rt::fail("Division by zero"); }
V bulk_fill(V list, V value, V start, V bound, std::uint64_t inclusive) {
    return V::integer(rt::fillLoop(list.asList(), value, start.asInt(), bound.asInt(), inclusive != 0));
}
V bulk_copy(V dst, V src, V start, V bound, std::uint64_t live) {
    return V::integer(rt::copyLoop(dst.asList(), src.asList(), start.asInt(), bound.asInt(), live != 0));
}
void unsupported_member() { rt::fail("Class and member syntax has no defined execution semantics"); }
V input() { return V::integer(rt::input()); }
V not_value(V v) { return V::integer(!v.asInt()); }
V abs_value(V v) { return V::integer(rt::abs(v)); }
std::uint64_t truth(V v) { return rt::truth(v); }
V get(V list, V index) { return rt::get(list.asList(), index.asInt()); }
V set(V list, V index, V value) { return rt::set(list.asList(), index.asInt(), value); }
V append(V list, V value) { return V::list(rt::append(list.asList(), value)); }

V list_literal(const V* values, std::size_t count) {
    return V::list(rt::makeList(values, count));
}

template<Op op> V binary_value(V a, V b) {
    if constexpr (op == Op::ADD) return V::integer(rt::add(a.asInt(), b.asInt()));
    if constexpr (op == Op::SUB) return V::integer(rt::sub(a.asInt(), b.asInt()));
    if constexpr (op == Op::MUL) return V::integer(rt::mul(a.asInt(), b.asInt()));
    if constexpr (op == Op::DIV) return V::integer(rt::divide(a.asInt(), b.asInt()));
    if constexpr (op == Op::EQ) return V::integer(rt::equal(a, b));
    if constexpr (op == Op::NE) return V::integer(!rt::equal(a, b));
    if constexpr (op == Op::LT) return V::integer(rt::less(a, b));
    if constexpr (op == Op::LE) return V::integer(!rt::less(b, a));
    if constexpr (op == Op::GT) return V::integer(rt::less(b, a));
    if constexpr (op == Op::GE) return V::integer(!rt::less(a, b));
}

using IntOp = arm::IntOp;
using Label = arm::Assembler::Label;

class Compiler {
    const Resolution& names;
    const Types& types;
    const std::vector<ListLayout> layouts;
    arm::Assembler code;
    std::vector<Label> entries;
    std::vector<std::size_t> locations;
    std::vector<unsigned> uses;
    std::vector<bool> inlineable;
    std::vector<bool> inline_in_frame;
    struct Term {
        int binding;
        std::uint32_t offset;
        bool operator==(Term other) const { return binding == other.binding && offset == other.offset; }
    };
    struct Cell {
        Term list, index;
        unsigned address, value;
        bool value_known;
    };
    std::vector<Cell> guarded_cells;
    std::vector<std::pair<int, Term>> aliases;
    struct LoopRange {
        int counter;
        int list;
        int start;
        int max_offset;
        const Node* list_read;
    };
    std::optional<LoopRange> unchecked_range;
    Label end, restart, bad_local, bad_global, bad_index, bad_pop, bad_division;
    int current_function = -1;
    std::size_t main_offset = 0;
    std::vector<std::size_t> function_offsets;

    template<typename Fn>
    void host(Fn function) { code.callHost(reinterpret_cast<const void*>(function)); }

    // Arguments are contiguous in slot order. Once an operand is consumed,
    // its scratch slot can be reused by the rest of the expression or block.
    static bool is_leaf(const Node& n) {
        return n.op == Op::ID || n.op == Op::INT || n.op == Op::DEF;
    }

    // Count each expanded node against a fixed per-callsite budget. Bodies
    // cannot contain calls or loops, so expansion never becomes recursive.
    static bool small_leaf_body(const Node& node, unsigned& remaining) {
        if (!remaining) return false;
        --remaining;
        switch (node.op) {
            case Op::CAL: case Op::FOR: case Op::WHL: case Op::RNG: return false;
            case Op::DEF: return true; // A function literal does not execute its body.
            default: break;
        }
        for (const auto& child : node.children)
            if (!small_leaf_body(child, remaining)) return false;
        return true;
    }

    int inline_target(const Node& call) const {
        const int target = types.expression(call.children[0]).function;
        if (target < 0 || !inlineable[target]) return -1;
        return names.functions[target]->children[0].children.size() == call.children[1].children.size()
            ? target : -1;
    }

    // A nested EPQ call may reuse the same inline frame. Snapshot arguments
    // in the caller's expression temporaries in that case.
    static bool contains_call(const Node& node) {
        if (node.op == Op::CAL) return true;
        if (node.op == Op::DEF) return false;
        return std::any_of(node.children.begin(), node.children.end(), contains_call);
    }

    // An address checked by a condition dominates its taken arm. Identify
    // list and index values symbolically, including affine offsets that wrap
    // with EPQ's uint32 arithmetic. Captured addresses reside in x9/x10 and
    // loaded values in x11/x12 across straight-line native instructions.
    std::optional<Term> term(const Node& n) const {
        if (n.op == Op::INT) return Term{-1, static_cast<std::uint32_t>(n.number)};
        if (n.op == Op::ID) {
            const int id = names.id.at(&n);
            for (const auto& [binding, source] : aliases)
                if (binding == id) return source;
            return Term{id, 0};
        }
        if ((n.op == Op::ADD || n.op == Op::SUB) && integer(n.children[0]) &&
            n.children[1].op == Op::INT) {
            if (auto t = term(n.children[0])) {
                const std::uint32_t k = n.children[1].number;
                t->offset += n.op == Op::ADD ? k : 0u - k;
                return t;
            }
        }
        return {};
    }

    bool retainable_index(const Node& n) const {
        return n.op == Op::IND && n.children[0].op == Op::ID && list(n.children[0]) &&
            layout(n.children[0]) == ListLayout::Ordinary &&
            integer(n.children[1]) && term(n.children[1]).has_value();
    }

    const Cell* guarded(const Node& n) const {
        if (guarded_cells.empty() || !retainable_index(n)) return nullptr;
        auto base = *term(n.children[0]);
        auto index = *term(n.children[1]);
        for (const auto& cell : guarded_cells)
            if (cell.list == base && cell.index == index) return &cell;
        return nullptr;
    }

    void forget_binding(int id) {
        if (guarded_cells.empty() && aliases.empty()) return;
        auto dead_cell = [id](const Cell& c) { return c.list.binding == id || c.index.binding == id; };
        guarded_cells.erase(std::remove_if(guarded_cells.begin(), guarded_cells.end(), dead_cell), guarded_cells.end());
        auto dead_alias = [id](const auto& p) { return p.first == id || p.second.binding == id; };
        aliases.erase(std::remove_if(aliases.begin(), aliases.end(), dead_alias), aliases.end());
    }

    // Calls into the runtime or a nested control-flow merge would invalidate
    // the cached registers. Indexed stores preserve addresses but invalidate
    // loaded values, because two different EPQ lists may be aliases.
    bool register_only_arm(const Node& n, unsigned& budget, bool allow_branches = false) const {
        if (!budget) return false;
        --budget;
        const auto& c = n.children;
        switch (n.op) {
            case Op::ID: case Op::INT: case Op::DEF: return true;
            case Op::SEQ: case Op::BLK: case Op::EXP: case Op::RET: break;
            case Op::IF:
                if (!allow_branches || !integer(c[0])) return false;
                break;
            case Op::SET:
                if (c[0].op != Op::ID && c[0].op != Op::IND) return false;
                if (c[0].op == Op::ID && layout(c[0]) != ListLayout::Ordinary) return false;
                break;
            case Op::IND:
                if (!list(c[0]) || !integer(c[1]) || layout(c[0]) != ListLayout::Ordinary) return false;
                break;
            case Op::ADD: case Op::SUB: case Op::MUL: case Op::DIV:
            case Op::EQ: case Op::NE: case Op::LT: case Op::GT: case Op::LE: case Op::GE:
                if (!integer(c[0]) || !integer(c[1])) return false;
                break;
            case Op::CAL: {
                int f = inline_target(n);
                if (f < 0 || !register_only_arm(c[0], budget) ||
                    !register_only_arm(names.functions[f]->children[1], budget))
                    return false;
                return std::all_of(c[1].children.begin(), c[1].children.end(),
                    [&](const Node& arg) { return register_only_arm(arg, budget); });
            }
            default: return false;
        }
        return std::all_of(c.begin(), c.end(),
            [&](const Node& child) { return register_only_arm(child, budget, allow_branches); });
    }

    bool in_range(const Node& access) const {
        if (!unchecked_range || !retainable_index(access)) return false;
        const auto list_id = *term(access.children[0]);
        const auto index = *term(access.children[1]);
        const auto k = static_cast<std::int32_t>(index.offset);
        return list_id.binding == unchecked_range->list && list_id.offset == 0 &&
            index.binding == unchecked_range->counter && k >= -unchecked_range->start &&
            k <= unchecked_range->max_offset;
    }

    bool indexed_condition(const Node& test, const Node& body, std::size_t s, Label otherwise) {
        auto op = comparison(test.op);
        if (!op) return false;
        for (const Node& operand : test.children)
            if (!retainable_index(operand) || !integer(operand)) return false;
        unsigned budget = 128;
        if (!register_only_arm(body, budget)) return false;
        for (unsigned i = 0; i < 2; ++i) {
            const auto& operands_ = test.children[i].children;
            const unsigned address = 9 + i, value = 11 + i;
            if (in_range(test.children[i])) {
                expr(operands_[1], s);
                code.uncheckedListAddress(address, 14, 0);
            } else {
                operands(operands_[0], operands_[1], s);
                code.knownListAddress(bad_index, address);
            }
            code.loadAt(address, 0, value);
            guarded_cells.push_back({*term(operands_[0]), *term(operands_[1]), address, value, true});
        }
        code.mov(0, 11);
        code.mov(1, 12);
        code.branchIntFalse(*op, otherwise);
        return true;
    }

    bool bound_inputs(const Node& n, std::vector<int>& inputs) const {
        if (!integer(n)) return false;
        if (n.op == Op::ID) {
            inputs.push_back(names.id.at(&n));
            return names.initialized.count(&n);
        }
        if (n.op == Op::INT) return true;
        if (n.op != Op::ADD && n.op != Op::SUB && n.op != Op::MUL) return false;
        return bound_inputs(n.children[0], inputs) && bound_inputs(n.children[1], inputs);
    }

    void loop_effects(const Node& n, LoopRange& range, std::vector<int>& writes) const {
        if (n.op == Op::DEF) return;
        if (n.op == Op::SET && n.children[0].op == Op::ID) writes.push_back(names.id.at(&n.children[0]));
        if (n.op == Op::CAL) loop_effects(names.functions[inline_target(n)]->children[1], range, writes);
        if (retainable_index(n)) {
            const auto index = *term(n.children[1]);
            const auto k = static_cast<std::int32_t>(index.offset);
            if (index.binding == range.counter && k >= -range.start && k <= 1024 &&
                names.initialized.count(&n.children[0])) {
                const int list_id = names.id.at(&n.children[0]);
                if (range.list == -1) {
                    range.list = list_id;
                    range.list_read = &n.children[0];
                }
                if (range.list == list_id) range.max_offset = std::max(range.max_offset, k);
            }
        }
        for (const Node& c : n.children) loop_effects(c, range, writes);
    }

    // Version a unit-stride loop when its immutable bound and one stable list
    // can be checked up front. The ordinary loop handles any failed range
    // proof, with the original ordering of side effects and bounds failures.
    std::optional<LoopRange> counted_range(const Node& n) const {
        const auto& c = n.children;
        if (n.op != Op::FOR || c[0].op != Op::EXP || c[2].op != Op::EXP || c[1].op != Op::LT) return {};
        const auto& init = c[0].children[0];
        const auto& step = c[2].children[0];
        if (init.op != Op::SET || init.children[0].op != Op::ID || init.children[1].op != Op::INT ||
            init.children[1].number < 0 || step.op != Op::SET || step.children[0].op != Op::ID ||
            step.children[1].op != Op::ADD || c[1].children[0].op != Op::ID) return {};
        const auto& sum = step.children[1].children;
        int counter = names.id.at(&init.children[0]);
        if (sum[0].op != Op::ID || sum[1].op != Op::INT || sum[1].number != 1 ||
            names.id.at(&sum[0]) != counter || names.id.at(&step.children[0]) != counter ||
            names.id.at(&c[1].children[0]) != counter) return {};
        std::vector<int> inputs, writes;
        unsigned budget = 160;
        if (!bound_inputs(c[1].children[1], inputs) ||
            std::find(inputs.begin(), inputs.end(), counter) != inputs.end() ||
            !register_only_arm(c[3], budget, true)) return {};
        LoopRange range{counter, -1, init.children[1].number, 0, nullptr};
        loop_effects(c[3], range, writes);
        if (!range.list_read || (names.bindings[range.list].owner != current_function &&
            !names.bindings[range.list].global)) return {};
        inputs.push_back(counter);
        inputs.push_back(range.list);
        for (int binding : inputs)
            if (std::find(writes.begin(), writes.end(), binding) != writes.end()) return {};
        return range;
    }

    void range_version(const Node& n, const LoopRange& range, std::size_t s, Label slow, Label done) {
        const auto& c = n.children;
        const auto& test = c[1].children;
        expr(test[1], s);
        code.mov(13, 0);
        leaf(test[0], 0);
        code.mov(1, 13);
        code.branchIntFalse(IntOp::Lt, done); // Never read a list on a zero-trip loop.
        leaf(*range.list_read, 0);
        code.loadListData(14);
        code.knownListLength();
        if (range.max_offset) {
            code.immediate(1, std::uint64_t(range.max_offset) * 4);
            code.integer(IntOp::Sub);
        }
        code.mov(1, 13);
        code.branchIntFalse(IntOp::Ge, slow);
        unchecked_range = range;
        const auto top = code.label();
        code.bind(top);
        stmt(c[3], s);
        stmt(c[2], s);
        leaf(test[0], 0);
        code.mov(1, 13);
        code.branchIntFalse(IntOp::Ge, top);
        code.jump(done);
        unchecked_range.reset();
    }

    std::size_t array_space(const std::vector<Node>& items) const {
        std::size_t space = items.size();
        for (std::size_t i = 0; i < items.size(); ++i)
            space = std::max(space, i + scratch(items[i]));
        return space;
    }

    std::size_t scratch(const Node& n) const {
        const auto& c = n.children;
        if (is_leaf(n) || c.empty()) return 0;
        switch (n.op) {
            case Op::CAL: {
                std::size_t space = std::max(array_space(c[1].children), c[1].children.size() + scratch(c[0]));
                int f = inline_target(n);
                return f < 0 ? space : std::max(space, scratch(names.functions[f]->children[1]));
            }
            case Op::LST: return array_space(c);
            case Op::RNG: return std::max(scratch(c[1]), 2 + scratch(c[2]));
            case Op::AND: case Op::OR:
                return std::max({std::size_t(1), scratch(c[0]), scratch(c[1])});
            case Op::SET:
                if (c[0].op == Op::ID) return scratch(c[1]);
                if (c[0].op != Op::IND) return 0;
                return std::max(scratch(c[0]), 2 + scratch(c[1]));
            case Op::IND: case Op::APP: case Op::ADD: case Op::SUB: case Op::MUL:
            case Op::DIV: case Op::EQ: case Op::LT: case Op::GT: case Op::LE:
            case Op::GE: case Op::NE:
                return std::max(scratch(c[0]), is_leaf(c[1]) ? 0 : 1 + scratch(c[1]));
            default: {
                std::size_t space = 0;
                for (const auto& child : c) space = std::max(space, scratch(child));
                return space;
            }
        }
    }

    void clear_scope(const Node& scope) {
        auto it = names.declarations.find(&scope);
        if (it == names.declarations.end()) return;
        bool loaded = false;
        for (int binding : it->second) {
            if (!names.needs_init_check.count(binding)) continue;
            forget_binding(binding);
            if (!loaded) {
                code.immediate(0, 3); // rt::V::undefined()
                loaded = true;
            }
            code.store(0, locations[binding]);
        }
    }

    void assign(const Node& id) {
        int b = names.id.at(&id);
        forget_binding(b);
        if (names.bindings[b].global) code.storeAt(0, 19, sizeof(V) * b);
        else code.store(0, locations[b]);
    }

    bool leaf(const Node& n, unsigned reg) {
        switch (n.op) {
            case Op::INT:
                code.immediate(reg, static_cast<std::uint64_t>(std::int64_t(n.number) * 4));
                break;
            case Op::DEF:
                code.immediate(reg, (std::uint64_t(names.function.at(&n)) << 2) | 2);
                break;
            case Op::ID: {
                auto b = names.id.at(&n);
                bool global = names.bindings[b].global;
                if (global) code.loadAt(19, sizeof(V) * b, reg);
                else code.load(locations[b], reg);
                if (!names.initialized.count(&n)) {
                    code.branchEqualImmediate(reg, 3, global ? bad_global : bad_local);
                }
                break;
            }
            default: return false;
        }
        return true;
    }

    bool list(const Node& n) const { return types.expression(n).kind == Kind::List; }
    bool integer(const Node& n) const { return types.expression(n).kind == Kind::Int; }
    ListLayout layout(const Node& n) const {
        return n.op == Op::ID ? layouts[names.id.at(&n)] : ListLayout::Ordinary;
    }

    void operands(const Node& a, const Node& b, std::size_t slot) {
        expr(a, slot);
        if (leaf(b, 1)) return;
        code.saveTemp(0, slot);
        expr(b, slot + 1);
        code.mov(1, 0);
        code.takeTemp(slot, 0);
    }

    void values(const std::vector<Node>& values, std::size_t slot) {
        for (std::size_t i = 0; i < values.size(); ++i) {
            expr(values[i], slot + i);
            code.store(0, slot + i);
        }
    }

    void address(unsigned reg, std::size_t slot, std::size_t count) {
        if (count) code.addressOfSlot(reg, slot);
        else code.immediate(reg, 0);
    }

    void call(const Node& n, std::size_t slot, bool tail = false) {
        const auto& callee = n.children[0];
        const auto& args = n.children[1].children;
        int known = types.expression(callee).function;
        const int expanded = inline_target(n);
        const bool bind_early = expanded >= 0 && !contains_call(callee) &&
            !std::any_of(args.begin(), args.end(), contains_call);
        if (bind_early) {
            const auto& params = names.functions[expanded]->children[0].children;
            for (std::size_t i = 0; i < args.size(); ++i) {
                expr(args[i], slot);
                auto source = guarded_cells.empty() ? std::nullopt : term(args[i]);
                assign(params[i]);
                if (source) aliases.emplace_back(names.id.at(&params[i]), *source);
            }
        } else {
            values(args, slot);
        }
        if (known < 0 || (callee.op != Op::DEF &&
            !(callee.op == Op::ID && names.initialized.count(&callee)))) {
            expr(callee, slot + args.size());
        }
        if (known >= 0 && args.size() != names.functions[known]->children[0].children.size()) {
            host(&invalid_arity);
            return;
        }
        if (expanded >= 0) {
            const auto& def = names.functions[expanded]->children;
            if (!bind_early) {
                for (std::size_t i = 0; i < args.size(); ++i) {
                    code.load(slot + i, 0);
                    assign(def[0].children[i]);
                }
            }
            const Label caller_end = end;
            end = code.label();
            stmt(def[1], slot);
            code.immediate(0, 0);
            code.bind(end);
            end = caller_end;
            return;
        }
        if (tail && known >= 0 && known == current_function) {
            const auto& params = names.functions[known]->children[0].children;
            for (std::size_t i = 0; i < params.size(); ++i) {
                code.load(slot + i, 0);
                assign(params[i]);
            }
            code.jump(restart);
            return;
        }
        if (known < 0) {
            code.immediate(1, args.size());
            host(&checked_target);
            code.mov(2, 0);
        }
        code.context(0);
        address(1, slot, args.size());
        if (known < 0) code.callRegister(2);
        else code.callLabel(entries[known]);
    }

    void logic(const Node& n, std::size_t slot) {
        const auto& left = n.children[0];
        expr(left, slot);
        bool preserve = !integer(left);
        if (preserve) {
            code.store(0, slot);
            host(&truth);
        }
        auto skip = code.label();
        auto done = code.label();
        if (n.op == Op::AND) code.branchZero(0, skip);
        else code.branchNonzero(0, skip);
        expr(n.children[1], slot);
        if (preserve) code.jump(done);
        code.bind(skip);
        if (preserve) code.load(slot, 0);
        code.bind(done);
    }

    std::optional<int> small_offset(const Node& n) const {
        if (n.op != Op::ADD && n.op != Op::SUB) return {};
        const auto& literal = n.children[1];
        if (literal.op != Op::INT || literal.number < -1023 || literal.number > 1023 ||
            !integer(n.children[0])) return {};
        return n.op == Op::ADD ? literal.number : -literal.number;
    }

    // In a statement the assignment's result has no consumer. Update the
    // local in its allocated register, including the EPQ int32 wrap.
    bool increment_local(const Node& n) {
        if (n.op != Op::SET || n.children[0].op != Op::ID) return false;
        const auto& rhs = n.children[1];
        auto offset = small_offset(rhs);
        if (!offset) return false;
        const auto& old = rhs.children[0];
        int id = names.id.at(&n.children[0]);
        if (old.op != Op::ID || names.id.at(&old) != id || names.bindings[id].global ||
            !names.initialized.count(&old)) return false;
        forget_binding(id);
        code.incrementLocal(locations[id], *offset);
        return true;
    }

    template<Op op>
    void arithmetic(const Node& n, std::size_t slot, IntOp instruction) {
        if constexpr (op == Op::ADD || op == Op::SUB) {
            if (auto offset = small_offset(n)) {
                expr(n.children[0], slot);
                code.addSmallInteger(0, *offset);
                return;
            }
        }
        operands(n.children[0], n.children[1], slot);
        if (integer(n.children[0]) && integer(n.children[1])) code.integer(instruction);
        else host(&binary_value<op>);
    }

    void expr(const Node& n, std::size_t s) {
        if (leaf(n, 0)) return;
        const auto& c = n.children;
        switch (n.op) {
            case Op::ADD: return arithmetic<Op::ADD>(n, s, IntOp::Add);
            case Op::SUB: return arithmetic<Op::SUB>(n, s, IntOp::Sub);
            case Op::MUL: return arithmetic<Op::MUL>(n, s, IntOp::Mul);
            case Op::LT: return arithmetic<Op::LT>(n, s, IntOp::Lt);
            case Op::LE: return arithmetic<Op::LE>(n, s, IntOp::Le);
            case Op::GT: return arithmetic<Op::GT>(n, s, IntOp::Gt);
            case Op::GE: return arithmetic<Op::GE>(n, s, IntOp::Ge);
            case Op::EQ: return arithmetic<Op::EQ>(n, s, IntOp::Eq);
            case Op::NE: return arithmetic<Op::NE>(n, s, IntOp::Ne);
            case Op::DIV:
                operands(c[0], c[1], s);
                if (integer(c[0]) && integer(c[1])) return code.knownIntDivide(bad_division);
                return host(&binary_value<Op::DIV>);
            case Op::IND:
                if (auto cell = guarded(n)) {
                    if (cell->value_known) code.mov(0, cell->value);
                    else code.loadAt(cell->address, 0, 0);
                    return;
                }
                if (in_range(n)) {
                    expr(c[1], s);
                    code.uncheckedListAddress(0, 14, 0);
                    code.loadAt(0, 0, 0);
                    return;
                }
                operands(c[0], c[1], s);
                if (auto* ops = private_list_ops(layout(c[0]))) {
                    if (integer(c[1])) return code.knownListGet(bad_index, ops->width);
                    return host(ops->get);
                }
                if (list(c[0]) && integer(c[1])) return code.knownListGet(bad_index);
                return host(&get);
            case Op::APP:
                operands(c[0], c[1], s);
                if (auto* ops = private_list_ops(layout(c[0]))) return host(ops->append);
                return host(&append);
            case Op::SET:
                if (c[0].op == Op::ID) {
                    if (auto* ops = private_list_ops(layout(c[0]))) {
                        const auto& items = c[1].children;
                        clear_scope(c[1]);
                        values(items, s);
                        address(0, s, items.size());
                        code.immediate(1, items.size());
                        host(ops->literal);
                    } else expr(c[1], s);
                    assign(c[0]);
                } else if (c[0].op == Op::IND) {
                    const auto& target = c[0].children;
                    if (auto cell = guarded(c[0])) {
                        unsigned address = cell->address;
                        expr(c[1], s);
                        code.storeAt(0, address, 0);
                        for (auto& saved : guarded_cells) saved.value_known = false;
                        return;
                    }
                    operands(target[0], target[1], s);
                    code.saveTemp(0, s);
                    code.saveTemp(1, s + 1);
                    expr(c[1], s + 2);
                    code.mov(2, 0);
                    code.takeTemp(s, 0);
                    code.takeTemp(s + 1, 1);
                    if (auto* ops = private_list_ops(layout(target[0]))) {
                        if (integer(target[1])) code.knownListSet(bad_index, ops->width);
                        else host(ops->set);
                    } else if (list(target[0]) && integer(target[1])) code.knownListSet(bad_index);
                    else host(&set);
                    for (auto& saved : guarded_cells) saved.value_known = false;
                } else host(&unsupported_member);
                return;
            case Op::NEG:
                expr(c[0], s);
                if (integer(c[0])) code.integer(IntOp::Neg);
                else if (list(c[0])) code.knownListPop(bad_pop);
                else host(&rt::neg);
                return;
            case Op::NOT:
                expr(c[0], s);
                if (integer(c[0])) code.integer(IntOp::Not);
                else host(&not_value);
                return;
            case Op::ABS:
                expr(c[0], s);
                if (integer(c[0])) code.integer(IntOp::Abs);
                else if (list(c[0])) code.knownListLength();
                else host(&abs_value);
                return;
            case Op::AND: case Op::OR: return logic(n, s);
            case Op::INP: return host(&input);
            case Op::LST:
                clear_scope(n);
                values(c, s);
                address(0, s, c.size());
                code.immediate(1, c.size());
                return host(&list_literal);
            case Op::CAL: return call(n, s);
            case Op::CLS: case Op::GET: case Op::SGET: return host(&unsupported_member);
            default: throw std::logic_error("Invalid expression in native compiler");
        }
    }

    static std::optional<IntOp> comparison(Op op) {
        switch (op) {
            case Op::EQ: return IntOp::Eq;
            case Op::NE: return IntOp::Ne;
            case Op::LT: return IntOp::Lt;
            case Op::LE: return IntOp::Le;
            case Op::GT: return IntOp::Gt;
            case Op::GE: return IntOp::Ge;
            default: return std::nullopt;
        }
    }

    void condition(const Node& n, std::size_t s, Label otherwise) {
        if (auto op = comparison(n.op); op && integer(n.children[0]) && integer(n.children[1])) {
            operands(n.children[0], n.children[1], s);
            code.branchIntFalse(*op, otherwise);
            return;
        }
        expr(n, s);
        if (!integer(n)) host(&truth);
        code.branchZero(0, otherwise);
    }

    void range(const Node& n, std::size_t s) {
        const auto& c = n.children;
        expr(c[1], s);
        code.store(0, s);
        code.immediate(0, 0);
        if (!names.fresh_iterators.count(&n)) assign(c[0]);
        code.store(0, s + 1);
        const auto top = code.label(), done = code.label();
        code.bind(top);
        code.load(s, 0);
        if (list(c[1])) code.knownListLength();
        else host(&abs_value);
        code.mov(1, 0);
        code.load(s + 1, 0);
        code.branchIntFalse(IntOp::Lt, done);
        code.load(s, 0);
        code.load(s + 1, 1);
        if (list(c[1])) code.knownListGet(bad_index);
        else host(&get);
        assign(c[0]);
        stmt(c[2], s + 2);
        code.load(s + 1, 0);
        code.immediate(1, 4);
        code.integer(IntOp::Add);
        code.store(0, s + 1);
        code.jump(top);
        code.bind(done);
    }

    void stmt(const Node& n, std::size_t s) {
        const auto& c = n.children;
        clear_scope(n);
        switch (n.op) {
            case Op::BLK: case Op::SEQ:
                for (const auto& child : c) stmt(child, s);
                break;
            case Op::EXP:
                if (!increment_local(c[0])) expr(c[0], s);
                break;
            case Op::RET:
                if (c[0].op == Op::CAL) call(c[0], s, true);
                else expr(c[0], s);
                code.jump(end);
                break;
            case Op::PRINT:
                expr(c[0], s);
                host(&rt::print);
                break;
            case Op::IF: {
                const auto alternative = code.label(), done = code.label();
                if (!indexed_condition(c[0], c[1], s, alternative)) condition(c[0], s, alternative);
                stmt(c[1], s);
                guarded_cells.clear();
                aliases.clear();
                code.jump(done);
                code.bind(alternative);
                stmt(c[2], s);
                code.bind(done);
                break;
            }
            case Op::FOR: case Op::WHL: {
                bool counted = n.op == Op::FOR;
                if (counted) stmt(c[0], s);
                if (auto fill = match_fill_loop(n, names, types)) {
                    const auto done = code.label();
                    condition(c[1], s, done);
                    leaf(*fill->list, 0);
                    leaf(*fill->value, 1);
                    leaf(*fill->start_read, 2);
                    leaf(*fill->bound, 3);
                    code.immediate(4, fill->inclusive);
                    if (auto* ops = private_list_ops(layout(*fill->list))) host(ops->fill);
                    else host(&bulk_fill);
                    assign(*fill->start_read);
                    code.bind(done);
                    break;
                }
                if (auto copy = match_copy_loop(n, names, types);
                    copy && layout(copy->source) == ListLayout::Ordinary &&
                    layout(copy->destination) == ListLayout::Ordinary) {
                    const auto done = code.label();
                    condition(c[counted ? 1 : 0], s, done);
                    leaf(copy->destination, 0);
                    leaf(copy->source, 1);
                    leaf(copy->index, 2);
                    if (copy->uses_source_length) code.immediate(3, 0);
                    else leaf(copy->bound, 3);
                    code.immediate(4, copy->uses_source_length);
                    host(&bulk_copy);
                    assign(copy->index);
                    code.bind(done);
                    break;
                }
                const auto top = code.label(), done = code.label();
                if (auto range = counted_range(n)) {
                    const auto slow = code.label();
                    range_version(n, *range, s, slow, done);
                    code.bind(slow);
                }
                code.bind(top);
                condition(c[counted ? 1 : 0], s, done);
                stmt(c[counted ? 3 : 1], s);
                if (counted) stmt(c[2], s);
                code.jump(top);
                code.bind(done);
                break;
            }
            case Op::RNG: return range(n, s);
            default: throw std::logic_error("Invalid statement in native compiler");
        }
    }

    // Loop-carried locals get first choice of callee-saved registers. Globals
    // always live in the shared table where other EPQ functions can see them.
    void count_uses(const Node& n, unsigned weight) {
        if (n.op == Op::DEF) return;
        if (n.op == Op::ID) {
            auto it = names.id.find(&n);
            if (it != names.id.end()) uses[it->second] += weight;
            return;
        }
        if (n.op == Op::WHL || n.op == Op::FOR || n.op == Op::RNG)
            weight = std::min(weight * 4, 256u);
        for (const auto& child : n.children) count_uses(child, weight);
        if (n.op == Op::CAL) {
            int f = inline_target(n);
            if (f >= 0) {
                inline_in_frame[f] = true;
                count_uses(names.functions[f]->children[1], weight);
            }
        }
    }

    void function(int id, const Node& body) {
        std::fill(uses.begin(), uses.end(), 0);
        std::fill(inline_in_frame.begin(), inline_in_frame.end(), false);
        count_uses(body, 1);
        std::vector<int> locals;
        for (std::size_t i = 0; i < names.bindings.size(); ++i) {
            const auto& b = names.bindings[i];
            if (!b.global && (b.owner == id || (b.owner >= 0 && inline_in_frame[b.owner])))
                locals.push_back(static_cast<int>(i));
        }
        std::stable_sort(locals.begin(), locals.end(), [&](int a, int b) { return uses[a] > uses[b]; });
        unsigned registers = 0;
        for (std::size_t i = 0; i < locals.size(); ++i) {
            locations[locals[i]] = i;
            if (i < 8 && uses[locals[i]] > 1) ++registers;
        }
        current_function = id;
        end = code.label();
        restart = code.label();
        bad_pop = code.label();
        bad_index = code.label();
        bad_division = code.label();
        bad_local = code.label();
        bad_global = code.label();
        code.prologue(locals.size() + scratch(body), registers);
        if (id >= 0) {
            const auto& params = names.functions[id]->children[0].children;
            for (std::size_t i = 0; i < params.size(); ++i) {
                code.argument(0, i);
                assign(params[i]);
            }
        }
        code.bind(restart);
        stmt(body, locals.size());
        code.immediate(0, 0); // Falling through returns the integer zero.
        code.bind(end);
        code.epilogue();
        code.bind(bad_local);
        host(&undefined_local);
        code.bind(bad_global);
        host(&undefined_global);
        code.bind(bad_pop); host(&empty_error);
        code.bind(bad_index); host(&index_error);
        code.bind(bad_division); host(&division_error);
    }

public:
    Compiler(const Node& root, const Resolution& r, const Types& t)
        : names(r), types(t), layouts(list_layouts(root, r, t)), locations(r.bindings.size()), uses(r.bindings.size()),
          inlineable(r.functions.size()), inline_in_frame(r.functions.size()) {
        for (std::size_t i = 0; i < r.functions.size(); ++i) {
            entries.push_back(code.label());
            unsigned budget = 40;
            inlineable[i] = r.functions[i]->children[0].children.size() <= 8 &&
                            small_leaf_body(r.functions[i]->children[1], budget);
        }
    }

    arm::Image build(const Node& root) {
        main_offset = code.position();
        function(-1, root);
        for (std::size_t i = 0; i < entries.size(); ++i) {
            function_offsets.push_back(code.position());
            code.bind(entries[i]);
            function(static_cast<int>(i), names.functions[i]->children[1]);
        }
        return code.seal();
    }

    Run run(const Node& root, const arm::Image& image) const {
        Run result{root, names, std::vector<V>(names.bindings.size(), V::undefined()),
                   image.entry<Entry>(main_offset), {}, {}};
        for (auto offset : function_offsets) result.targets.push_back(image.entry<Entry>(offset));
        return result;
    }
};

} // namespace

int quick_run(const Node& root) {
    auto resolution = resolve(root);
    auto types = infer(root, resolution);
    Compiler compiler(root, resolution, types);
    auto image = compiler.build(root);
    auto run = compiler.run(root, image);
    struct Restore {
        Run* saved;
        ~Restore() { active = saved; }
    } restore{active};
    active = &run;
    return rt::run(&enter);
}

const char* quick_function_display(std::int32_t id) {
    if (!active || id < 0 || static_cast<std::size_t>(id) >= active->names.functions.size())
        rt::fail("Invalid function");
    if (!active->displays) active->displays = function_displays(active->root);
    return active->displays->at(active->names.functions[id]).c_str();
}

} // namespace epq

namespace rt {
const char* functionDisplay(std::int32_t id) { return epq::quick_function_display(id); }
} // namespace rt
