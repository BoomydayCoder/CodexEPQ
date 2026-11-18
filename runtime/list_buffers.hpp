#pragma once

#include "epq_runtime.hpp"

#include <cstring>

namespace rt {

// EPQ keeps list headers alive for a run, but a growing list discards its old
// element array. Reuse those short arrays by exact capacity; their contents
// are dead as soon as the header points to its replacement.
class ListBuffers {
    static constexpr unsigned maxPooled = 512;
    static constexpr unsigned chunkWords = (64 * 1024 - sizeof(void*)) / sizeof(V);
    struct Chunk {
        Chunk* previous;
        V words[chunkWords];
    };
    Chunk* last = nullptr;
    unsigned used = chunkWords;
    V* recycled[maxPooled + 1]{};

public:
    V* acquire(unsigned size) {
        if (size > maxPooled) return new V[size];
        if (V* block = recycled[size]) {
            std::memcpy(&recycled[size], static_cast<const void*>(block), sizeof(block));
            return block;
        }
        if (size > chunkWords - used) {
            auto* next = new Chunk;
            next->previous = last;
            last = next;
            used = 0;
        }
        V* result = last->words + used;
        used += size;
        return result;
    }

    void release(V* block, unsigned size) {
        if (size > maxPooled) {
            delete[] block;
        } else {
            // The first unused word holds the next link without aliasing V as
            // a pointer object. The next user writes its live elements afresh.
            static_assert(sizeof(block) <= sizeof(V));
            std::memcpy(static_cast<void*>(block), &recycled[size], sizeof(block));
            recycled[size] = block;
        }
    }

    void releaseAtExit(V* block, unsigned size) {
        if (size > maxPooled) delete[] block;
    }

    ~ListBuffers() {
        while (last) {
            Chunk* previous = last->previous;
            delete last;
            last = previous;
        }
    }
};

} // namespace rt
