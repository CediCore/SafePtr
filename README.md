SafePtr
=======

SafePtr is a lightweight, high-performance, thread-safe smart pointer
designed for systems with many readers and occasional writers.
It offers:

- Lock-free, wait-free reads
- Mutex-serialized writes
- RCU-style snapshot access
- Deferred deletion when readers finish
- Array support (SafeArrayPtr<T[]>)
- Header-only implementation
- No external dependencies

SafePtr is an ideal replacement for std::unique_ptr when thread safety
and high read throughput are required.


Key Features
------------

- Lock-free, wait-free read guards
- Serialized writes (fast under low contention)
- Snapshot (RCU-like) reader view
- Deferred destruction when no guard uses the old pointer
- Custom deleter support
- Header-only (just include the .hpp)
- Designed for high read concurrency
- SafeArrayPtr<T[]> for array access


Ideal Use Cases
---------------

- Shared configuration/state
- Real-time loops, game engines
- AI agents, simulation systems
- Concurrent lookup tables
- High-frequency analytics or read pipelines


Example: Single Object
----------------------

    #include "safeptr.hpp"
    using safeptr::SafeUniquePtr;

    struct Config { int value = 0; };

    SafeUniquePtr<Config> cfg(new Config());

    // Reader
    {
        auto r = cfg.read();
        int v = r->value;
    }

    // Writer
    {
        auto w = cfg.write();
        int old = w.old();
        w.set_value(Config{ old + 1 });
    }


Example: Array Version
----------------------

    #include "safeptr.hpp"
    using safeptr::SafeArrayPtr;

    SafeArrayPtr<int> arr(new int[5]{1,2,3,4,5});

    // Reader
    {
        auto r = arr.read();
        int x = r[2];
    }

    // Writer
    {
        auto w = arr.write();
        int* next = new int[5];
        for (int i = 0; i < 5; ++i) next[i] = i * 10;
        w.set_array(next);
    }


Threading Model
---------------

Reads:
- Lock-free
- Wait-free
- Snapshot-based
- Never blocks
- Readers do not interact with writers or each other

Writes:
- Serialized via mutex
- Replace global pointer atomically
- Old pointer is deleted once last reader finishes


API Overview
------------

SafeUniquePtr<T>:

    ReadGuard  read();
    WriteGuard write();

    bool valid() const;
    explicit operator bool() const;

    T* get_unsafe();
    void reset(T*);

SafeArrayPtr<T[]>:

    auto r = arr.read();
    r[i];           // safe array indexing

    auto w = arr.write();
    w.set_array(new_data);


Installation
------------

SafePtr is header-only.

Just include:

    #include "safeptr.hpp"

Or add the include/ directory to your project include path.


Building (Makefile)
-------------------

The provided Makefile (GNU Make, w64devkit compatible) builds the project.

Build everything:

    make

Run benchmark:

    make bench
    ./build/bin/bench_safeptr

Clean:

    make clean


Benchmark Results (example)
---------------------------

On a modern 16-thread CPU (w64devkit/GCC):

READ ONLY:          ~5–20ms
WRITE ONLY:         ~20–600ms
90% READ MIXED:     ~6–200ms

Reads scale extremely well due to lock-free snapshot semantics.


License
-------

MIT License. See LICENSE for details.
