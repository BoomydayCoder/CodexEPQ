#pragma once

#include "types.hpp"

#include <optional>

namespace epq {

struct CopyLoop {
    const Node& index;
    const Node& bound;
    const Node& destination;
    const Node& source;
    bool uses_source_length;
};

std::optional<CopyLoop> match_copy_loop(const Node& loop, const Resolution& names,
                                        const Types& types);

} // namespace epq
