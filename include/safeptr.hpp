#pragma once

#include <atomic>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <memory>    // std::default_delete
#include <optional>  // std::optional
#include <new>       // std::nothrow_t

/// \file safeptr.hpp
/// \brief SafePtr<T,Deleter> – a thread-safe smart pointer with borrow guards.
///
/// Features:
///   - Unique ownership (no copy, only move)
///   - Lock-free reads (RCU-style snapshots)
///   - Serialized writes via std::mutex
///   - Runtime borrow model (read/write guards; Rust-like semantics)
///   - Custom deleter (compatible with std::unique_ptr<T,Deleter>)
///   - Non-blocking try_read() / try_write()
///   - SafeWeakPtr<T,Deleter> as a non-owning observer handle
///
/// SafePtr is intended for shared state that is:
///   - read frequently,
///   - written occasionally,
/// while remaining thread-safe and free of data races / UB.

namespace safeptr {

    // Forward declaration
    template<typename T, typename Deleter>
    class SafeWeakPtr;

    // ============================================================
    //  SafePtr<T, Deleter> — single-object variant
    // ============================================================

    template<typename T, typename Deleter = std::default_delete<T>>
    class SafePtr {
    private:
        struct ControlBlock {
            std::atomic<T*>  ptr{nullptr};     ///< current version
            std::atomic<int> readers{0};       ///< active readers
            std::atomic<T*>  retired{nullptr}; ///< old version pending deletion
            std::mutex       write_mtx;        ///< serializes writers
            Deleter          deleter{};        ///< deleter instance

            // NEW: reference counting for strong / weak handles
            std::atomic<int> strong{1};        ///< number of owning SafePtr
            std::atomic<int> weak{0};          ///< number of SafeWeakPtr

            explicit ControlBlock(T* p) noexcept {
                ptr.store(p, std::memory_order_release);
            }

            ~ControlBlock() noexcept {
                T* cur = ptr.load(std::memory_order_acquire);
                if (cur) {
                    deleter(cur);
                }
                T* r = retired.load(std::memory_order_acquire);
                if (r && r != cur) {
                    deleter(r);
                }
            }
        };

        ControlBlock* ctrl = nullptr;

        friend class SafeWeakPtr<T, Deleter>;

        // Helper: release our strong reference (used by dtor, reset, move-assign)
        void release_strong() noexcept {
            if (!ctrl) return;

            if (ctrl->strong.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                // last strong owner -> destroy managed object
                T* obj = ctrl->ptr.exchange(nullptr, std::memory_order_acq_rel);
                if (obj) {
                    ctrl->deleter(obj);
                }

                // if no weak observers -> delete control block
                if (ctrl->weak.load(std::memory_order_acquire) == 0) {
                    delete ctrl;
                }
            }

            ctrl = nullptr;
        }

    public:
        class ReadGuard;
        class WriteGuard;

        /// \brief Default constructor – an empty SafePtr.
        constexpr SafePtr() noexcept = default;

        /// \brief Constructor from raw pointer (takes ownership).
        explicit SafePtr(T* p)
            : ctrl(p ? new ControlBlock(p) : nullptr)
        {}

        /// \brief Destructor – expects no guards to be alive.
        ~SafePtr() noexcept {
            release_strong();
        }

        /// \brief No copy – unique ownership.
        SafePtr(const SafePtr&) = delete;
        SafePtr& operator=(const SafePtr&) = delete;

        /// \brief Move constructor.
        SafePtr(SafePtr&& other) noexcept
            : ctrl(std::exchange(other.ctrl, nullptr))
        {
            // strong count remains the same (unique ownership moved)
        }

        /// \brief Move assignment.
        SafePtr& operator=(SafePtr&& other) noexcept {
            if (this != &other) {
                // drop our current strong ownership (if any)
                release_strong();
                // take over other's control block
                ctrl = std::exchange(other.ctrl, nullptr);
            }
            return *this;
        }

        /// \brief Returns true if an object is currently managed.
        [[nodiscard]] bool valid() const noexcept {
            return ctrl &&
                   ctrl->ptr.load(std::memory_order_acquire) != nullptr;
        }

        /// \brief STL-style: get() – obtain the raw pointer (UNSAFE).
        ///
        /// \warning Not thread-safe without acquiring a read guard.
        [[nodiscard]] T* get() const noexcept {
            return ctrl
                ? ctrl->ptr.load(std::memory_order_acquire)
                : nullptr;
        }

        /// \brief Alias for get(), explicitly marked UNSAFE.
        [[nodiscard]] T* get_unsafe() const noexcept {
            return get();
        }

        /// \brief Replace the managed object; expects no active guards.
        ///
        /// Releases current strong ownership and (if non-null) creates a new ControlBlock.
        void reset(T* p = nullptr) {
            // release current strong ref (like destructor)
            release_strong();

            if (p) {
                ctrl = new ControlBlock(p);
            }
        }

        /// \brief True if an object is managed (same semantics as unique_ptr).
        explicit operator bool() const noexcept {
            return valid();
        }

        // -------------------------------------------------
        // Borrow API (blocking)
        // -------------------------------------------------

        /// \brief Acquire a read borrow (lock-free reader).
        ///
        /// Never blocks on locks, only uses atomics.
        /// \throws std::logic_error if no ControlBlock exists.
        [[nodiscard]] ReadGuard read() const {
            if (!ctrl) {
                throw std::logic_error("SafePtr::read() on null ControlBlock");
            }
            return ReadGuard(ctrl);
        }

        /// \brief Acquire a write borrow (blocking writer).
        ///
        /// Serializes all writers via std::mutex.
        /// \throws std::logic_error if no ControlBlock exists.
        [[nodiscard]] WriteGuard write() {
            if (!ctrl) {
                throw std::logic_error("SafePtr::write() on null ControlBlock");
            }
            return WriteGuard(ctrl);
        }

        // -------------------------------------------------
        // Borrow API (non-blocking / try_*)
        // -------------------------------------------------

        /// \brief Non-blocking attempt to acquire a read borrow.
        ///
        /// Because reads are lock-free, this fails only if no ControlBlock exists.
        [[nodiscard]] std::optional<ReadGuard> try_read() const noexcept {
            if (!ctrl) {
                return std::nullopt;
            }
            return std::optional<ReadGuard>(ReadGuard(ctrl, std::nothrow_t{}));
        }

        /// \brief Non-blocking attempt to acquire a write borrow.
        ///
        /// Uses std::try_to_lock.  
        /// - Success → returns WriteGuard  
        /// - Failure → returns std::nullopt  
        [[nodiscard]] std::optional<WriteGuard> try_write() noexcept {
            if (!ctrl) {
                return std::nullopt;
            }

            std::unique_lock<std::mutex> lk(ctrl->write_mtx, std::try_to_lock);
            if (!lk.owns_lock()) {
                return std::nullopt;
            }

            return std::optional<WriteGuard>(WriteGuard(ctrl, std::move(lk), true));
        }

        // ============================================================
        //   ReadGuard
        // ============================================================
        class ReadGuard {
        public:
            using is_read_guard_tag = void;

        private:
            ControlBlock* ctrl = nullptr;
            T*            snapshot = nullptr;

            friend class SafePtr;

            /// \brief Standard constructor (throwing).
            explicit ReadGuard(ControlBlock* c)
                : ctrl(c)
            {
                ctrl->readers.fetch_add(1, std::memory_order_acq_rel);
                snapshot = ctrl->ptr.load(std::memory_order_acquire);
            }

            /// \brief Non-throwing constructor for try_read().
            ReadGuard(ControlBlock* c, std::nothrow_t) noexcept
                : ctrl(c)
            {
                ctrl->readers.fetch_add(1, std::memory_order_acq_rel);
                snapshot = ctrl->ptr.load(std::memory_order_acquire);
            }

        public:
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

            [[nodiscard]] const T& operator*() const noexcept { return *snapshot; }
            [[nodiscard]] const T* operator->() const noexcept { return snapshot; }

        private:
            void release() noexcept {
                if (!ctrl) return;

                int prev = ctrl->readers.fetch_sub(1, std::memory_order_acq_rel);
                if (prev == 1) {
                    // last reader → may delete retired pointer
                    T* r = ctrl->retired.exchange(nullptr, std::memory_order_acq_rel);
                    if (r) {
                        ctrl->deleter(r);
                    }
                }

                ctrl     = nullptr;
                snapshot = nullptr;
            }
        };

        // ============================================================
        //   WriteGuard
        // ============================================================
        class WriteGuard {
        public:
            using is_write_guard_tag = void;

        private:
            ControlBlock*            ctrl   = nullptr;
            std::unique_lock<std::mutex> lock;
            T*                       old_ptr = nullptr; ///< snapshot of old version
            T*                       local   = nullptr; ///< new version (if set)

            friend class SafePtr;
            friend class SafeWeakPtr<T, Deleter>;

            /// \brief Blocking constructor (write()).
            explicit WriteGuard(ControlBlock* c)
                : ctrl(c),
                  lock(c->write_mtx)
            {
                old_ptr = ctrl->ptr.load(std::memory_order_acquire);
            }

            /// \brief Adopting constructor for try_write() / SafeWeakPtr.
            WriteGuard(ControlBlock* c,
                       std::unique_lock<std::mutex>&& lk,
                       bool /*adopt_tag*/) noexcept
                : ctrl(c),
                  lock(std::move(lk))
            {
                old_ptr = ctrl->ptr.load(std::memory_order_acquire);
            }

        public:
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

            /// \brief Access the old value (read-only).
            [[nodiscard]] const T& old() const {
                if (!old_ptr) {
                    throw std::logic_error("WriteGuard::old(): no old value");
                }
                return *old_ptr;
            }

            /// \brief Set new value (copy into local buffer).
            void set_value(const T& v) {
                if (!local) {
                    local = new T(v);
                } else {
                    *local = v;
                }
            }

            /// \brief Default-construct a new value and return a reference.
            [[nodiscard]] T& emplace_default() {
                if (!local) {
                    local = new T();
                }
                return *local;
            }

            /// \brief Write access (lazy default).
            [[nodiscard]] T& operator*() {
                if (!local) {
                    local = new T();
                }
                return *local;
            }

            [[nodiscard]] T* operator->() {
                if (!local) {
                    local = new T();
                }
                return local;
            }

            [[nodiscard]] const T& operator*() const {
                if (!local) {
                    throw std::logic_error("WriteGuard::operator*() const: local not set");
                }
                return *local;
            }

            [[nodiscard]] const T* operator->() const {
                if (!local) {
                    throw std::logic_error("WriteGuard::operator->() const: local not set");
                }
                return local;
            }

        private:
            void commit_and_cleanup() noexcept {
                if (!ctrl) return;
                if (!local) return; // nothing to commit

                T* new_ptr = local;
                local      = nullptr;

                // atomically replace global pointer
                T* old_global = ctrl->ptr.exchange(new_ptr, std::memory_order_acq_rel);

                if (old_global) {
                    int r = ctrl->readers.load(std::memory_order_acquire);
                    if (r == 0) {
                        ctrl->deleter(old_global);
                    } else {
                        T* expected = nullptr;
                        if (!ctrl->retired.compare_exchange_strong(
                                expected, old_global,
                                std::memory_order_acq_rel,
                                std::memory_order_acquire))
                        {
                            // retired slot already taken → delete directly
                            ctrl->deleter(old_global);
                        }
                    }
                }
            }
        };
    };

    // ============================================================
    //  SafeWeakPtr<T,Deleter> – non-owning handle
    // ============================================================

    /// \brief Non-owning observer handle to a SafePtr control block.
    ///
    /// Lifetime is managed via atomic strong/weak reference counters
    /// stored in the shared ControlBlock:
    ///   - The managed object is destroyed when strong == 0.
    ///   - The ControlBlock itself is destroyed when strong == 0 && weak == 0.
    template<typename T, typename Deleter = std::default_delete<T>>
    class SafeWeakPtr {
    private:
        using Strong       = SafePtr<T, Deleter>;
        using ControlBlock = typename Strong::ControlBlock;

        ControlBlock* ctrl = nullptr;

        void release_weak() noexcept {
            if (!ctrl) return;

            if (ctrl->weak.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                // last weak; if no strong -> delete control block
                if (ctrl->strong.load(std::memory_order_acquire) == 0) {
                    delete ctrl;
                }
            }
            ctrl = nullptr;
        }

    public:
        /// \brief Default constructor – empty weak handle.
        constexpr SafeWeakPtr() noexcept = default;

        /// \brief Construct from a strong SafePtr.
        SafeWeakPtr(const Strong& sp) noexcept
            : ctrl(sp.ctrl)
        {
            if (ctrl) {
                ctrl->weak.fetch_add(1, std::memory_order_acq_rel);
            }
        }

        /// \brief Copy constructor.
        SafeWeakPtr(const SafeWeakPtr& other) noexcept
            : ctrl(other.ctrl)
        {
            if (ctrl) {
                ctrl->weak.fetch_add(1, std::memory_order_acq_rel);
            }
        }

        /// \brief Move constructor.
        SafeWeakPtr(SafeWeakPtr&& other) noexcept
            : ctrl(std::exchange(other.ctrl, nullptr))
        {}

        /// \brief Destructor – releases weak reference.
        ~SafeWeakPtr() {
            release_weak();
        }

        /// \brief Copy assignment.
        SafeWeakPtr& operator=(const SafeWeakPtr& other) noexcept {
            if (this != &other) {
                release_weak();
                ctrl = other.ctrl;
                if (ctrl) {
                    ctrl->weak.fetch_add(1, std::memory_order_acq_rel);
                }
            }
            return *this;
        }

        /// \brief Move assignment.
        SafeWeakPtr& operator=(SafeWeakPtr&& other) noexcept {
            if (this != &other) {
                release_weak();
                ctrl = std::exchange(other.ctrl, nullptr);
            }
            return *this;
        }

        /// \brief Returns true if no object is currently available.
        [[nodiscard]] bool expired() const noexcept {
            return !ctrl ||
                   ctrl->ptr.load(std::memory_order_acquire) == nullptr;
        }

        /// \brief Non-blocking attempt to acquire a ReadGuard.
        ///
        /// Returns std::nullopt if:
        ///   - no control block
        ///   - no object available
        [[nodiscard]] std::optional<typename Strong::ReadGuard> try_read() const noexcept {
            if (!ctrl) {
                return std::nullopt;
            }
            if (ctrl->ptr.load(std::memory_order_acquire) == nullptr) {
                return std::nullopt;
            }

            return std::optional<typename Strong::ReadGuard>(
                typename Strong::ReadGuard(ctrl, std::nothrow_t{})
            );
        }

        /// \brief Non-blocking attempt to acquire a WriteGuard.
        ///
        /// Returns std::nullopt if:
        ///   - no control block
        ///   - no object available
        ///   - writer mutex already locked
        [[nodiscard]] std::optional<typename Strong::WriteGuard> try_write() noexcept {
            if (!ctrl) {
                return std::nullopt;
            }
            if (ctrl->ptr.load(std::memory_order_acquire) == nullptr) {
                return std::nullopt;
            }

            std::unique_lock<std::mutex> lk(ctrl->write_mtx, std::try_to_lock);
            if (!lk.owns_lock()) {
                return std::nullopt;
            }

            return std::optional<typename Strong::WriteGuard>(
                typename Strong::WriteGuard(ctrl, std::move(lk), true)
            );
        }
    };

    // ============================================================
    //  Aliases – drop-in smart-pointer replacements
    // ============================================================

    /// \brief Thread-safe replacement for std::unique_ptr<T>.
    template<typename T, typename Deleter = std::default_delete<T>>
    using SafeUniquePtr = SafePtr<T, Deleter>;

    /// \brief Non-owning counterpart to SafeUniquePtr<T>.
    template<typename T, typename Deleter = std::default_delete<T>>
    using SafeWeakUniquePtr = SafeWeakPtr<T, Deleter>;

} // namespace safeptr
