#include "epq_runtime.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

namespace rt {

[[noreturn]] void fail(const char* message) {
    std::fprintf(stderr, "%s\n", message);
    std::exit(1);
}

namespace {

// List headers have stable addresses, including while the element array grows.
// Small literals (such as [distance, vertex]) fit entirely in their header.
struct Slab {
    Slab* previous;
    unsigned used;
    List lists[256];
};

struct ListPool {
    Slab* last = nullptr;

    List* allocate() {
        if (!last || last->used == 256) {
            auto* next = new Slab;
            next->previous = last;
            next->used = 0;
            last = next;
        }
        return &last->lists[last->used++];
    }

    ~ListPool() {
        while (last) {
            for (unsigned i = 0; i < last->used; ++i) {
                List& list = last->lists[i];
                if (list.data != list.small) delete[] list.data;
            }
            Slab* previous = last->previous;
            delete last;
            last = previous;
        }
    }
};
ListPool pool;

// Immutable ancestor chains keep all state off the heap in ordinary nested
// comparisons. Following the same pair twice introduces no new difference.
struct PairPath {
    const List* left;
    const List* right;
    const PairPath* parent;
};

bool revisited(const PairPath* path, const List* left, const List* right) {
    for (; path; path = path->parent) {
        if (path->left == left && path->right == right) return true;
    }
    return false;
}

bool equalElements(const List* left, const List* right, const PairPath* parent) {
    if (left->size != right->size) return false;
    if (revisited(parent, left, right)) return true;
    const PairPath here{left, right, parent};
    for (std::int32_t i = 0; i < left->size; ++i) {
        V a = left->data[i], b = right->data[i];
        if (a.sameWord(b)) continue;
        if (!a.isList() || !b.isList()) return false;
        if (!equalElements(a.listUnchecked(), b.listUnchecked(), &here)) return false;
    }
    return true;
}

int compareElements(const List* left, const List* right, const PairPath* parent) {
    if (revisited(parent, left, right)) return 0;
    const PairPath here{left, right, parent};
    const auto count = std::min(left->size, right->size);
    for (std::int32_t i = 0; i < count; ++i) {
        V a = left->data[i], b = right->data[i];
        if (a.isInt() && b.isInt()) {
            if (a.sameWord(b)) continue;
            return a.intWordLess(b) ? -1 : 1;
        }
        if (!a.isList() || !b.isList()) fail("Invalid comparison");
        int nested = compareElements(a.listUnchecked(), b.listUnchecked(), &here);
        if (nested) return nested;
    }
    return (left->size > right->size) - (left->size < right->size);
}

struct PrintPath {
    const List* list;
    const PrintPath* parent;
};

class Printer {
    char buffer[8192];
    std::size_t used = 0;

    void reserve(std::size_t count) {
        if (sizeof(buffer) - used < count) flush();
    }

    void character(char c) {
        reserve(1);
        buffer[used++] = c;
    }

public:
    void flush() {
        std::fwrite(buffer, 1, used, stdout);
        used = 0;
    }

    void write(V value, const PrintPath* parent = nullptr) {
        if (value.isInt()) {
            reserve(12);
            used = std::to_chars(buffer + used, buffer + sizeof(buffer), value.intUnchecked()).ptr - buffer;
        } else if (value.isList()) {
            const List* list = value.listUnchecked();
            for (auto* path = parent; path; path = path->parent) {
                if (path->list == list) fail("Cannot print cyclic list");
            }
            const PrintPath path{list, parent};
            character('[');
            for (std::int32_t i = 0; i < list->size; ++i) {
                if (i) {
                    reserve(2);
                    buffer[used++] = ',';
                    buffer[used++] = ' ';
                }
                write(list->data[i], &path);
            }
            character(']');
        } else if (value.isFunction()) {
            flush();
            std::fputs(functionDisplay(value.asFunction()), stdout);
        } else {
            fail("Undefined variable");
        }
    }
};

bool whitespace(int c) {
    return c == ' ' || (c >= '\t' && c <= '\r');
}

} // namespace

List* makeList(std::initializer_list<V> items) {
    return makeList(items.begin(), items.size());
}

List* makeList(const V* items, std::size_t count) {
    if (count > INT32_MAX) fail("List is too large");
    List* list = pool.allocate();
    list->size = static_cast<std::int32_t>(count);
    list->capacity = std::max(list->size, 2);
    list->data = list->capacity == 2 ? list->small : new V[list->capacity];
    if (count) std::memcpy(list->data, items, count * sizeof(V));
    return list;
}

static void expand(List* list, std::int32_t capacity) {
    V* data = new V[capacity];
    std::memcpy(data, list->data, std::size_t(list->size) * sizeof(V));
    if (list->data != list->small) delete[] list->data;
    list->data = data;
    list->capacity = capacity;
}

void grow(List* list) {
    if (list->capacity == INT32_MAX) fail("List is too large");
    std::int32_t capacity = list->capacity > INT32_MAX / 2 ? INT32_MAX : list->capacity * 2;
    expand(list, capacity);
}

std::int32_t fillLoop(List* list, V value, std::int32_t start, std::int32_t bound, bool inclusive) {
    const auto trips = std::int64_t(bound) - start + inclusive;
    if (trips <= 0) return start;
    if (inclusive && bound == INT32_MAX) {
        // The induction variable wraps and the original condition remains true.
        for (;;) append(list, value);
    }
    const auto required = std::int64_t(list->size) + trips;
    if (required > INT32_MAX) fail("List is too large");
    if (required > list->capacity) {
        const auto doubled = std::min<std::int64_t>(std::int64_t(list->capacity) * 2, INT32_MAX);
        expand(list, static_cast<std::int32_t>(std::max(doubled, required)));
    }
    std::fill_n(list->data + list->size, trips, value);
    list->size = static_cast<std::int32_t>(required);
    return bound + inclusive;
}

bool listsEqual(const List* left, const List* right) { return equalElements(left, right, nullptr); }
bool listsLess(const List* left, const List* right) { return compareElements(left, right, nullptr) < 0; }

std::int32_t input() {
    std::fputs("<< ", stdout);
    std::fflush(stdout);
    int c;
    do { c = getc_unlocked(stdin); } while (whitespace(c));
    bool negative = c == '-';
    if (negative || c == '+') c = getc_unlocked(stdin);
    if (c < '0' || c > '9') fail("Expected integer input");
    std::uint64_t magnitude = 0;
    do {
        magnitude = magnitude * 10 + unsigned(c - '0');
        if (magnitude > std::uint64_t(INT32_MAX) + negative) fail("Integer input out of range");
        c = getc_unlocked(stdin);
    } while (c >= '0' && c <= '9');
    if (c != EOF && !whitespace(c)) std::ungetc(c, stdin);
    return static_cast<std::int32_t>(negative ? -std::int64_t(magnitude) : std::int64_t(magnitude));
}

void print(V value) {
    std::fputs(">> ", stdout);
    Printer printer;
    printer.write(value);
    printer.flush();
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

int run(void (*entry)()) {
    const char* timing = std::getenv("EPQ_TIMING");
    if (timing && std::strcmp(timing, "1") == 0) {
        auto start = std::chrono::steady_clock::now();
        entry();
        std::fflush(stdout);
        auto elapsed = std::chrono::steady_clock::now() - start;
        auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();
        std::fprintf(stderr, "Execution time: %lld nanoseconds\n", static_cast<long long>(ns));
    } else {
        entry();
    }
    return 0;
}

} // namespace rt
