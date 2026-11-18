#pragma once

#include "loopopt.hpp"

#include <array>

namespace epq {

// The final coordinate is always 1, so each row can include a constant.
struct AffineLoop {
    static constexpr unsigned limit = 5;
    using Row = std::array<std::uint32_t, limit>;
    std::array<Row, limit> matrix{};
    std::vector<int> bindings;
    std::array<bool, limit> reads_initial{}, writes{};
};

std::optional<AffineLoop> match_affine(const Node&, const Resolution&, const Types&);
}
