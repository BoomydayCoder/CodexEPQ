#include "epq_runtime.hpp"
#include "list_buffers.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <dispatch/dispatch.h>
#include <unistd.h>

namespace rt {

namespace {

void writeStdout(const char* data, std::size_t bytes) {
    while (bytes) {
        const ssize_t written = ::write(STDOUT_FILENO, data, bytes);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) {
            std::perror("Cannot write EPQ output");
            std::_Exit(1);
        }
        data += written;
        bytes -= static_cast<std::size_t>(written);
    }
}

// One EPQ thread submits prompts. After a burst of buffered input, a serial
// output queue can write several at once and keep writing while EPQ computes.
// Flushing before every kernel read bounds the backlog to one input buffer and
// ensures a client waiting for a prompt can always supply the next integer.
class Prompts {
    std::atomic<unsigned> pending{0};
    dispatch_queue_t queue = nullptr;
    unsigned readsInBuffer = 0;
    bool scheduled = false;

    static void sendPending(void* context) {
        auto& self = *static_cast<Prompts*>(context);
        unsigned count = self.pending.exchange(0, std::memory_order_relaxed);
        static constexpr auto block = [] {
            std::array<char, 3072> bytes{};
            for (std::size_t i = 0; i < bytes.size(); i += 3) {
                bytes[i] = '<';
                bytes[i + 1] = '<';
                bytes[i + 2] = ' ';
            }
            return bytes;
        }();
        while (count) {
            unsigned batch = std::min(count, static_cast<unsigned>(block.size() / 3));
            writeStdout(block.data(), batch * 3);
            count -= batch;
        }
    }

public:
    void submit(bool inputAvailable) {
        if (!inputAvailable) {
            flush();
            writeStdout("<< ", 3);
            return;
        }
        if (!queue && ++readsInBuffer >= 16)
            queue = dispatch_queue_create("epq.prompts", DISPATCH_QUEUE_SERIAL);
        if (!queue) {
            writeStdout("<< ", 3);
            return;
        }
        if (pending.fetch_add(1, std::memory_order_relaxed) == 0)
            dispatch_async_f(queue, this, sendPending);
        scheduled = true;
    }

    void flush() {
        if (scheduled) {
            dispatch_sync_f(queue, nullptr, [](void*) {});
            scheduled = false;
        }
    }

    void beforeRead() {
        flush();
        readsInBuffer = 0;
    }

    ~Prompts() {
        flush();
        if (queue) dispatch_release(queue);
    }
};

Prompts prompts;
} // namespace

[[noreturn]] void fail(const char* message) {
    prompts.flush();
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
    ListBuffers buffers;

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
                if (list.data != list.small) buffers.releaseAtExit(list.data, list.capacity);
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

class InputBuffer {
    unsigned char data[16384];
    std::size_t next = 0;
    std::size_t end = 0;

public:
    bool available() const { return next < end; }

    int get() {
        if (!available()) {
            prompts.beforeRead();
            ssize_t count;
            do { count = ::read(STDIN_FILENO, data, sizeof(data)); }
            while (count < 0 && errno == EINTR);
            if (count < 0) fail("Cannot read EPQ input");
            if (!count) return EOF;
            next = 0;
            end = static_cast<std::size_t>(count);
        }
        return data[next++];
    }

    void putBack() { --next; }
};

InputBuffer inputBuffer;

} // namespace

List* makeList(std::initializer_list<V> items) {
    return makeList(items.begin(), items.size());
}

List* makeList(const V* items, std::size_t count) {
    if (count > INT32_MAX) fail("List is too large");
    List* list = pool.allocate();
    list->size = static_cast<std::int32_t>(count);
    list->capacity = std::max(list->size, 2);
    list->data = list->small;
    if (list->capacity > 2) list->data = pool.buffers.acquire(list->capacity);
    if (count) std::memcpy(list->data, items, count * sizeof(V));
    return list;
}

void resizeBacking(List* list, std::int32_t capacity, std::size_t bytesToCopy) {
    V* data = pool.buffers.acquire(capacity);
    std::memcpy(data, list->data, bytesToCopy);
    if (list->data != list->small) pool.buffers.release(list->data, list->capacity);
    list->data = data;
    list->capacity = capacity;
}

static void expand(List* list, std::int32_t capacity) {
    resizeBacking(list, capacity, std::size_t(list->size) * sizeof(V));
}

void grow(List* list) {
    if (list->capacity == INT32_MAX) fail("List is too large");
    std::int32_t capacity = list->capacity > INT32_MAX / 2 ? INT32_MAX : list->capacity * 2;
    expand(list, capacity);
}

static void reserveAppendCapacity(List* list, std::int32_t required) {
    if (required <= list->capacity) return;
    const auto doubled = std::min<std::int64_t>(std::int64_t(list->capacity) * 2, INT32_MAX);
    expand(list, std::max(required, static_cast<std::int32_t>(doubled)));
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
    reserveAppendCapacity(list, static_cast<std::int32_t>(required));
    std::fill_n(list->data + list->size, trips, value);
    list->size = static_cast<std::int32_t>(required);
    return bound + inclusive;
}

std::int32_t copyLoop(List* dst, List* src, std::int32_t start, std::int32_t bound,
                      bool usesSourceLength) {
    const auto end = usesSourceLength ? src->size : bound;
    if (start >= end) return start;
    const auto count = std::int64_t(end) - start;
    const auto required = std::int64_t(dst->size) + count;
    if (start < 0 || end > src->size || (usesSourceLength && dst == src) || required > INT32_MAX) {
        // Self-appends can create the next readable element and can extend the
        // loop's bound. Scalar execution preserves that behavior and errors.
        while (start < (usesSourceLength ? src->size : bound)) {
            append(dst, get(src, start));
            ++start;
        }
        return start;
    }
    reserveAppendCapacity(dst, static_cast<std::int32_t>(required));
    // Even if dst == src, the new tail starts after all source elements.
    std::memcpy(dst->data + dst->size, src->data + start, std::size_t(count) * sizeof(V));
    dst->size = static_cast<std::int32_t>(required);
    return end;
}

void applyAffine(std::uint32_t* state, const std::uint32_t* matrix, unsigned size, std::uint32_t count) {
    constexpr unsigned cells = maxAffineDimension * maxAffineDimension;
    if (size == 0 || size > maxAffineDimension) fail("Invalid affine dimension");
    std::uint32_t power[cells];
    std::copy_n(matrix, size * size, power);
    while (count) {
        if (count & 1u) {
            std::uint32_t next[maxAffineDimension]{};
            for (unsigned i = 0; i < size; ++i)
                for (unsigned j = 0; j < size; ++j)
                    next[i] += power[i * size + j] * state[j];
            std::copy_n(next, size, state);
        }
        count >>= 1;
        if (!count) break;
        std::uint32_t next[cells]{};
        for (unsigned i = 0; i < size; ++i)
            for (unsigned k = 0; k < size; ++k)
                for (unsigned j = 0; j < size; ++j)
                    next[i * size + j] += power[i * size + k] * power[k * size + j];
        std::copy_n(next, size * size, power);
    }
}

bool listsEqual(const List* left, const List* right) { return equalElements(left, right, nullptr); }
bool listsLess(const List* left, const List* right) { return compareElements(left, right, nullptr) < 0; }

std::int32_t input() {
    prompts.submit(inputBuffer.available());
    int c;
    do { c = inputBuffer.get(); } while (whitespace(c));
    bool negative = c == '-';
    if (negative || c == '+') c = inputBuffer.get();
    if (c < '0' || c > '9') fail("Expected integer input");
    std::uint64_t magnitude = 0;
    do {
        magnitude = magnitude * 10 + unsigned(c - '0');
        if (magnitude > std::uint64_t(INT32_MAX) + negative) fail("Integer input out of range");
        c = inputBuffer.get();
    } while (c >= '0' && c <= '9');
    if (c != EOF && !whitespace(c)) inputBuffer.putBack();
    return static_cast<std::int32_t>(negative ? -std::int64_t(magnitude) : std::int64_t(magnitude));
}

void print(V value) {
    prompts.flush();
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
        prompts.flush();
        std::fflush(stdout);
        auto elapsed = std::chrono::steady_clock::now() - start;
        auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();
        std::fprintf(stderr, "Execution time: %lld nanoseconds\n", static_cast<long long>(ns));
    } else {
        entry();
        prompts.flush();
    }
    return 0;
}

} // namespace rt
