SafePtr
=======

A high-performance, thread-safe smart pointer designed for shared state that is read frequently
and written occasionally. SafePtr provides a lock-free, RCU-style read path and a serialized,
copy-on-write update mechanism for writers.

SafePtr is intended as a drop-in replacement for std::unique_ptr in multithreaded systems where
multiple readers and occasional writers must safely share access to a single object without
introducing heavy locking overhead.

Key Features
------------

- Lock-free read guards (no mutex, no blocking)
- Serialized write guards (std::mutex) with copy-on-write updates
- SafeWeakPtr for non-owning observers
- Unique ownership semantics (similar to std::unique_ptr)
- Supports custom deleters
- Non-blocking try_read() and try_write()
- ABA-safe pointer replacement with deferred deletion
- Header-only library

SafePtr is ideal for:

- Shared configuration/state objects
- High-frequency readers with low write contention
- Game engines, real-time systems, and simulation loops
- Multithreaded AI state, analytics caches, or message routing tables

Example Usage
-------------

#include <safeptr.hpp>

using safeptr::SafeUniquePtr;

struct Config {
    int value = 0;
};

SafeUniquePtr<Config> config(new Config());

// Reader thread
{
    auto r = config.read();
    int v = r->value;
}

// Writer thread
{
    auto w = config.write();
    auto old = w.old();
    w.set_value(Config{ old.value + 1 });
}

Threading Model
---------------

Reads:
- Lock-free
- Snapshot-based
- Never block, never wait for writers

Writes:
- Serialized with a mutex
- Replace global pointer atomically
- Old pointer is deleted when all readers finish

API Overview
------------

ReadGuard read();
WriteGuard write();
std::optional<ReadGuard> try_read();
std::optional<WriteGuard> try_write();
T* get();                // Unsafe access
void reset(T*);          // Replace object
operator bool() const;

Weak Pointer:

SafeWeakPtr<T> weak(sp);
weak.try_read();
weak.try_write();

Installation
------------

SafePtr is header-only.

Copy include/safeptr.hpp into your project and include it:

#include "safeptr.hpp"

Or use CMake:

add_subdirectory(SafePtr)
target_link_libraries(MyApp PRIVATE safeptr)

Building Tests
--------------

mkdir build
cd build
cmake ..
cmake --build .
ctest

Benchmark
---------

You can build and run the benchmark:

cmake --build . --target bench_safeptr
./bench_safeptr

License
-------

MIT License. See LICENSE file for details.
