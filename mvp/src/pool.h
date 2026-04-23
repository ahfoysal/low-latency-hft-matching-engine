#pragma once

// Object pool allocator with an intrusive free-list.
//
// - Preallocates a contiguous slab of `T` slots up front (single allocation,
//   no page faults during steady-state trading).
// - `acquire()` pops a free slot in O(1); `release()` pushes back in O(1).
// - Free slots are threaded through an intrusive singly-linked list that
//   reuses the slot storage itself, so there's zero per-node overhead beyond
//   one pointer while a slot is free.
//
// Not thread-safe by design — the matching thread is the sole owner. Cross-
// thread hand-off happens via the SPSC ring (see spsc_ring.h); nothing in
// the pool's hot path needs atomics.

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <type_traits>

namespace hft {

template <typename T>
class ObjectPool {
public:
    explicit ObjectPool(std::size_t capacity)
        : capacity_(capacity),
          storage_(static_cast<Slot*>(::operator new[](
              sizeof(Slot) * capacity, std::align_val_t{alignof(Slot)}))),
          free_head_(nullptr),
          in_use_(0) {
        // Thread every slot into the free-list. We walk backwards so the
        // first acquire() returns slot 0 (nicer for debugging / cache locality
        // on sequential allocations right after warmup).
        for (std::size_t i = capacity_; i-- > 0;) {
            Slot* s = storage_ + i;
            s->next = free_head_;
            free_head_ = s;
        }
    }

    ~ObjectPool() {
        // We only destruct slots the user has explicitly released by calling
        // release(); any still-live objects are the caller's responsibility.
        ::operator delete[](storage_, std::align_val_t{alignof(Slot)});
    }

    ObjectPool(const ObjectPool&)            = delete;
    ObjectPool& operator=(const ObjectPool&) = delete;

    // Acquire raw storage for a T. Caller placement-news into it. Returns
    // nullptr if the pool is exhausted (caller decides whether to fall back
    // to heap or drop the order — for M2 we size the pool generously).
    T* acquire() noexcept {
        if (free_head_ == nullptr) [[unlikely]] {
            return nullptr;
        }
        Slot* s    = free_head_;
        free_head_ = s->next;
        ++in_use_;
        return reinterpret_cast<T*>(&s->storage);
    }

    // Return a previously-acquired T back to the pool. Caller is responsible
    // for having already destructed the T (call p->~T() first if non-trivial).
    void release(T* p) noexcept {
        assert(p && "release(nullptr)");
        Slot* s    = reinterpret_cast<Slot*>(p);
        s->next    = free_head_;
        free_head_ = s;
        --in_use_;
    }

    std::size_t capacity() const noexcept { return capacity_;  }
    std::size_t in_use()   const noexcept { return in_use_;    }
    std::size_t free()     const noexcept { return capacity_ - in_use_; }

private:
    // A slot is exactly sizeof(T) / alignof(T); while free, we reinterpret
    // the first bytes as a `next` pointer (union). This keeps the pool slab
    // tight — no 8-byte tax per node while free.
    union Slot {
        Slot*                                                     next;
        std::aligned_storage_t<sizeof(T), alignof(T)>             storage;
        Slot() {}
        ~Slot() {}
    };

    std::size_t capacity_;
    Slot*       storage_;
    Slot*       free_head_;
    std::size_t in_use_;
};

} // namespace hft
