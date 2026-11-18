#include "quick.hpp"
#include "display.hpp"
#include "loopopt.hpp"
#include "quick_arm.hpp"
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
    arm::Assembler code;
    std::vector<Label> entries;
    std::vector<std::size_t> locations;
    std::vector<unsigned> uses;
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
            case Op::CAL:
                return std::max(array_space(c[1].children), c[1].children.size() + scratch(c[0]));
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
            if (!loaded) {
                code.immediate(0, 3); // rt::V::undefined()
                loaded = true;
            }
            code.store(0, locations[binding]);
        }
    }

    void assign(const Node& id) {
        int b = names.id.at(&id);
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

    void operands(const Node& a, const Node& b, std::size_t slot) {
        expr(a, slot);
        if (leaf(b, 1)) return;
        code.store(0, slot);
        expr(b, slot + 1);
        code.mov(1, 0);
        code.load(slot, 0);
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
        values(args, slot);
        int known = types.expression(callee).function;
        if (known < 0 || (callee.op != Op::DEF &&
            !(callee.op == Op::ID && names.initialized.count(&callee)))) {
            expr(callee, slot + args.size());
        }
        if (known >= 0 && args.size() != names.functions[known]->children[0].children.size()) {
            host(&invalid_arity);
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

    template<Op op>
    void arithmetic(const Node& n, std::size_t slot, IntOp instruction) {
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
                operands(c[0], c[1], s);
                if (list(c[0]) && integer(c[1])) return code.knownListGet(bad_index);
                return host(&get);
            case Op::APP:
                operands(c[0], c[1], s);
                return host(&append);
            case Op::SET:
                if (c[0].op == Op::ID) {
                    expr(c[1], s);
                    assign(c[0]);
                } else if (c[0].op == Op::IND) {
                    const auto& target = c[0].children;
                    operands(target[0], target[1], s);
                    code.store(0, s);
                    code.store(1, s + 1);
                    expr(c[1], s + 2);
                    code.mov(2, 0);
                    code.load(s, 0);
                    code.load(s + 1, 1);
                    if (list(target[0]) && integer(target[1])) code.knownListSet(bad_index);
                    else host(&set);
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
            case Op::EXP: expr(c[0], s); break;
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
                condition(c[0], s, alternative);
                stmt(c[1], s);
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
                    host(&bulk_fill);
                    assign(*fill->start_read);
                    code.bind(done);
                    break;
                }
                const auto top = code.label(), done = code.label();
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
    }

    void function(int id, const Node& body) {
        count_uses(body, 1);
        std::vector<int> locals;
        for (std::size_t i = 0; i < names.bindings.size(); ++i)
            if (!names.bindings[i].global && names.bindings[i].owner == id)
                locals.push_back(static_cast<int>(i));
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
    Compiler(const Resolution& r, const Types& t)
        : names(r), types(t), locations(r.bindings.size()), uses(r.bindings.size()) {
        for (std::size_t i = 0; i < r.functions.size(); ++i) entries.push_back(code.label());
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
    Compiler compiler(resolution, types);
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
