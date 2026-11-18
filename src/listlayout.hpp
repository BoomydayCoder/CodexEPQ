#pragma once

#include "types.hpp"

namespace epq {

// A single fresh-list binding can use private storage when every operation on
// its identity is visible, including operations from functions using a global.
enum class ListLayout { Ordinary, Byte, Int, Count };
std::vector<ListLayout> list_layouts(const Node& root, const Resolution& res, const Types& types);

} // namespace epq
