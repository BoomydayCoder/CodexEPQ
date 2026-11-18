#include "quick_storage.hpp"

#include <algorithm>
#include <climits>
#include <cstring>

namespace epq {
namespace {

using rt::List;
using rt::V;

template<unsigned Width>
void reserve(List* list, std::int64_t size) {
    if (size > INT32_MAX) rt::fail("List is too large");
    if constexpr (Width != 0) {
        // capacity is measured in physical V words for all pool allocations.
        const auto words = (size * Width + sizeof(V) - 1) / sizeof(V);
        if (words <= static_cast<std::size_t>(list->capacity)) return;
        const auto grown = std::min<std::int64_t>(INT32_MAX, std::int64_t(list->capacity) * 2);
        const auto capacity = static_cast<std::int32_t>(std::max<std::int64_t>(words, grown));
        rt::resizeBacking(list, capacity, std::size_t(list->size) * Width);
    }
}

// The backing store is allocated as V[]. Access its object representation
// through bytes, preserving C++ aliasing rules for the packed int32 form.
template<unsigned Width>
void store(List* list, std::int32_t index, V item) {
    if constexpr (Width > 0) {
        auto* bytes = reinterpret_cast<unsigned char*>(list->data);
        auto n = item.intUnchecked();
        if constexpr (Width == 1) bytes[index] = static_cast<unsigned char>(n);
        else std::memcpy(bytes + std::size_t(index) * Width, &n, Width);
    }
}

template<unsigned Width>
V literal(const V* items, std::size_t count) {
    if (count > INT32_MAX) rt::fail("List is too large");
    auto* list = rt::makeList({});
    reserve<Width>(list, count);
    if constexpr (Width != 0)
        for (std::int32_t i = 0; std::size_t(i) < count; ++i) store<Width>(list, i, items[i]);
    list->size = static_cast<std::int32_t>(count);
    return V::list(list);
}

template<unsigned Width>
V append(V base, V item) {
    auto* list = base.listUnchecked();
    reserve<Width>(list, std::int64_t(list->size) + 1);
    store<Width>(list, list->size, item);
    ++list->size;
    return base;
}

template<unsigned Width>
V fill(V base, V item, V first, V limit, std::uint64_t inclusive) {
    auto* list = base.listUnchecked();
    const std::int64_t end = std::int64_t(limit.intUnchecked()) + (inclusive != 0);
    if (end > INT32_MAX) rt::fail("List is too large");
    const std::int64_t count = end - first.intUnchecked();
    const std::int64_t size = list->size + count;
    reserve<Width>(list, size);
    if constexpr (Width == 1) {
        auto* bytes = reinterpret_cast<unsigned char*>(list->data);
        std::memset(bytes + list->size, item.intUnchecked(), static_cast<std::size_t>(count));
    } else if constexpr (Width != 0) {
        for (std::int32_t i = list->size; i < size; ++i) store<Width>(list, i, item);
    }
    list->size = static_cast<std::int32_t>(size);
    return V::integer(static_cast<std::int32_t>(end));
}

std::int32_t checked_index(List* list, V index) {
    auto i = index.asInt();
    if (std::uint32_t(i) >= std::uint32_t(list->size)) rt::fail("Index out of bounds");
    return i;
}

template<unsigned Width>
V get(V base, V index) {
    static_assert(Width > 0);
    auto* list = base.listUnchecked();
    auto i = checked_index(list, index);
    const auto* bytes = reinterpret_cast<const unsigned char*>(list->data);
    if constexpr (Width == 1) return V::integer(bytes[i]);
    std::int32_t value;
    std::memcpy(&value, bytes + std::size_t(i) * Width, Width);
    return V::integer(value);
}

template<unsigned Width>
V set(V base, V index, V item) {
    auto* list = base.listUnchecked();
    store<Width>(list, checked_index(list, index), item);
    return item;
}

template<unsigned Width>
constexpr PrivateListOps operations() {
    PrivateListOps ops{Width, literal<Width>, append<Width>, fill<Width>, nullptr, set<Width>};
    if constexpr (Width > 0) ops.get = get<Width>;
    return ops;
}

} // namespace

const PrivateListOps* private_list_ops(ListLayout layout) {
    static constexpr auto count = operations<0>();
    static constexpr auto byte = operations<1>();
    static constexpr auto integer = operations<4>();
    switch (layout) {
        case ListLayout::Count: return &count;
        case ListLayout::Byte: return &byte;
        case ListLayout::Int: return &integer;
        case ListLayout::Ordinary: return nullptr;
    }
    return nullptr;
}

} // namespace epq
