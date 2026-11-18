#pragma once

#include "resolve.hpp"

namespace epq {

enum class Kind { Dynamic, Int, List, Function };

struct Type {
    Kind kind = Kind::Dynamic;
    int function = -1; // Literal index if this value always holds the same function.
};

class Types {
public:
    Type expression(const Node& node) const;
    Type element(const Node& list_expression) const;
    Type binding(int id) const { return bindings.at(id); }
    Type result(int function) const { return results.at(function); }

private:
    friend Types infer(const Node&, const Resolution&);
    std::unordered_map<const Node*, Type> expressions;
    std::unordered_map<const Node*, Type> elements;
    std::vector<Type> bindings;
    std::vector<Type> results;
};

Types infer(const Node& root, const Resolution& resolution);

} // namespace epq
