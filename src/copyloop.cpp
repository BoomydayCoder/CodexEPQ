#include "copyloop.hpp"
#include "loopopt.hpp"

namespace epq {

std::optional<CopyLoop> match_copy_loop(const Node& loop, const Resolution& names,
                                        const Types& types) {
    const bool counted = loop.op == Op::FOR;
    if (!counted && loop.op != Op::WHL) return {};
    const auto& parts = loop.children;
    const Node& test = parts[counted ? 1 : 0];
    if (test.op != Op::LT) return {};
    const Node& index = test.children[0];
    const Node& bound = test.children[1];
    if (index.op != Op::ID || types.expression(index).kind != Kind::Int) return {};
    const int counter = names.id.at(&index);

    const Node* body = &parts[counted ? 3 : 1];
    const Node* increment = nullptr;
    if (counted) {
        increment = &parts[2];
    } else {
        // A while loop writes back its counter in the body.
        while ((body->op == Op::BLK || body->op == Op::SEQ) && body->children.size() == 1)
            body = &body->children.front();
        if (body->op != Op::SEQ || body->children.size() != 2) return {};
        increment = &body->children[1];
        body = &body->children[0];
    }
    const Node* append = expression_statement(*body);
    if (!append || append->op != Op::APP || !increments_by_one(*increment, counter, names)) return {};
    const Node& dst = append->children[0];
    const Node& element = append->children[1];
    if (dst.op != Op::ID || types.expression(dst).kind != Kind::List || element.op != Op::IND) return {};
    const Node& src = element.children[0];
    const Node& subscript = element.children[1];
    if (src.op != Op::ID || types.expression(src).kind != Kind::List || subscript.op != Op::ID
        || names.id.at(&subscript) != counter) return {};

    const bool live = bound.op == Op::ABS && bound.children[0].op == Op::ID
        && names.id.at(&bound.children[0]) == names.id.at(&src);
    if (!live && bound.op != Op::INT && !(bound.op == Op::ID && names.id.at(&bound) != counter
                                         && types.expression(bound).kind == Kind::Int)) return {};
    return CopyLoop{index, bound, dst, src, live};
}

} // namespace epq
