#include <gtest/gtest.h>
#include "safeptr.hpp"
#include <thread>
#include <atomic>

using safeptr::SafeUniquePtr;
using safeptr::SafeWeakUniquePtr;

struct TestState {
    int value = 0;
};

TEST(SafePtrBasic, ConstructAndRead) {
    SafeUniquePtr<int> sp(new int(42));
    auto r = sp.read();
    EXPECT_EQ(*r, 42);
}

TEST(SafePtrBasic, WriteSetValueExplicit) {
    SafeUniquePtr<int> sp(new int(1));

    {
        auto w = sp.write();
        int prev = w.old();
        w.set_value(prev + 41);
    }

    auto r = sp.read();
    EXPECT_EQ(*r, 42);
}

TEST(SafePtrBasic, WriteViaOperator) {
    SafeUniquePtr<int> sp(new int(1));
    {
        auto w = sp.write();
        *w = w.old() + 41;  // lazy-new T(), assign value
    }
    auto r = sp.read();
    EXPECT_EQ(*r, 42);
}

TEST(SafePtrThreaded, MultiReaders) {
    SafeUniquePtr<int> sp(new int(5));

    auto job = [&]() {
        for (int i = 0; i < 2000; ++i) {
            auto r = sp.read();
            (void)*r;
        }
    };

    std::thread t1(job), t2(job);
    t1.join();
    t2.join();

    auto r = sp.read();
    EXPECT_EQ(*r, 5);
}

TEST(SafePtrThreaded, MixedReadsWrites) {
    SafeUniquePtr<int> sp(new int(0));
    std::atomic<bool> done{false};

    // Writer thread
    std::thread writer([&]() {
        for (int i = 0; i < 5000; ++i) {
            auto w = sp.write();
            int v = w.old();
            w.set_value(v + 1);
        }
        done.store(true);
    });

    // Reader thread
    std::thread reader([&]() {
        while (!done.load()) {
            auto r = sp.read();
            (void)*r;
        }
    });

    writer.join();
    reader.join();

    auto r = sp.read();
    EXPECT_GE(*r, 5000);
}

TEST(SafeWeakPtrBasic, WeakReadAlive) {
    SafeUniquePtr<TestState> sp(new TestState{10});
    SafeWeakUniquePtr<TestState> wk(sp);

    auto opt = wk.try_read();
    ASSERT_TRUE(opt.has_value());
    EXPECT_EQ(opt->operator->()->value, 10);  // opt-> is ReadGuard
}

TEST(SafeWeakPtrBasic, WeakReadFailsAfterDestroy) {
    SafeWeakUniquePtr<TestState> wk;

    {
        SafeUniquePtr<TestState> sp(new TestState{123});
        wk = SafeWeakUniquePtr<TestState>(sp);

        auto r1 = wk.try_read();
        ASSERT_TRUE(r1.has_value());
        EXPECT_EQ(r1->value, 123);
    }

    // nach Zerstörung des strong ptr: kein Objekt mehr vorhanden
    auto r2 = wk.try_read();
    EXPECT_FALSE(r2.has_value());
}
