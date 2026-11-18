#include "loopopt.hpp"

namespace epq {
const Node* expression_statement(const Node& statement) {
    const Node* node = &statement;
    while ((node->op == Op::BLK || node->op == Op::SEQ) && node->children.size() == 1)
        node = &node->children.front();
    return node->op == Op::EXP ? &node->children.front() : nullptr;
}

bool increments_by_one(const Node& statement, int binding, const Resolution& bindings) {
    const Node* step = expression_statement(statement);
    auto counter = [&](const Node& n) {
        return n.op == Op::ID && bindings.id.at(&n) == binding;
    };
    auto one = [](const Node& n) { return n.op == Op::INT && n.number == 1; };
    if (!step || step->op != Op::SET || !counter(step->children[0])) return false;
    const Node& add = step->children[1];
    if (add.op != Op::ADD) return false;
    const auto& terms = add.children;
    return (counter(terms[0]) && one(terms[1])) || (one(terms[0]) && counter(terms[1]));
}

std::optional<FillLoop> match_fill_loop(const Node& loop, const Resolution& bindings,
                                        const Types& types) {
    if (loop.op != Op::FOR) return {};
    const auto& c = loop.children;
    const Node* init = expression_statement(c[0]);
    if (!init || init->op != Op::SET || init->children[0].op != Op::ID) return {};
    const int counter = bindings.id.at(&init->children[0]);
    if (types.binding(counter).kind != Kind::Int) return {};

    auto is_counter = [&](const Node& node) {
        return node.op == Op::ID && bindings.id.at(&node) == counter;
    };
    auto invariant = [&](const Node& node) {
        return node.op == Op::INT || (node.op == Op::ID && !is_counter(node));
    };

    const Node& test = c[1];
    if (test.op != Op::LT && test.op != Op::LE) return {};
    const auto& start = test.children[0];
    const auto& bound = test.children[1];
    if (!is_counter(start) || !invariant(bound) || types.expression(bound).kind != Kind::Int) return {};

    if (!increments_by_one(c[2], counter, bindings)) return {};

    const Node* body = expression_statement(c[3]);
    if (!body || body->op != Op::APP) return {};
    const auto& list = body->children[0];
    const auto& value = body->children[1];
    // Re-reading a binding returns the same reference even when appending the
    // destination list to itself. A fresh list literal would have new identity
    // on each iteration, and an index/length could change on each append.
    if (list.op != Op::ID || is_counter(list) || !invariant(value)) return {};
    return FillLoop{counter, &start, &bound, &list, &value, test.op == Op::LE};
}

} // namespace epq
