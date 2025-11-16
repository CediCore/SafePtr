#include <iostream>
#include <thread>
#include <vector>
#include <chrono>
#include "safeptr.hpp"

using safeptr::SafeUniquePtr;
using Clock = std::chrono::high_resolution_clock;

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

int main() {
    constexpr int OPS = 500'000;
    SafeUniquePtr<int> sp(new int(0));

    std::cout << "============================================\n";
    std::cout << "  SafePtr – READ-ONLY BENCHMARK 1..16\n";
    std::cout << "============================================\n";
    for (int threads = 1; threads <= 16; ++threads) {
        long long t_sp = run_bench(threads, [&]() {
            for (int i = 0; i < OPS; ++i) {
                auto r = sp.read();
                int v = *r;
                (void)v;
            }
        });
        std::cout << "Threads " << threads
                  << " | SafePtr (read): " << t_sp << " ms\n";
    }

    std::cout << "\n============================================\n";
    std::cout << "  SafePtr – WRITE-ONLY BENCHMARK 1..16\n";
    std::cout << "============================================\n";
    for (int threads = 1; threads <= 16; ++threads) {
        long long t_sp = run_bench(threads, [&]() {
            for (int i = 0; i < OPS; ++i) {
                auto w = sp.write();

                int v = 0;
                try {
                    v = w.old();
                } catch (...) {
                    v = 0;
                }

                w.set_value(v + 1);
            }
        });
        std::cout << "Threads " << threads
                  << " | SafePtr (write): " << t_sp << " ms\n";
    }

    std::cout << "\n============================================\n";
    std::cout << "  SafePtr – MIXED 90% READ BENCHMARK 1..16\n";
    std::cout << "============================================\n";
    for (int threads = 1; threads <= 16; ++threads) {
        long long t_sp = run_bench(threads, [&]() {
            for (int i = 0; i < OPS; ++i) {
                if (i % 10 == 0) {
                    auto w = sp.write();
                    int v = 0;
                    try {
                        v = w.old();
                    } catch (...) {}
                    w.set_value(v + 1);
                } else {
                    auto r = sp.read();
                    int v = *r;
                    (void)v;
                }
            }
        });
        std::cout << "Threads " << threads
                  << " | SafePtr (mixed): " << t_sp << " ms\n";
    }

    return 0;
}
