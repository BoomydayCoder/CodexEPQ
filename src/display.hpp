#pragma once

#include "ast.hpp"
#include <unordered_map>

namespace epq {

// A function's printed value includes its original bytecode listing.
using FunctionDisplays = std::unordered_map<const Node*, std::string>;
FunctionDisplays function_displays(const Node& program);

} // namespace epq
