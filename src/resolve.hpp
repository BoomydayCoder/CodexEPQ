#pragma once

#include "ast.hpp"

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace epq {

struct Binding {
    std::string name;
    int owner;       // Function index, or -1 for the outer program.
    bool global;
    bool parameter;
};

// Node keys refer to the original AST, which must outlive this result.
struct Resolution {
    std::vector<Binding> bindings;
    std::unordered_map<const Node*, int> id;
    std::vector<const Node*> functions;
    std::unordered_map<const Node*, int> function;
    // Reads with an assignment on every incoming path may skip the init check.
    std::unordered_set<const Node*> initialized;
    // Bindings for which at least one actual read needs a runtime check.
    std::unordered_set<int> needs_init_check;
    // Local slots that begin a new lifetime on entering a lexical scope.
    // The scope of a loop condition spans the loop; the body has its own entry.
    std::unordered_map<const Node*, std::vector<int>> declarations;
    // A new range binding is inaccessible outside its body, so the initial
    // integer zero cannot be observed before it receives the first element.
    std::unordered_set<const Node*> fresh_iterators;
};

Resolution resolve(const Node& root);

} // namespace epq
