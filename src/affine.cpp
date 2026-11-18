#include "affine.hpp"

#include <algorithm>

namespace epq {
namespace {

class Transfer {
    using Row = AffineLoop::Row;
    static constexpr unsigned constant = AffineLoop::limit - 1;
    const Resolution& res;
    const Types& types;
    int counter;
    AffineLoop model;

    int coordinate(const Node& name) {
        int id = res.id.at(&name);
        if (id == counter || types.binding(id).kind != Kind::Int) return -1;
        auto it = std::find(model.bindings.begin(), model.bindings.end(), id);
        if (it != model.bindings.end()) return static_cast<int>(it - model.bindings.begin());
        if (model.bindings.size() == constant) return -1;
        model.bindings.push_back(id);
        return static_cast<int>(model.bindings.size() - 1);
    }

    // Symbolically execute assignments in source order, substituting each
    // binding's current row on a read. All coefficients wrap like EPQ integers.
    bool expression(const Node& n, Row& out) {
        const auto& c = n.children;
        switch (n.op) {
        case Op::INT:
            out[constant] = static_cast<std::uint32_t>(n.number);
            return true;
        case Op::ID: {
            int i = coordinate(n);
            if (i < 0 || !res.initialized.count(&n)) return false;
            if (!model.writes[i]) model.reads_initial[i] = true;
            out = model.matrix[i];
            return true;
        }
        case Op::NEG:
            if (!expression(c[0], out)) return false;
            for (auto& x : out) x = 0u - x;
            return true;
        case Op::MUL: {
            unsigned literal = c[0].op == Op::INT ? 0 : 1;
            if (c[literal].op != Op::INT || !expression(c[1 - literal], out)) return false;
            for (auto& x : out) x *= static_cast<std::uint32_t>(c[literal].number);
            return true;
        }
        case Op::ADD: case Op::SUB: {
            Row right{};
            if (!expression(c[0], out) || !expression(c[1], right)) return false;
            for (unsigned i = 0; i < out.size(); ++i)
                out[i] += n.op == Op::ADD ? right[i] : 0u - right[i];
            return true;
        }
        default: return false;
        }
    }

    bool statements(const Node& n) {
        if (n.op == Op::BLK || n.op == Op::SEQ) {
            for (const Node& child : n.children) if (!statements(child)) return false;
            return true;
        }
        if (n.op != Op::EXP) return false;
        const Node& e = n.children[0];
        if (e.op != Op::SET || e.children[0].op != Op::ID) return false;
        Row value{};
        if (!expression(e.children[1], value)) return false;
        int i = coordinate(e.children[0]);
        if (i < 0) return false;
        model.matrix[i] = value;
        model.writes[i] = true;
        return true;
    }

public:
    Transfer(const Resolution& r, const Types& t, int index) : res(r), types(t), counter(index) {
        for (unsigned i = 0; i < AffineLoop::limit; ++i) model.matrix[i][i] = 1;
    }
    std::optional<AffineLoop> run(const Node& body, int bound) {
        if (!statements(body) || model.bindings.empty()) return {};
        for (unsigned i = 0; i < model.bindings.size(); ++i)
            if (model.writes[i] && model.bindings[i] == bound) return {};
        return model;
    }
};
}

std::optional<AffineLoop> match_affine(const Node& loop, const Resolution& res, const Types& types) {
    if (loop.op != Op::FOR) return {};
    const auto& c = loop.children;
    const Node& test = c[1];
    if (test.op != Op::LT || test.children[0].op != Op::ID) return {};
    const Node& index = test.children[0];
    const Node& bound = test.children[1];
    if (types.expression(index).kind != Kind::Int || types.expression(bound).kind != Kind::Int) return {};
    if (bound.op != Op::ID && bound.op != Op::INT) return {};
    int id = res.id.at(&index);
    int limit = bound.op == Op::ID ? res.id.at(&bound) : -1;
    if (id == limit || !increments_by_one(c[2], id, res)) return {};
    return Transfer(res, types, id).run(c[3], limit);
}
}
