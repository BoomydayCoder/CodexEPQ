#include "listlayout.hpp"

namespace epq {
namespace {

struct Uses {
    unsigned initializers = 0;
    bool escapes = false;
    bool read_element = false;
    bool integers = true;
    bool bytes = true;
};

class LayoutAnalysis {
    const Resolution& res;
    const Types& types;
    std::vector<Uses> uses;
    bool in_function = false;

    Uses& use(const Node& id) { return uses[res.id.at(&id)]; }
    void record_value(Uses& u, const Node& value) {
        u.integers &= types.expression(value).kind == Kind::Int;
        u.bytes &= value.op == Op::INT && value.number >= 0 && value.number <= 255;
        visit(value);
    }
    void visit(const Node& n) {
        const auto& c = n.children;
        switch (n.op) {
        case Op::CLS: case Op::SGET: case Op::GET: return;
        case Op::ID: use(n).escapes = true; return;
        case Op::DEF: {
            bool enclosing = in_function;
            in_function = true;
            for (const auto& child : c) visit(child);
            in_function = enclosing;
            return;
        }
        case Op::EXP: {
            const Node& e = c[0];
            const auto& args = e.children;
            if (e.op == Op::SET && args[0].op == Op::ID && args[1].op == Op::LST) {
                Uses& u = use(args[0]);
                ++u.initializers;
                // A callable initializer can replace a global between evaluation
                // of a subscript's base and its index/RHS. That base must retain
                // the old object's identity.
                if (in_function && res.bindings[res.id.at(&args[0])].global) u.escapes = true;
                for (const Node& value : args[1].children) record_value(u, value);
                return;
            }
            if (e.op == Op::APP && args[0].op == Op::ID) {
                record_value(use(args[0]), args[1]);
                return;
            }
            break;
        }
        case Op::SET:
            if (c[0].op != Op::IND && c[0].op != Op::ID) return;
            if (c[0].op == Op::IND && c[0].children[0].op == Op::ID) {
                visit(c[0].children[1]);
                record_value(use(c[0].children[0]), c[1]);
                return;
            }
            break;
        case Op::IND:
            if (c[0].op == Op::ID) {
                use(c[0]).read_element = true;
                visit(c[1]);
                return;
            }
            break;
        case Op::ABS:
            if (c[0].op == Op::ID) return;
            break;
        default: break;
        }
        for (const auto& child : c) visit(child);
    }

public:
    LayoutAnalysis(const Resolution& r, const Types& t)
        : res(r), types(t), uses(r.bindings.size()) {}
    std::vector<ListLayout> run(const Node& root) {
        visit(root);
        std::vector<ListLayout> result(uses.size(), ListLayout::Ordinary);
        for (std::size_t i = 0; i < uses.size(); ++i) {
            const auto& u = uses[i];
            if (u.initializers != 1 || u.escapes || types.binding(i).kind != Kind::List) continue;
            if (!u.read_element) result[i] = ListLayout::Count;
            else if (u.bytes) result[i] = ListLayout::Byte;
            else if (u.integers) result[i] = ListLayout::Int;
        }
        return result;
    }
};

} // namespace

std::vector<ListLayout> list_layouts(const Node& root, const Resolution& res, const Types& types) {
    return LayoutAnalysis(res, types).run(root);
}

} // namespace epq
