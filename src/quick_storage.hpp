#pragma once

#include "epq_runtime.hpp"
#include "listlayout.hpp"

namespace epq {

// The list header stays at its ordinary address. Only the payload of a
// provably private binding has a specialized representation.
struct PrivateListOps {
    unsigned width;
    rt::V (*literal)(const rt::V*, std::size_t);
    rt::V (*append)(rt::V, rt::V);
    rt::V (*fill)(rt::V, rt::V, rt::V, rt::V, std::uint64_t);
    rt::V (*get)(rt::V, rt::V);
    rt::V (*set)(rt::V, rt::V, rt::V);
};

const PrivateListOps* private_list_ops(ListLayout layout);

} // namespace epq
