#include "resolve.hpp"

#include <stdexcept>
#include <optional>
#include <utility>

namespace epq {
namespace {

struct Frame {
    int owner;
    std::unordered_map<std::string, int> locals;
    std::vector<std::string> names;
};

using Assigned = std::unordered_set<int>;

void intersect(Assigned& target, const Assigned& other) {
    for (auto it = target.begin(); it != target.end();) {
        if (other.count(*it)) ++it;
        else it = target.erase(it);
    }
}

class Initialization {
    Resolution& result;
    std::optional<Assigned> on_call;
    bool at_top = true;

    void visit(const Node& node, Assigned& assigned) {
        const auto& c = node.children;
        switch (node.op) {
            case Op::ID: {
                int id = result.id.at(&node);
                if (assigned.count(id)) result.initialized.insert(&node);
                else result.needs_init_check.insert(id);
                return;
            }
            case Op::SET:
                if (c[0].op == Op::ID) {
                    visit(c[1], assigned);
                    assigned.insert(result.id.at(&c[0]));
                } else if (c[0].op == Op::IND) {
                    visit(c[0], assigned);
                    visit(c[1], assigned);
                }
                return;
            case Op::CLS:
            case Op::GET:
            case Op::SGET:
            case Op::DEF:
                return;
            case Op::CAL:
                for (const auto& arg : c[1].children) visit(arg, assigned);
                visit(c[0], assigned);
                // Every call chain starts from some top-level call. Globals
                // assigned at all those sites stay initialized throughout every
                // function, even with rebinding, recursion, or indirect calls.
                if (at_top) {
                    if (on_call) intersect(*on_call, assigned);
                    else on_call = assigned;
                }
                return;
            case Op::IF: {
                visit(c[0], assigned);
                Assigned alternative = assigned;
                visit(c[1], assigned);
                visit(c[2], alternative);
                intersect(assigned, alternative);
                return;
            }
            case Op::AND:
            case Op::OR:
            case Op::WHL: {
                visit(c[0], assigned);
                Assigned conditional = assigned;
                visit(c[1], conditional);
                return;
            }
            case Op::FOR: {
                visit(c[0], assigned);
                visit(c[1], assigned);
                Assigned iteration = assigned;
                visit(c[3], iteration);
                visit(c[2], iteration);
                return;
            }
            case Op::RNG: {
                visit(c[1], assigned);
                assigned.insert(result.id.at(&c[0]));
                Assigned iteration = assigned;
                visit(c[2], iteration);
                return;
            }
            default:
                for (const auto& child : c) visit(child, assigned);
        }
    }

public:
    explicit Initialization(Resolution& result) : result(result) {}

    void run(const Node& root) {
        Assigned initial;
        visit(root, initial);
        at_top = false;
        Assigned globals;
        if (on_call) {
            for (int id : *on_call)
                if (result.bindings[id].global) globals.insert(id);
        }
        for (const Node* function : result.functions) {
            Assigned entry = globals;
            for (const auto& param : function->children[0].children)
                entry.insert(result.id.at(&param));
            visit(function->children[1], entry);
        }
    }
};

class Resolver {
    Resolution result;
    std::unordered_map<std::string, int> globals;

    int new_binding(const std::string& name, int owner, bool global, bool parameter) {
        int index = static_cast<int>(result.bindings.size());
        result.bindings.push_back({name, owner, global, parameter});
        return index;
    }

    // EPQ discovers global names ahead of execution, including names assigned
    // in top-level while/range expressions and in an if's condition.
    void collect_globals(const Node& node) {
        const auto& c = node.children;
        switch (node.op) {
            case Op::BLK:
            case Op::FOR:
            case Op::DEF:
                return;
            case Op::IF:
                collect_globals(c[0]);
                return;
            case Op::SET:
                if (c[0].op == Op::ID && !globals.count(c[0].name)) {
                    globals.emplace(c[0].name, new_binding(c[0].name, -1, true, false));
                }
                break;
            default:
                break;
        }
        for (const auto& child : c) collect_globals(child);
    }

    void bind(const Node& id, Frame& frame, bool write, bool parameter = false) {
        if (auto it = globals.find(id.name); it != globals.end()) {
            if (parameter) throw std::runtime_error("Argument shadows global variable: " + id.name);
            result.id.emplace(&id, it->second);
            return;
        }
        if (auto it = frame.locals.find(id.name); it != frame.locals.end()) {
            if (parameter) throw std::runtime_error("Duplicate argument name: " + id.name);
            result.id.emplace(&id, it->second);
            return;
        }
        if (!write) throw std::runtime_error("Undefined variable " + id.name);
        int slot = new_binding(id.name, frame.owner, false, parameter);
        frame.locals.emplace(id.name, slot);
        frame.names.push_back(id.name);
        result.id.emplace(&id, slot);
    }

    template<class Body>
    void scope(const Node& node, Frame& frame, Body body) {
        auto start = frame.names.size();
        body();
        while (frame.names.size() > start) {
            auto it = frame.locals.find(frame.names.back());
            result.declarations[&node].push_back(it->second);
            frame.locals.erase(it);
            frame.names.pop_back();
        }
    }

    void statement_scope(const Node& node, Frame& frame) {
        scope(node, frame, [&] { walk(node, frame); });
    }

    void define(const Node& node) {
        int index = static_cast<int>(result.functions.size());
        result.function.emplace(&node, index);
        result.functions.push_back(&node);
        Frame frame{index, {}, {}};
        for (const auto& param : node.children[0].children) {
            if (param.op != Op::ID) throw std::runtime_error("Invalid argument list");
            bind(param, frame, true, true);
        }
        walk(node.children[1], frame);
    }

    void walk(const Node& node, Frame& frame) {
        const auto& c = node.children;
        switch (node.op) {
            case Op::ID:
                bind(node, frame, false);
                return;
            case Op::SET:
                if (c[0].op == Op::ID) {
                    walk(c[1], frame);
                    bind(c[0], frame, true);
                } else if (c[0].op == Op::IND) {
                    walk(c[0], frame);
                    walk(c[1], frame);
                }
                return;
            case Op::DEF:
                define(node);
                return;
            case Op::CAL:
                // The argument list is not a list literal and introduces no scope.
                for (const auto& arg : c[1].children) walk(arg, frame);
                walk(c[0], frame);
                return;
            case Op::BLK:
            case Op::LST:
                scope(node, frame, [&] {
                    for (const auto& child : c) walk(child, frame);
                });
                return;
            case Op::IF:
            case Op::WHL:
                scope(node, frame, [&] {
                    walk(c[0], frame);
                    for (std::size_t i = 1; i < c.size(); ++i) statement_scope(c[i], frame);
                });
                return;
            case Op::FOR:
                scope(node, frame, [&] {
                    walk(c[0], frame);
                    walk(c[1], frame);
                    statement_scope(c[3], frame);
                    walk(c[2], frame);
                });
                return;
            case Op::RNG:
                scope(node, frame, [&] {
                    walk(c[1], frame);
                    if (!globals.count(c[0].name) && !frame.locals.count(c[0].name))
                        result.fresh_iterators.insert(&node);
                    bind(c[0], frame, true);
                    statement_scope(c[2], frame);
                });
                return;
            case Op::CLS:
            case Op::GET:
            case Op::SGET:
                // Only evaluation fails. An uncalled function can contain these
                // parsed productions and still be printed as a function value.
                return;
            default:
                for (const auto& child : c) walk(child, frame);
        }
    }

public:
    Resolution run(const Node& root) {
        collect_globals(root);
        Frame frame{-1, {}, {}};
        walk(root, frame);
        Initialization(result).run(root);
        return std::move(result);
    }
};

} // namespace

Resolution resolve(const Node& root) {
    return Resolver().run(root);
}

} // namespace epq
