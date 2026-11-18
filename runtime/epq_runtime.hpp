#pragma once

#include <cstddef>
#include <cstdint>
#include <initializer_list>

namespace rt {

struct List;
[[noreturn, gnu::cold]] void fail(const char* message);

// Two low bits distinguish an integer, a list address, a function literal and
// an uninitialised binding. Signed integers retain their order when tagged.
class V {
    std::uintptr_t word;
    explicit constexpr V(std::uintptr_t bits) : word(bits) {}

public:
    // V{} is integer zero. A list can allocate unused capacity without writing
    // those words, since only its first size elements are ever read.
    V() = default;
    static constexpr V integer(std::int32_t n) {
        return V(static_cast<std::uintptr_t>(std::int64_t(n) * 4));
    }
    static V list(List* p) { return V(reinterpret_cast<std::uintptr_t>(p) | 1u); }
    static constexpr V function(std::int32_t id) {
        return V((std::uintptr_t(id) << 2) | 2u);
    }
    static constexpr V undefined() { return V(3); }

    constexpr bool isInt() const { return (word & 3) == 0; }
    constexpr bool isList() const { return (word & 3) == 1; }
    constexpr bool isFunction() const { return (word & 3) == 2; }
    constexpr bool isUndefined() const { return word == 3; }
    constexpr std::int32_t intUnchecked() const {
        auto n = static_cast<std::int32_t>(word >> 2);
        // Every producer sign-extends int32 before tagging. Let Clang carry an
        // already-tagged integer through a load, comparison and store unchanged.
        __builtin_assume(word == static_cast<std::uintptr_t>(std::int64_t(n) * 4));
        return n;
    }
    List* listUnchecked() const { return reinterpret_cast<List*>(word - 1); }
    std::int32_t asInt() const {
        if (!isInt()) fail("Operand must be an integer");
        return intUnchecked();
    }
    List* asList() const {
        if (!isList()) fail("Operand must be a list");
        return listUnchecked();
    }
    std::int32_t asFunction() const {
        if (!isFunction()) fail("Operand must be a function");
        return static_cast<std::int32_t>(word >> 2);
    }
    constexpr bool sameWord(V other) const { return word == other.word; }
    constexpr bool intWordLess(V other) const {
        return static_cast<std::intptr_t>(word) < static_cast<std::intptr_t>(other.word);
    }
};
static_assert(sizeof(V) == 8, "EPQ requires a 64-bit target");

struct List {
    V* data;
    std::int32_t size;
    std::int32_t capacity;
    V small[2];
};

List* makeList(std::initializer_list<V> items = {});
List* makeList(const V* items, std::size_t count);
// Capacity is always in V-sized words, including for compiler-proven packed lists.
void resizeBacking(List* list, std::int32_t capacity, std::size_t bytesToCopy);
void grow(List* list);
std::int32_t fillLoop(List* list, V value, std::int32_t start, std::int32_t bound, bool inclusive);
std::int32_t copyLoop(List* destination, List* source, std::int32_t start,
                      std::int32_t bound, bool usesSourceLength);

inline V get(List* list, std::int32_t index) {
    if (std::uint32_t(index) >= std::uint32_t(list->size)) fail("Index out of bounds");
    return list->data[index];
}
inline V set(List* list, std::int32_t index, V value) {
    if (std::uint32_t(index) >= std::uint32_t(list->size)) fail("Index out of bounds");
    return list->data[index] = value;
}
inline List* append(List* list, V value) {
    const auto size = list->size;
    if (size == list->capacity) grow(list);
    list->data[size] = value;
    list->size = size + 1;
    return list;
}
inline V pop(List* list) {
    if (list->size == 0) fail("List is empty");
    return list->data[--list->size];
}
inline std::int32_t length(List* list) { return list->size; }

// Used only when the compiler proves both lists contain integers. Comparing
// the signed tagged words directly also works for negative integers.
inline bool intListsLess(const List* a, const List* b) {
    auto n = a->size < b->size ? a->size : b->size;
    for (std::int32_t i = 0; i < n; ++i) {
        V left = a->data[i], right = b->data[i];
        if (!left.sameWord(right)) return left.intWordLess(right);
    }
    return a->size < b->size;
}
inline bool intListsEqual(const List* a, const List* b) {
    return a->size == b->size && __builtin_memcmp(a->data, b->data, std::size_t(a->size) * sizeof(V)) == 0;
}

// Unsigned arithmetic gives overflow one definition across compiler settings.
inline std::int32_t add(std::int32_t a, std::int32_t b) {
    return static_cast<std::int32_t>(std::uint32_t(a) + std::uint32_t(b));
}
inline std::int32_t sub(std::int32_t a, std::int32_t b) {
    return static_cast<std::int32_t>(std::uint32_t(a) - std::uint32_t(b));
}
inline std::int32_t mul(std::int32_t a, std::int32_t b) {
    return static_cast<std::int32_t>(std::uint32_t(a) * std::uint32_t(b));
}
inline std::int32_t negate(std::int32_t a) { return sub(0, a); }
inline std::int32_t absolute(std::int32_t a) { return a < 0 ? negate(a) : a; }
inline std::int32_t divide(std::int32_t a, std::int32_t b) {
    if (b == 0) fail("Division by zero");
    if (b == -1) return negate(a);
    return a / b;
}

inline V neg(V value) {
    return value.isInt() ? V::integer(negate(value.intUnchecked())) : pop(value.asList());
}
inline std::int32_t abs(V value) {
    return value.isInt() ? absolute(value.intUnchecked()) : length(value.asList());
}
inline bool truth(V value) {
    if (value.isInt()) return value.intUnchecked() != 0;
    if (value.isUndefined()) fail("Undefined variable");
    return value.isFunction() || length(value.listUnchecked()) != 0;
}
bool listsEqual(const List* left, const List* right);
bool listsLess(const List* left, const List* right);
inline bool equal(V a, V b) {
    if (a.sameWord(b)) return true;
    return a.isList() && b.isList() && listsEqual(a.listUnchecked(), b.listUnchecked());
}
inline bool less(V a, V b) {
    if (a.isInt() && b.isInt()) return a.intWordLess(b);
    if (a.isList() && b.isList()) return listsLess(a.listUnchecked(), b.listUnchecked());
    fail("Invalid comparison");
}

// The emitter provides the legacy printed representation for each literal.
const char* functionDisplay(std::int32_t id);
// Apply a small integer linear transformation repeatedly modulo 2^32.
constexpr unsigned maxAffineDimension = 5;
void applyAffine(std::uint32_t* state, const std::uint32_t* matrix, unsigned size, std::uint32_t count);
std::int32_t input();
void print(V value);
int run(void (*entry)());

} // namespace rt
