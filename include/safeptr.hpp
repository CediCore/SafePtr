#pragma once

#include <atomic>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <memory>   // std::default_delete

// ============================================================================
//  SafePtr — RCU-style thread-safe smart pointer with unique ownership
//
//  Features:
//    • Unique ownership (move-only)
//    • Lock-free reads (RCU snapshot)
//    • Mutex-serialized writes
//    • Rust-style borrow model (ReadGuard / WriteGuard)
//    • Custom deleter compatible with std::unique_ptr
//    • Full specialization for arrays T[]
//
//  Aliases:
//    safeptr::SafeUniquePtr<T>
//    safeptr::SafeArrayPtr<T>
//
//  This header is self-contained and header-only.
// ============================================================================

namespace safeptr {

// ============================================================================
//  SafePtr<T> — Single object variant
// ============================================================================

template<typename T, typename Deleter = std::default_delete<T>>
class SafePtr {
private:
    struct ControlBlock {
        std::atomic<T*> ptr{nullptr};       // current live version
        std::atomic<int> readers{0};        // number of active read borrows
        std::atomic<T*> retired{nullptr};   // old pointer waiting for reclamation
        std::mutex write_mtx;               // serialized write access
        Deleter deleter{};                  // deleter instance

        explicit ControlBlock(T* p) noexcept {
            ptr.store(p, std::memory_order_release);
        }

        ~ControlBlock() noexcept {
            // destroy primary ptr
            if (T* cur = ptr.load(std::memory_order_acquire)) {
                deleter(cur);
            }
            // destroy retired version
            if (T* r = retired.load(std::memory_order_acquire)) {
                if (r != ptr.load(std::memory_order_relaxed))
                    deleter(r);
            }
        }
    };

    ControlBlock* ctrl = nullptr;

public:
    class ReadGuard;
    class WriteGuard;

    // ---------------------------------------------------------------------
    //  Construct / Destroy
    // ---------------------------------------------------------------------
    constexpr SafePtr() noexcept = default;

    explicit SafePtr(T* p)
        : ctrl(p ? new ControlBlock(p) : nullptr)
    {}

    ~SafePtr() noexcept {
        delete ctrl;
    }

    SafePtr(const SafePtr&) = delete;
    SafePtr& operator=(const SafePtr&) = delete;

    // Move
    SafePtr(SafePtr&& other) noexcept
        : ctrl(std::exchange(other.ctrl, nullptr))
    {}

    SafePtr& operator=(SafePtr&& other) noexcept {
        if (this != &other) {
            delete ctrl;
            ctrl = std::exchange(other.ctrl, nullptr);
        }
        return *this;
    }

    // ---------------------------------------------------------------------
    //  State
    // ---------------------------------------------------------------------
    [[nodiscard]] bool valid() const noexcept {
        return ctrl &&
               ctrl->ptr.load(std::memory_order_acquire) != nullptr;
    }

    [[nodiscard]] T* get_unsafe() const noexcept {
        return ctrl ? ctrl->ptr.load(std::memory_order_acquire) : nullptr;
    }

    void reset(T* p = nullptr) {
        delete ctrl;
        ctrl = p ? new ControlBlock(p) : nullptr;
    }

    // ---------------------------------------------------------------------
    //  Borrowing API
    // ---------------------------------------------------------------------
    [[nodiscard]] ReadGuard read() const {
        if (!ctrl)
            throw std::logic_error("SafePtr::read() on null ControlBlock");
        return ReadGuard(ctrl);
    }

    [[nodiscard]] WriteGuard write() {
        if (!ctrl)
            throw std::logic_error("SafePtr::write() on null ControlBlock");
        return WriteGuard(ctrl);
    }

    // =====================================================================
    //  ReadGuard — snapshot borrow (lock-free)
    // =====================================================================
    class ReadGuard {
    public:
        using is_read_guard_tag = void;

    private:
        ControlBlock* ctrl = nullptr;
        T* snapshot        = nullptr;

    public:
        explicit ReadGuard(ControlBlock* c)
            : ctrl(c)
        {
            ctrl->readers.fetch_add(1, std::memory_order_acq_rel);
            snapshot = ctrl->ptr.load(std::memory_order_acquire);
        }

        ReadGuard(const ReadGuard&) = delete;
        ReadGuard& operator=(const ReadGuard&) = delete;

        ReadGuard(ReadGuard&& other) noexcept
            : ctrl(std::exchange(other.ctrl, nullptr)),
              snapshot(std::exchange(other.snapshot, nullptr))
        {}

        ReadGuard& operator=(ReadGuard&& other) noexcept {
            if (this != &other) {
                release();
                ctrl     = std::exchange(other.ctrl, nullptr);
                snapshot = std::exchange(other.snapshot, nullptr);
            }
            return *this;
        }

        ~ReadGuard() noexcept {
            release();
        }

        [[nodiscard]] const T& operator*()  const noexcept { return *snapshot; }
        [[nodiscard]] const T* operator->() const noexcept { return snapshot; }

    private:
        void release() noexcept {
            if (!ctrl) return;

            if (ctrl->readers.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                if (T* r = ctrl->retired.exchange(nullptr, std::memory_order_acq_rel))
                    ctrl->deleter(r);
            }

            ctrl     = nullptr;
            snapshot = nullptr;
        }
    };

    // =====================================================================
    //  WriteGuard — exclusive lock, full copy-on-write swap
    // =====================================================================
    class WriteGuard {
    public:
        using is_write_guard_tag = void;

    private:
        ControlBlock* ctrl = nullptr;
        std::unique_lock<std::mutex> lock;
        T* old_ptr = nullptr;
        T* local   = nullptr;   // newly allocated version

    public:
        explicit WriteGuard(ControlBlock* c)
            : ctrl(c),
              lock(c->write_mtx)
        {
            old_ptr = ctrl->ptr.load(std::memory_order_acquire);
        }

        WriteGuard(const WriteGuard&) = delete;
        WriteGuard& operator=(const WriteGuard&) = delete;

        WriteGuard(WriteGuard&& other) noexcept
            : ctrl(std::exchange(other.ctrl, nullptr)),
              lock(std::move(other.lock)),
              old_ptr(std::exchange(other.old_ptr, nullptr)),
              local(std::exchange(other.local, nullptr))
        {}

        WriteGuard& operator=(WriteGuard&& other) noexcept {
            if (this != &other) {
                commit_and_cleanup();
                ctrl    = std::exchange(other.ctrl, nullptr);
                lock    = std::move(other.lock);
                old_ptr = std::exchange(other.old_ptr, nullptr);
                local   = std::exchange(other.local, nullptr);
            }
            return *this;
        }

        ~WriteGuard() noexcept {
            commit_and_cleanup();
        }

        // -------------------------------------------------------------
        //  API
        // -------------------------------------------------------------
        [[nodiscard]] const T& old() const {
            if (!old_ptr)
                throw std::logic_error("WriteGuard::old(): no old value");
            return *old_ptr;
        }

        void set_value(const T& v) {
            if (!local) local = new T(v);
            else        *local = v;
        }

        [[nodiscard]] T& emplace_default() {
            if (!local) local = new T();
            return *local;
        }

        [[nodiscard]] T& operator*() {
            if (!local) local = new T();
            return *local;
        }

        [[nodiscard]] T* operator->() {
            if (!local) local = new T();
            return local;
        }

    private:
        void commit_and_cleanup() noexcept {
            if (!ctrl || !local)
                return;

            T* new_ptr = local;
            local = nullptr;

            T* old_global = ctrl->ptr.exchange(new_ptr, std::memory_order_acq_rel);

            if (old_global) {
                if (ctrl->readers.load(std::memory_order_acquire) == 0) {
                    ctrl->deleter(old_global);
                } else {
                    T* expected = nullptr;
                    if (!ctrl->retired.compare_exchange_strong(
                            expected, old_global,
                            std::memory_order_acq_rel,
                            std::memory_order_acquire))
                    {
                        ctrl->deleter(old_global);
                    }
                }
            }
        }
    };
};


// ============================================================================
//  SafePtr<T[]> — Array specialization (RCU array pointer)
//  (Drop-in replacement for std::unique_ptr<T[]>)
// ============================================================================

template<typename T, typename Deleter>
class SafePtr<T[], Deleter> {
private:
    struct ControlBlock {
        std::atomic<T*> ptr{nullptr};
        std::atomic<int> readers{0};
        std::atomic<T*> retired{nullptr};
        std::mutex write_mtx;
        Deleter deleter{};

        explicit ControlBlock(T* p) noexcept {
            ptr.store(p, std::memory_order_release);
        }

        ~ControlBlock() noexcept {
            if (T* cur = ptr.load(std::memory_order_acquire))
                deleter(cur);
            if (T* r = retired.load(std::memory_order_acquire))
                if (r != ptr.load(std::memory_order_relaxed))
                    deleter(r);
        }
    };

    ControlBlock* ctrl = nullptr;

public:
    class ReadGuard;
    class WriteGuard;

    // Construction
    constexpr SafePtr() noexcept = default;

    explicit SafePtr(T* p)
        : ctrl(p ? new ControlBlock(p) : nullptr)
    {}

    ~SafePtr() noexcept {
        delete ctrl;
    }

    SafePtr(const SafePtr&) = delete;
    SafePtr& operator=(const SafePtr&) = delete;

    SafePtr(SafePtr&& other) noexcept
        : ctrl(std::exchange(other.ctrl, nullptr))
    {}

    SafePtr& operator=(SafePtr&& other) noexcept {
        if (this != &other) {
            delete ctrl;
            ctrl = std::exchange(other.ctrl, nullptr);
        }
        return *this;
    }

    // State
    [[nodiscard]] bool valid() const noexcept {
        return ctrl &&
               ctrl->ptr.load(std::memory_order_acquire) != nullptr;
    }

    [[nodiscard]] T* get_unsafe() const noexcept {
        return ctrl ? ctrl->ptr.load(std::memory_order_acquire) : nullptr;
    }

    void reset(T* p = nullptr) {
        delete ctrl;
        ctrl = p ? new ControlBlock(p) : nullptr;
    }

    // Reads / writes
    [[nodiscard]] ReadGuard read() const {
        if (!ctrl)
            throw std::logic_error("SafePtr<T[]>::read(): null block");
        return ReadGuard(ctrl);
    }

    [[nodiscard]] WriteGuard write() {
        if (!ctrl)
            throw std::logic_error("SafePtr<T[]>::write(): null block");
        return WriteGuard(ctrl);
    }

    // =====================================================================
    //  ReadGuard (arrays) — supports operator[]
    // =====================================================================
    class ReadGuard {
    public:
        using is_read_guard_tag = void;

    private:
        ControlBlock* ctrl = nullptr;
        T* snapshot        = nullptr;

    public:
        explicit ReadGuard(ControlBlock* c)
            : ctrl(c)
        {
            ctrl->readers.fetch_add(1, std::memory_order_acq_rel);
            snapshot = ctrl->ptr.load(std::memory_order_acquire);
        }

        ReadGuard(ReadGuard&& other) noexcept
            : ctrl(std::exchange(other.ctrl, nullptr)),
              snapshot(std::exchange(other.snapshot, nullptr))
        {}

        ~ReadGuard() noexcept {
            release();
        }

        ReadGuard(const ReadGuard&) = delete;
        ReadGuard& operator=(const ReadGuard&) = delete;

        [[nodiscard]] const T& operator[](std::size_t i) const noexcept {
            return snapshot[i];
        }

        [[nodiscard]] const T* data() const noexcept {
            return snapshot;
        }

    private:
        void release() noexcept {
            if (!ctrl) return;

            if (ctrl->readers.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                if (T* r = ctrl->retired.exchange(nullptr, std::memory_order_acq_rel))
                    ctrl->deleter(r);
            }

            ctrl     = nullptr;
            snapshot = nullptr;
        }
    };

    // =====================================================================
    //  WriteGuard (arrays)
    // =====================================================================
    class WriteGuard {
    public:
        using is_write_guard_tag = void;

    private:
        ControlBlock* ctrl = nullptr;
        std::unique_lock<std::mutex> lock;
        T* old_ptr = nullptr;
        T* local   = nullptr; // new array

    public:
        explicit WriteGuard(ControlBlock* c)
            : ctrl(c),
              lock(c->write_mtx)
        {
            old_ptr = ctrl->ptr.load(std::memory_order_acquire);
        }

        WriteGuard(WriteGuard&& other) noexcept
            : ctrl(std::exchange(other.ctrl, nullptr)),
              lock(std::move(other.lock)),
              old_ptr(std::exchange(other.old_ptr, nullptr)),
              local(std::exchange(other.local, nullptr))
        {}

        ~WriteGuard() noexcept {
            commit_and_cleanup();
        }

        WriteGuard(const WriteGuard&) = delete;
        WriteGuard& operator=(const WriteGuard&) = delete;

        [[nodiscard]] const T* old_data() const noexcept {
            return old_ptr;
        }

        /// Set freshly allocated array (caller must use new T[n])
        void set_array(T* p) noexcept {
            local = p;
        }

    private:
        void commit_and_cleanup() noexcept {
            if (!ctrl || !local)
                return;

            T* new_ptr = local;
            local = nullptr;

            T* old_global = ctrl->ptr.exchange(new_ptr, std::memory_order_acq_rel);

            if (old_global) {
                if (ctrl->readers.load(std::memory_order_acquire) == 0) {
                    ctrl->deleter(old_global);
                } else {
                    T* expected = nullptr;
                    if (!ctrl->retired.compare_exchange_strong(
                            expected, old_global,
                            std::memory_order_acq_rel,
                            std::memory_order_acquire))
                    {
                        ctrl->deleter(old_global);
                    }
                }
            }
        }
    };
};


// ============================================================================
//  Public Aliases
// ============================================================================

template<typename T, typename Deleter = std::default_delete<T>>
using SafeUniquePtr = SafePtr<T, Deleter>;

template<typename T, typename Deleter = std::default_delete<T[]>>
using SafeArrayPtr = SafePtr<T[], Deleter>;

} // namespace safeptr
