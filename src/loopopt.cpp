#include "loopopt.hpp"

namespace epq {
namespace {

const Node* expression_statement(const Node& statement) {
    const Node* node = &statement;
    while ((node->op == Op::BLK || node->op == Op::SEQ) && node->children.size() == 1)
        node = &node->children.front();
    return node->op == Op::EXP ? &node->children.front() : nullptr;
}

} // namespace

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

    const Node* step = expression_statement(c[2]);
    if (!step || step->op != Op::SET || !is_counter(step->children[0])) return {};
    const Node& add = step->children[1];
    if (add.op != Op::ADD) return {};
    const Node& left = add.children[0];
    const Node& right = add.children[1];
    auto is_one = [](const Node& node) { return node.op == Op::INT && node.number == 1; };
    if (!(is_counter(left) && is_one(right)) && !(is_one(left) && is_counter(right))) return {};

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
