#pragma once

#include "types.hpp"

#include <optional>

namespace epq {

// Replace repeated, identical appends with one bulk fill. The emitter still
// executes the initializer and the first comparison before reading list/value.
struct FillLoop {
    int counter;
    const Node* start_read;
    const Node* bound;
    const Node* list;
    const Node* value;
    bool inclusive;
};

std::optional<FillLoop> match_fill_loop(const Node& loop, const Resolution& bindings,
                                        const Types& types);

} // namespace epq
