// vortex/result.hpp — Expected-style Result type for error handling.
//
// VORTEX uses Result<T> for fallible operations instead of exceptions
// or out-parameters. This keeps the API ergonomic and exception-free.
//
// T must be movable but need NOT be default-constructible.
//
// Usage:
//   vortex::Result<vortex::Bytecode> bc = vortex::Bytecode::load("file.vtbc");
//   if (bc.is_err()) {
//       std::cerr << "load failed: " << bc.error() << "\n";
//       return 1;
//   }
//   vortex::Bytecode& b = bc.value();

#ifndef VORTEX_RESULT_HPP
#define VORTEX_RESULT_HPP

#include <cassert>
#include <new>
#include <string>
#include <utility>
#include <type_traits>

namespace vortex {

template <typename T>
class Result {
public:
    /* Success constructor */
    Result(T v) : ok_(true) {
        new (&storage_) T(std::move(v));
    }

    /* Error constructor (from string) */
    static Result err(std::string e) {
        Result r(false);
        r.error_ = std::move(e);
        return r;
    }
    static Result err(const char* e) {
        Result r(false);
        r.error_ = e;
        return r;
    }

    /* Move constructor/assignment */
    Result(Result&& other) : ok_(other.ok_) {
        if (ok_) {
            new (&storage_) T(std::move(other.value()));
        } else {
            error_ = std::move(other.error_);
        }
    }
    Result& operator=(Result&& other) {
        if (this != &other) {
            destroy();
            ok_ = other.ok_;
            if (ok_) {
                new (&storage_) T(std::move(other.value()));
            } else {
                error_ = std::move(other.error_);
            }
        }
        return *this;
    }

    /* No copy */
    Result(const Result&) = delete;
    Result& operator=(const Result&) = delete;

    ~Result() { destroy(); }

    bool is_ok()  const { return ok_; }
    bool is_err() const { return !ok_; }

    /* Access the value. UB if is_err(). */
    T& value() { assert(ok_); return *reinterpret_cast<T*>(&storage_); }
    const T& value() const { assert(ok_); return *reinterpret_cast<const T*>(&storage_); }

    /* Access the error message. UB if is_ok(). */
    const std::string& error() const { assert(!ok_); return error_; }

    /* Convenience: dereference to value (must be ok) */
    T& operator*()  { return value(); }
    const T& operator*() const { return value(); }
    T* operator->() { return &value(); }
    const T* operator->() const { return &value(); }

    /* Convert to bool for use in conditionals */
    explicit operator bool() const { return ok_; }

private:
    Result(bool ok) : ok_(ok) {}

    void destroy() {
        if (ok_) {
            reinterpret_cast<T*>(&storage_)->~T();
        }
    }

    bool ok_;
    typename std::aligned_storage<sizeof(T), alignof(T)>::type storage_;
    std::string error_;
};

/* Void specialization for operations that can fail but return nothing */
template <>
class Result<void> {
public:
    Result() : ok_(true) {}

    static Result err(std::string e) {
        Result r(false);
        r.error_ = std::move(e);
        return r;
    }
    static Result err(const char* e) {
        Result r(false);
        r.error_ = e;
        return r;
    }

    bool is_ok()  const { return ok_; }
    bool is_err() const { return !ok_; }
    const std::string& error() const { return error_; }
    explicit operator bool() const { return ok_; }

private:
    explicit Result(bool ok) : ok_(ok) {}
    bool ok_;
    std::string error_;
};

} // namespace vortex

#endif // VORTEX_RESULT_HPP
