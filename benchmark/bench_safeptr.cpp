#include <iostream>
#include <thread>
#include <vector>
#include <chrono>
#include <iomanip>
#include "safeptr.hpp"

using safeptr::SafeUniquePtr;
using Clock = std::chrono::high_resolution_clock;

constexpr int OPS = 500'000;   // operations per thread

// -------------------------------------------------------------
//  Small helper to run a benchmark function across N threads
// -------------------------------------------------------------
template<typename Fn>
long long run_bench(int threads, Fn fn) {
    std::vector<std::thread> pool;
    pool.reserve(static_cast<std::size_t>(threads));

    auto start = Clock::now();
    for (int i = 0; i < threads; ++i)
        pool.emplace_back(fn);

    for (auto& t : pool)
        t.join();

    auto end = Clock::now();
    return std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
}

// -------------------------------------------------------------
//  Pretty header printer
// -------------------------------------------------------------
void print_section(const char* title) {
    std::cout << "\n============================================\n"
              << "  " << title << "\n"
              << "============================================\n";
}

// -------------------------------------------------------------
//  MAIN
// -------------------------------------------------------------
int main() {
    SafeUniquePtr<int> sp(new int(0));

    // ---------------------------------------------------------
    //  READ-ONLY BENCHMARK
    // ---------------------------------------------------------
    print_section("SafePtr – READ-ONLY BENCHMARK 1..16");

    for (int threads = 1; threads <= 16; ++threads) {
        long long ms = run_bench(threads, [&]() {
            for (int i = 0; i < OPS; ++i) {
                auto r = sp.read();
                int v = *r;
                (void)v;
            }
        });

        std::cout << "Threads " << std::setw(2) << threads
                  << " | read:   " << ms << " ms\n";
    }

    // Reset value before next phase
    sp.write().set_value(0);

    // ---------------------------------------------------------
    //  WRITE-ONLY BENCHMARK
    // ---------------------------------------------------------
    print_section("SafePtr – WRITE-ONLY BENCHMARK 1..16");

    for (int threads = 1; threads <= 16; ++threads) {
        long long ms = run_bench(threads, [&]() {
            for (int i = 0; i < OPS; ++i) {
                auto w = sp.write();

                // No need for try/catch — w.old() is ALWAYS valid.
                const int v = w.old();
                w.set_value(v + 1);
            }
        });

        std::cout << "Threads " << std::setw(2) << threads
                  << " | write:  " << ms << " ms\n";
    }

    // Reset again for mixed benchmark
    sp.write().set_value(0);

    // ---------------------------------------------------------
    //  MIXED BENCHMARK (90% reads / 10% writes)
    // ---------------------------------------------------------
    print_section("SafePtr – MIXED 90% READ / 10% WRITE BENCHMARK 1..16");

    for (int threads = 1; threads <= 16; ++threads) {
        long long ms = run_bench(threads, [&]() {
            for (int i = 0; i < OPS; ++i) {
                // Every 10th op → write
                if ((i % 10) == 0) {
                    auto w = sp.write();
                    const int v = w.old();
                    w.set_value(v + 1);
                }
                else {
                    auto r = sp.read();
                    int v = *r;
                    (void)v;
                }
            }
        });

        std::cout << "Threads " << std::setw(2) << threads
                  << " | mixed:  " << ms << " ms\n";
    }

    std::cout << "\nDone.\n";
    return 0;
}
