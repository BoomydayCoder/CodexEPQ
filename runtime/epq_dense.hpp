#pragma once

#include "epq_runtime.hpp"
#include <vector>

namespace rt {

// Used for bindings whose list reference never escapes. Index checks and the
// maximum length are identical to the tagged-value list representation.
class ListCount {
    std::int32_t count = 0;
public:
    void reset(std::int32_t size) { count = size; }
    std::int32_t size() const { return count; }
    void check(std::int32_t i) const {
        if (std::uint32_t(i) >= std::uint32_t(count)) fail("Index out of bounds");
    }
    void extend(std::int64_t n) {
        if (n > INT32_MAX - count) fail("List is too large");
        count += static_cast<std::int32_t>(n);
    }
};

template<typename T>
class DenseList {
    std::vector<T> items;
public:
    void reset(std::initializer_list<std::int32_t> values) {
        items.assign(values.begin(), values.end());
    }
    std::int32_t size() const { return static_cast<std::int32_t>(items.size()); }
    std::int32_t get(std::int32_t i) const {
        if (std::uint32_t(i) >= items.size()) fail("Index out of bounds");
        return items[i];
    }
    void set(std::int32_t i, std::int32_t value) {
        if (std::uint32_t(i) >= items.size()) fail("Index out of bounds");
        items[i] = static_cast<T>(value);
    }
    void append(std::int32_t value) {
        if (items.size() == INT32_MAX) fail("List is too large");
        items.push_back(static_cast<T>(value));
    }
    void extend(std::int64_t n, std::int32_t value) {
        if (n > INT32_MAX - size()) fail("List is too large");
        items.insert(items.end(), static_cast<std::size_t>(n), static_cast<T>(value));
    }
};

// Called only after the first true comparison of an ascending unit-step loop.
inline std::int64_t fillCount(std::int32_t start, std::int32_t bound, bool inclusive) {
    if (inclusive && bound == INT32_MAX) fail("List is too large");
    return std::int64_t(bound) - start + inclusive;
}

} // namespace rt
