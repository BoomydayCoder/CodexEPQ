#pragma once

#include "types.hpp"

#include <optional>

namespace epq {

const Node* expression_statement(const Node& statement);
bool increments_by_one(const Node& statement, int binding, const Resolution& bindings);

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
