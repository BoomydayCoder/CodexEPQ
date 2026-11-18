#pragma once

#include "ast.hpp"

namespace epq {

// Emit a complete C++ translation unit for the native runtime.
std::string emit(const Node& program);

} // namespace epq
