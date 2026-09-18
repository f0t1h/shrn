// shrn: run commands and check their outputs. C++20, POSIX.
// Define SHRN_NO_ZLIB to disable gzip support.
// Define SHRN_FORCE_FALLBACK_EXPECTED to use the built-in expected implementation.

#ifndef SHRN_HPP
#define SHRN_HPP

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#if __has_include(<expected>)
#include <expected>
#endif

#if !defined(SHRN_FORCE_FALLBACK_EXPECTED) && defined(__cpp_lib_expected) && __cpp_lib_expected >= 202202L
#define SHRN_HAS_STD_EXPECTED 1
#else
#define SHRN_HAS_STD_EXPECTED 0
#endif

#if !defined(SHRN_NO_ZLIB) && __has_include(<zlib.h>)
#include <zlib.h>
#define SHRN_HAS_ZLIB 1
#else
#define SHRN_HAS_ZLIB 0
#endif

namespace shrn {

// expected

#if SHRN_HAS_STD_EXPECTED

template <class T, class E> using expected = std::expected<T, E>;
template <class E> using unexpected = std::unexpected<E>;
template <class E> using bad_expected_access = std::bad_expected_access<E>;
using unexpect_t = std::unexpect_t;
using std::unexpect;

#else  // fallback implementation

struct unexpect_t {
    explicit unexpect_t() = default;
};
inline constexpr unexpect_t unexpect{};

template <class E> class bad_expected_access;

template <> class bad_expected_access<void> : public std::exception {
public:
    const char* what() const noexcept override {
        return "bad access to shrn::expected without expected value";
    }
};

template <class E> class bad_expected_access : public bad_expected_access<void> {
public:
    explicit bad_expected_access(E e) : err_(std::move(e)) {}
    E& error() & noexcept { return err_; }
    const E& error() const& noexcept { return err_; }
    E&& error() && noexcept { return std::move(err_); }

private:
    E err_;
};

template <class E> class unexpected {
    static_assert(!std::is_reference_v<E> && !std::is_void_v<E>);

public:
    constexpr unexpected(const unexpected&) = default;
    constexpr unexpected(unexpected&&) = default;

    template <class Err = E>
        requires(!std::is_same_v<std::remove_cvref_t<Err>, unexpected> &&
                 !std::is_same_v<std::remove_cvref_t<Err>, std::in_place_t> &&
                 std::is_constructible_v<E, Err>)
    constexpr explicit unexpected(Err&& e) : err_(std::forward<Err>(e)) {}

    template <class... Args>
        requires std::is_constructible_v<E, Args...>
    constexpr explicit unexpected(std::in_place_t, Args&&... args) : err_(std::forward<Args>(args)...) {}

    constexpr E& error() & noexcept { return err_; }
    constexpr const E& error() const& noexcept { return err_; }
    constexpr E&& error() && noexcept { return std::move(err_); }
    constexpr const E&& error() const&& noexcept { return std::move(err_); }

    template <class G>
    friend constexpr bool operator==(const unexpected& a, const unexpected<G>& b) {
        return a.error() == b.error();
    }

private:
    E err_;
};

template <class E> unexpected(E) -> unexpected<E>;

namespace detail {
template <class T> struct is_unexpected : std::false_type {};
template <class E> struct is_unexpected<unexpected<E>> : std::true_type {};
template <class T> struct is_expected : std::false_type {};
}  // namespace detail

template <class T, class E> class expected;

namespace detail {
template <class T, class E> struct is_expected<expected<T, E>> : std::true_type {};
}  // namespace detail

template <class T, class E> class expected {
    static_assert(!std::is_reference_v<T> && !std::is_void_v<E>);

public:
    using value_type = T;
    using error_type = E;
    using unexpected_type = unexpected<E>;
    template <class U> using rebind = expected<U, E>;

    constexpr expected()
        requires std::is_default_constructible_v<T>
        : v_(std::in_place_index<0>) {}
    constexpr expected(const expected&) = default;
    constexpr expected(expected&&) = default;

    // Assign in place when the state is unchanged. If constructing the other
    // alternative throws, restore the previous state.
    constexpr expected& operator=(const expected& rhs) noexcept(
        std::is_nothrow_copy_constructible_v<T> && std::is_nothrow_copy_assignable_v<T> &&
        std::is_nothrow_copy_constructible_v<E> && std::is_nothrow_copy_assignable_v<E>)
        requires(std::is_copy_constructible_v<T> && std::is_copy_assignable_v<T> &&
                 std::is_copy_constructible_v<E> && std::is_copy_assignable_v<E> &&
                 (std::is_nothrow_move_constructible_v<T> || std::is_nothrow_move_constructible_v<E>))
    {
        if (has_value() && rhs.has_value()) **this = *rhs;
        else if (!has_value() && !rhs.has_value()) error() = rhs.error();
        else if (rhs.has_value()) reinit<0>(*rhs);
        else reinit<1>(rhs.error());
        return *this;
    }
    constexpr expected& operator=(expected&& rhs) noexcept(
        std::is_nothrow_move_constructible_v<T> && std::is_nothrow_move_assignable_v<T> &&
        std::is_nothrow_move_constructible_v<E> && std::is_nothrow_move_assignable_v<E>)
        requires(std::is_move_constructible_v<T> && std::is_move_assignable_v<T> &&
                 std::is_move_constructible_v<E> && std::is_move_assignable_v<E> &&
                 (std::is_nothrow_move_constructible_v<T> || std::is_nothrow_move_constructible_v<E>))
    {
        if (has_value() && rhs.has_value()) **this = std::move(*rhs);
        else if (!has_value() && !rhs.has_value()) error() = std::move(rhs.error());
        else if (rhs.has_value()) reinit<0>(std::move(*rhs));
        else reinit<1>(std::move(rhs.error()));
        return *this;
    }

    template <class U = T>
        requires(!std::is_same_v<std::remove_cvref_t<U>, std::in_place_t> &&
                 !std::is_same_v<std::remove_cvref_t<U>, expected> &&
                 !detail::is_unexpected<std::remove_cvref_t<U>>::value &&
                 std::is_constructible_v<T, U>)
    constexpr explicit(!std::is_convertible_v<U, T>) expected(U&& u)
        : v_(std::in_place_index<0>, std::forward<U>(u)) {}

    template <class G>
        requires std::is_constructible_v<E, const G&>
    constexpr explicit(!std::is_convertible_v<const G&, E>) expected(const unexpected<G>& u)
        : v_(std::in_place_index<1>, u.error()) {}

    template <class G>
        requires std::is_constructible_v<E, G>
    constexpr explicit(!std::is_convertible_v<G, E>) expected(unexpected<G>&& u)
        : v_(std::in_place_index<1>, std::move(u).error()) {}

    template <class... Args>
        requires std::is_constructible_v<T, Args...>
    constexpr explicit expected(std::in_place_t, Args&&... args)
        : v_(std::in_place_index<0>, std::forward<Args>(args)...) {}

    template <class... Args>
        requires std::is_constructible_v<E, Args...>
    constexpr explicit expected(unexpect_t, Args&&... args)
        : v_(std::in_place_index<1>, std::forward<Args>(args)...) {}

    template <class U = T>
        requires(!std::is_same_v<std::remove_cvref_t<U>, expected> &&
                 !detail::is_unexpected<std::remove_cvref_t<U>>::value &&
                 std::is_constructible_v<T, U> && std::is_assignable_v<T&, U> &&
                 (std::is_nothrow_constructible_v<T, U> || std::is_nothrow_move_constructible_v<T> ||
                  std::is_nothrow_move_constructible_v<E>))
    constexpr expected& operator=(U&& u) {
        if (has_value()) **this = std::forward<U>(u);
        else reinit<0>(std::forward<U>(u));
        return *this;
    }
    template <class G>
        requires(std::is_constructible_v<E, const G&> && std::is_assignable_v<E&, const G&> &&
                 (std::is_nothrow_constructible_v<E, const G&> || std::is_nothrow_move_constructible_v<T> ||
                  std::is_nothrow_move_constructible_v<E>))
    constexpr expected& operator=(const unexpected<G>& u) {
        if (has_value()) reinit<1>(u.error());
        else error() = u.error();
        return *this;
    }
    template <class G>
        requires(std::is_constructible_v<E, G> && std::is_assignable_v<E&, G> &&
                 (std::is_nothrow_constructible_v<E, G> || std::is_nothrow_move_constructible_v<T> ||
                  std::is_nothrow_move_constructible_v<E>))
    constexpr expected& operator=(unexpected<G>&& u) {
        if (has_value()) reinit<1>(std::move(u).error());
        else error() = std::move(u).error();
        return *this;
    }

    template <class... Args>
        requires std::is_nothrow_constructible_v<T, Args...>
    constexpr T& emplace(Args&&... args) noexcept {
        return v_.template emplace<0>(std::forward<Args>(args)...);
    }

    constexpr void swap(expected& other) noexcept(
        std::is_nothrow_move_constructible_v<T> && std::is_nothrow_swappable_v<T> &&
        std::is_nothrow_move_constructible_v<E> && std::is_nothrow_swappable_v<E>)
        requires(std::is_swappable_v<T> && std::is_swappable_v<E> && std::is_move_constructible_v<T> &&
                 std::is_move_constructible_v<E> &&
                 (std::is_nothrow_move_constructible_v<T> || std::is_nothrow_move_constructible_v<E>))
    {
        using std::swap;
        if (has_value() == other.has_value()) {
            if (has_value()) swap(**this, *other);
            else swap(error(), other.error());
        } else if (!has_value()) {
            other.swap(*this);
        } else if constexpr (std::is_nothrow_move_constructible_v<E>) {
            E tmp(std::move(other.error()));
            if constexpr (std::is_nothrow_move_constructible_v<T>) {
                other.v_.template emplace<0>(std::move(**this));
            } else {
                try {
                    other.v_.template emplace<0>(std::move(**this));
                } catch (...) {
                    other.v_.template emplace<1>(std::move(tmp));  // nothrow
                    throw;
                }
            }
            v_.template emplace<1>(std::move(tmp));  // nothrow
        } else {
            T tmp(std::move(**this));  // nothrow: E is not nothrow-move-constructible, so T is
            try {
                v_.template emplace<1>(std::move(other.error()));
            } catch (...) {
                v_.template emplace<0>(std::move(tmp));  // nothrow
                throw;
            }
            other.v_.template emplace<0>(std::move(tmp));  // nothrow
        }
    }

    [[nodiscard]] constexpr bool has_value() const noexcept { return v_.index() == 0; }
    constexpr explicit operator bool() const noexcept { return has_value(); }

    constexpr T* operator->() noexcept { return std::get_if<0>(&v_); }
    constexpr const T* operator->() const noexcept { return std::get_if<0>(&v_); }
    constexpr T& operator*() & noexcept { return *std::get_if<0>(&v_); }
    constexpr const T& operator*() const& noexcept { return *std::get_if<0>(&v_); }
    constexpr T&& operator*() && noexcept { return std::move(*std::get_if<0>(&v_)); }
    constexpr const T&& operator*() const&& noexcept { return std::move(*std::get_if<0>(&v_)); }

    constexpr T& value() & {
        if (!has_value()) throw bad_expected_access<E>(error());
        return **this;
    }
    constexpr const T& value() const& {
        if (!has_value()) throw bad_expected_access<E>(error());
        return **this;
    }
    constexpr T&& value() && {
        if (!has_value()) throw bad_expected_access<E>(std::move(error()));
        return std::move(**this);
    }
    constexpr const T&& value() const&& {
        if (!has_value()) throw bad_expected_access<E>(std::move(error()));
        return std::move(**this);
    }

    constexpr E& error() & noexcept { return *std::get_if<1>(&v_); }
    constexpr const E& error() const& noexcept { return *std::get_if<1>(&v_); }
    constexpr E&& error() && noexcept { return std::move(*std::get_if<1>(&v_)); }
    constexpr const E&& error() const&& noexcept { return std::move(*std::get_if<1>(&v_)); }

    template <class U>
    constexpr T value_or(U&& dflt) const& {
        return has_value() ? **this : static_cast<T>(std::forward<U>(dflt));
    }
    template <class U>
    constexpr T value_or(U&& dflt) && {
        return has_value() ? std::move(**this) : static_cast<T>(std::forward<U>(dflt));
    }
    template <class G = E>
    constexpr E error_or(G&& dflt) const& {
        return has_value() ? static_cast<E>(std::forward<G>(dflt)) : error();
    }
    template <class G = E>
    constexpr E error_or(G&& dflt) && {
        return has_value() ? static_cast<E>(std::forward<G>(dflt)) : std::move(error());
    }

    // Operations preserve the value's constness and reference category.
    template <class F>
    constexpr auto and_then(F&& f) & {
        return do_and_then(*this, std::forward<F>(f));
    }
    template <class F>
    constexpr auto and_then(F&& f) const& {
        return do_and_then(*this, std::forward<F>(f));
    }
    template <class F>
    constexpr auto and_then(F&& f) && {
        return do_and_then(std::move(*this), std::forward<F>(f));
    }
    template <class F>
    constexpr auto and_then(F&& f) const&& {
        return do_and_then(std::move(*this), std::forward<F>(f));
    }
    template <class F>
    constexpr auto transform(F&& f) & {
        return do_transform(*this, std::forward<F>(f));
    }
    template <class F>
    constexpr auto transform(F&& f) const& {
        return do_transform(*this, std::forward<F>(f));
    }
    template <class F>
    constexpr auto transform(F&& f) && {
        return do_transform(std::move(*this), std::forward<F>(f));
    }
    template <class F>
    constexpr auto transform(F&& f) const&& {
        return do_transform(std::move(*this), std::forward<F>(f));
    }
    template <class F>
    constexpr auto or_else(F&& f) & {
        return do_or_else(*this, std::forward<F>(f));
    }
    template <class F>
    constexpr auto or_else(F&& f) const& {
        return do_or_else(*this, std::forward<F>(f));
    }
    template <class F>
    constexpr auto or_else(F&& f) && {
        return do_or_else(std::move(*this), std::forward<F>(f));
    }
    template <class F>
    constexpr auto or_else(F&& f) const&& {
        return do_or_else(std::move(*this), std::forward<F>(f));
    }
    template <class F>
    constexpr auto transform_error(F&& f) & {
        return do_transform_error(*this, std::forward<F>(f));
    }
    template <class F>
    constexpr auto transform_error(F&& f) const& {
        return do_transform_error(*this, std::forward<F>(f));
    }
    template <class F>
    constexpr auto transform_error(F&& f) && {
        return do_transform_error(std::move(*this), std::forward<F>(f));
    }
    template <class F>
    constexpr auto transform_error(F&& f) const&& {
        return do_transform_error(std::move(*this), std::forward<F>(f));
    }

    template <class U, class G>
    friend constexpr bool operator==(const expected& a, const expected<U, G>& b) {
        if (a.has_value() != b.has_value()) return false;
        return a.has_value() ? (*a == *b) : (a.error() == b.error());
    }
    template <class U>
        requires(!detail::is_expected<U>::value && !detail::is_unexpected<U>::value)
    friend constexpr bool operator==(const expected& a, const U& v) {
        return a.has_value() && *a == v;
    }
    template <class G>
    friend constexpr bool operator==(const expected& a, const unexpected<G>& u) {
        return !a.has_value() && a.error() == u.error();
    }

private:
    // Switch to alternative I; restore the previous alternative if construction throws.
    template <std::size_t I, class... Args>
    constexpr void reinit(Args&&... args) {
        constexpr std::size_t J = I == 0 ? 1 : 0;
        using New = std::variant_alternative_t<I, std::variant<T, E>>;
        using Old = std::variant_alternative_t<J, std::variant<T, E>>;
        if constexpr (std::is_nothrow_constructible_v<New, Args...>) {
            v_.template emplace<I>(std::forward<Args>(args)...);
        } else if constexpr (std::is_nothrow_move_constructible_v<New>) {
            New tmp(std::forward<Args>(args)...);  // throws before the old value is touched
            v_.template emplace<I>(std::move(tmp));
        } else {
            static_assert(std::is_nothrow_move_constructible_v<Old>);
            Old tmp(std::move(*std::get_if<J>(&v_)));
            try {
                v_.template emplace<I>(std::forward<Args>(args)...);
            } catch (...) {
                v_.template emplace<J>(std::move(tmp));  // nothrow: never observably valueless
                throw;
            }
        }
    }

    template <class Self, class F>
    static constexpr auto do_and_then(Self&& self, F&& f) {
        using U = std::remove_cvref_t<std::invoke_result_t<F, decltype(*std::forward<Self>(self))>>;
        static_assert(detail::is_expected<U>::value, "and_then must return an expected");
        if (!self.has_value()) return U(unexpect, std::forward<Self>(self).error());
        return std::invoke(std::forward<F>(f), *std::forward<Self>(self));
    }
    template <class Self, class F>
    static constexpr auto do_transform(Self&& self, F&& f) {
        using U = std::remove_cv_t<std::invoke_result_t<F, decltype(*std::forward<Self>(self))>>;
        if constexpr (std::is_void_v<U>) {
            if (!self.has_value()) return expected<void, E>(unexpect, std::forward<Self>(self).error());
            std::invoke(std::forward<F>(f), *std::forward<Self>(self));
            return expected<void, E>();
        } else {
            if (!self.has_value()) return expected<U, E>(unexpect, std::forward<Self>(self).error());
            return expected<U, E>(std::in_place, std::invoke(std::forward<F>(f), *std::forward<Self>(self)));
        }
    }
    template <class Self, class F>
    static constexpr auto do_or_else(Self&& self, F&& f) {
        using G = std::remove_cvref_t<std::invoke_result_t<F, decltype(std::forward<Self>(self).error())>>;
        static_assert(detail::is_expected<G>::value, "or_else must return an expected");
        if (!self.has_value()) return std::invoke(std::forward<F>(f), std::forward<Self>(self).error());
        return G(std::in_place, *std::forward<Self>(self));
    }
    template <class Self, class F>
    static constexpr auto do_transform_error(Self&& self, F&& f) {
        using G = std::remove_cv_t<std::invoke_result_t<F, decltype(std::forward<Self>(self).error())>>;
        if (!self.has_value())
            return expected<T, G>(unexpect, std::invoke(std::forward<F>(f), std::forward<Self>(self).error()));
        return expected<T, G>(std::in_place, *std::forward<Self>(self));
    }

    std::variant<T, E> v_;
};

template <class E> class expected<void, E> {
    static_assert(!std::is_void_v<E>);

public:
    using value_type = void;
    using error_type = E;
    using unexpected_type = unexpected<E>;
    template <class U> using rebind = expected<U, E>;

    // optional assignment preserves the value/error state when construction throws.
    constexpr expected() noexcept = default;
    constexpr expected(const expected&) = default;
    constexpr expected(expected&&) = default;
    constexpr expected& operator=(const expected&) = default;
    constexpr expected& operator=(expected&&) = default;

    template <class G>
        requires std::is_constructible_v<E, const G&>
    constexpr explicit(!std::is_convertible_v<const G&, E>) expected(const unexpected<G>& u)
        : e_(std::in_place, u.error()) {}
    template <class G>
        requires std::is_constructible_v<E, G>
    constexpr explicit(!std::is_convertible_v<G, E>) expected(unexpected<G>&& u)
        : e_(std::in_place, std::move(u).error()) {}
    constexpr explicit expected(std::in_place_t) noexcept {}
    template <class... Args>
        requires std::is_constructible_v<E, Args...>
    constexpr explicit expected(unexpect_t, Args&&... args) : e_(std::in_place, std::forward<Args>(args)...) {}

    template <class G>
        requires(std::is_constructible_v<E, const G&> && std::is_assignable_v<E&, const G&>)
    constexpr expected& operator=(const unexpected<G>& u) {
        if (e_.has_value()) *e_ = u.error();  // same state: assign, never reconstruct
        else e_.emplace(u.error());           // throwing: stays in the value state
        return *this;
    }
    template <class G>
        requires(std::is_constructible_v<E, G> && std::is_assignable_v<E&, G>)
    constexpr expected& operator=(unexpected<G>&& u) {
        if (e_.has_value()) *e_ = std::move(u).error();
        else e_.emplace(std::move(u).error());
        return *this;
    }

    constexpr void emplace() noexcept { e_.reset(); }
    constexpr void swap(expected& other) noexcept(std::is_nothrow_move_constructible_v<E> &&
                                                  std::is_nothrow_swappable_v<E>)
        requires(std::is_swappable_v<E> && std::is_move_constructible_v<E>)
    {
        if (e_.has_value() == other.e_.has_value()) {
            if (e_.has_value()) {
                using std::swap;
                swap(*e_, *other.e_);
            }
            return;
        }
        // Construct the destination error before clearing the source.
        // If construction throws, the destination remains in the value state.
        std::optional<E>& from = e_.has_value() ? e_ : other.e_;
        std::optional<E>& to = e_.has_value() ? other.e_ : e_;
        to.emplace(std::move(*from));
        from.reset();
    }

    [[nodiscard]] constexpr bool has_value() const noexcept { return !e_.has_value(); }
    constexpr explicit operator bool() const noexcept { return has_value(); }
    constexpr void operator*() const noexcept {}
    constexpr void value() const& {
        if (!has_value()) throw bad_expected_access<E>(error());
    }
    constexpr void value() && {
        if (!has_value()) throw bad_expected_access<E>(std::move(error()));
    }

    constexpr E& error() & noexcept { return *e_; }
    constexpr const E& error() const& noexcept { return *e_; }
    constexpr E&& error() && noexcept { return std::move(*e_); }
    constexpr const E&& error() const&& noexcept { return std::move(*e_); }

    template <class G = E>
    constexpr E error_or(G&& dflt) const& {
        return has_value() ? static_cast<E>(std::forward<G>(dflt)) : error();
    }
    template <class G = E>
    constexpr E error_or(G&& dflt) && {
        return has_value() ? static_cast<E>(std::forward<G>(dflt)) : std::move(error());
    }

    // Operations preserve the error's constness and reference category.
    template <class F>
    constexpr auto and_then(F&& f) & {
        return do_and_then(*this, std::forward<F>(f));
    }
    template <class F>
    constexpr auto and_then(F&& f) const& {
        return do_and_then(*this, std::forward<F>(f));
    }
    template <class F>
    constexpr auto and_then(F&& f) && {
        return do_and_then(std::move(*this), std::forward<F>(f));
    }
    template <class F>
    constexpr auto and_then(F&& f) const&& {
        return do_and_then(std::move(*this), std::forward<F>(f));
    }
    template <class F>
    constexpr auto transform(F&& f) & {
        return do_transform(*this, std::forward<F>(f));
    }
    template <class F>
    constexpr auto transform(F&& f) const& {
        return do_transform(*this, std::forward<F>(f));
    }
    template <class F>
    constexpr auto transform(F&& f) && {
        return do_transform(std::move(*this), std::forward<F>(f));
    }
    template <class F>
    constexpr auto transform(F&& f) const&& {
        return do_transform(std::move(*this), std::forward<F>(f));
    }
    template <class F>
    constexpr auto or_else(F&& f) & {
        return do_or_else(*this, std::forward<F>(f));
    }
    template <class F>
    constexpr auto or_else(F&& f) const& {
        return do_or_else(*this, std::forward<F>(f));
    }
    template <class F>
    constexpr auto or_else(F&& f) && {
        return do_or_else(std::move(*this), std::forward<F>(f));
    }
    template <class F>
    constexpr auto or_else(F&& f) const&& {
        return do_or_else(std::move(*this), std::forward<F>(f));
    }
    template <class F>
    constexpr auto transform_error(F&& f) & {
        return do_transform_error(*this, std::forward<F>(f));
    }
    template <class F>
    constexpr auto transform_error(F&& f) const& {
        return do_transform_error(*this, std::forward<F>(f));
    }
    template <class F>
    constexpr auto transform_error(F&& f) && {
        return do_transform_error(std::move(*this), std::forward<F>(f));
    }
    template <class F>
    constexpr auto transform_error(F&& f) const&& {
        return do_transform_error(std::move(*this), std::forward<F>(f));
    }

    template <class G>
    friend constexpr bool operator==(const expected& a, const expected<void, G>& b) {
        if (a.has_value() != b.has_value()) return false;
        return a.has_value() || a.error() == b.error();
    }
    template <class G>
    friend constexpr bool operator==(const expected& a, const unexpected<G>& u) {
        return !a.has_value() && a.error() == u.error();
    }

private:
    template <class Self, class F>
    static constexpr auto do_and_then(Self&& self, F&& f) {
        using U = std::remove_cvref_t<std::invoke_result_t<F>>;
        static_assert(detail::is_expected<U>::value, "and_then must return an expected");
        if (!self.has_value()) return U(unexpect, std::forward<Self>(self).error());
        return std::invoke(std::forward<F>(f));
    }
    template <class Self, class F>
    static constexpr auto do_transform(Self&& self, F&& f) {
        using U = std::remove_cv_t<std::invoke_result_t<F>>;
        if constexpr (std::is_void_v<U>) {
            if (!self.has_value()) return expected<void, E>(unexpect, std::forward<Self>(self).error());
            std::invoke(std::forward<F>(f));
            return expected<void, E>();
        } else {
            if (!self.has_value()) return expected<U, E>(unexpect, std::forward<Self>(self).error());
            return expected<U, E>(std::in_place, std::invoke(std::forward<F>(f)));
        }
    }
    template <class Self, class F>
    static constexpr auto do_or_else(Self&& self, F&& f) {
        using G = std::remove_cvref_t<std::invoke_result_t<F, decltype(std::forward<Self>(self).error())>>;
        static_assert(detail::is_expected<G>::value, "or_else must return an expected");
        if (!self.has_value()) return std::invoke(std::forward<F>(f), std::forward<Self>(self).error());
        return G();
    }
    template <class Self, class F>
    static constexpr auto do_transform_error(Self&& self, F&& f) {
        using G = std::remove_cv_t<std::invoke_result_t<F, decltype(std::forward<Self>(self).error())>>;
        if (!self.has_value())
            return expected<void, G>(unexpect, std::invoke(std::forward<F>(f), std::forward<Self>(self).error()));
        return expected<void, G>();
    }

    std::optional<E> e_;
};

#endif  // SHRN_HAS_STD_EXPECTED

template <class T> using result = expected<T, std::error_code>;

// Error codes

enum class errc {
    empty_file = 1,     ///< file exists but has no content
    zlib_unavailable,   ///< gzip input but library built without zlib
    zlib_error,         ///< zlib reported a stream error
    empty_command,      ///< run() called with no argv[0]
};

namespace detail {
class error_category_impl final : public std::error_category {
public:
    const char* name() const noexcept override { return "shrn"; }
    std::string message(int c) const override {
        switch (static_cast<errc>(c)) {
            case errc::empty_file: return "file is empty";
            case errc::zlib_unavailable: return "gzip input but built without zlib";
            case errc::zlib_error: return "zlib stream error";
            case errc::empty_command: return "empty command";
        }
        return "unknown shrn error";
    }
};
}  // namespace detail

inline const std::error_category& error_category() noexcept {
    static const detail::error_category_impl cat;
    return cat;
}

inline std::error_code make_error_code(errc e) noexcept {
    return {static_cast<int>(e), error_category()};
}

}  // namespace shrn

template <> struct std::is_error_code_enum<shrn::errc> : std::true_type {};

namespace shrn {

namespace fs = std::filesystem;

namespace detail {

inline std::error_code errno_code(std::errc fallback = std::errc::io_error) noexcept {
    int e = errno;
    return e != 0 ? std::error_code(e, std::generic_category()) : std::make_error_code(fallback);
}

inline std::size_t count_newlines(const char* data, std::size_t n) noexcept {
    std::size_t count = 0;
    const char* end = data + n;
    while (const char* hit = static_cast<const char*>(std::memchr(data, '\n', static_cast<std::size_t>(end - data)))) {
        ++count;
        data = hit + 1;
    }
    return count;
}

struct unique_fd {
    int fd = -1;
    unique_fd() = default;
    explicit unique_fd(int f) noexcept : fd(f) {}
    unique_fd(const unique_fd&) = delete;
    unique_fd& operator=(const unique_fd&) = delete;
    unique_fd(unique_fd&& o) noexcept : fd(o.fd) { o.fd = -1; }
    unique_fd& operator=(unique_fd&& o) noexcept {
        if (this != &o) {
            reset();
            fd = o.fd;
            o.fd = -1;
        }
        return *this;
    }
    ~unique_fd() { reset(); }
    void reset() noexcept {
        if (fd != -1) {
            ::close(fd);
            fd = -1;
        }
    }
    int release() noexcept {
        int f = fd;
        fd = -1;
        return f;
    }
    explicit operator bool() const noexcept { return fd != -1; }
};

#if SHRN_HAS_ZLIB
struct gz_guard {
    gzFile gz = nullptr;
    explicit gz_guard(gzFile g) noexcept : gz(g) {}
    gz_guard(const gz_guard&) = delete;
    gz_guard& operator=(const gz_guard&) = delete;
    ~gz_guard() {
        if (gz) gzclose(gz);
    }
    explicit operator bool() const noexcept { return gz != nullptr; }
};

/// Map a zlib failure after gzread/gzgetc to an error_code.
inline std::error_code gz_error(gzFile gz) noexcept {
    int err = Z_OK;
    gzerror(gz, &err);
    if (err == Z_ERRNO) return errno_code();
    return make_error_code(errc::zlib_error);
}
#endif

}  // namespace detail

// File queries

/// Regular file that the current process may read.
[[nodiscard]] inline bool file_readable(const fs::path& p) noexcept {
    std::error_code ec;
    auto st = fs::status(p, ec);
    return !ec && fs::is_regular_file(st) && ::access(p.c_str(), R_OK) == 0;
}

/// Readable regular file with size > 0.
[[nodiscard]] inline bool file_non_empty(const fs::path& p) noexcept {
    if (!file_readable(p)) return false;
    std::error_code ec;
    auto sz = fs::file_size(p, ec);
    return !ec && sz > 0;
}

/// File starts with the gzip magic bytes 1f 8b.
[[nodiscard]] inline bool file_is_gzipped(const fs::path& p) noexcept {
    std::FILE* f = std::fopen(p.c_str(), "rb");
    if (!f) return false;
    unsigned char magic[2];
    bool gz = std::fread(magic, 1, 2, f) == 2 && magic[0] == 0x1f && magic[1] == 0x8b;
    std::fclose(f);
    return gz;
}

/// Whole file contents.
[[nodiscard]] inline result<std::string> read_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return unexpected(detail::errno_code(std::errc::no_such_file_or_directory));

    std::string out;
    std::error_code ec;
    auto st = fs::status(p, ec);
    if (!ec && fs::is_regular_file(st)) {
        auto sz = fs::file_size(p, ec);
        if (!ec) {
            out.resize(static_cast<std::size_t>(sz));
            in.read(out.data(), static_cast<std::streamsize>(sz));
            out.resize(static_cast<std::size_t>(in.gcount()));
            if (in.bad()) return unexpected(detail::errno_code());
        }
    }
    char buffer[1 << 16];
    while (in.read(buffer, sizeof buffer) || in.gcount() > 0) {
        out.append(buffer, static_cast<std::size_t>(in.gcount()));
    }
    if (in.bad()) return unexpected(detail::errno_code());
    return out;
}

/// First content byte of a plain or gzip file; errc::empty_file if empty.
[[nodiscard]] inline result<int> file_first_byte(const fs::path& p) {
#if SHRN_HAS_ZLIB
    errno = 0;
    detail::gz_guard gz(gzopen(p.c_str(), "rb"));
    if (!gz) return unexpected(detail::errno_code(std::errc::not_enough_memory));
    int ch = gzgetc(gz.gz);
    if (ch != -1) return ch;
    int err = Z_OK;
    gzerror(gz.gz, &err);
    if (err == Z_OK || err == Z_STREAM_END) return unexpected(make_error_code(errc::empty_file));
    return unexpected(detail::gz_error(gz.gz));
#else
    if (file_is_gzipped(p)) return unexpected(make_error_code(errc::zlib_unavailable));
    std::FILE* f = std::fopen(p.c_str(), "rb");
    if (!f) return unexpected(detail::errno_code());
    int ch = std::fgetc(f);
    bool err = std::ferror(f) != 0;
    std::fclose(f);
    if (ch != EOF) return ch;
    if (err) return unexpected(std::make_error_code(std::errc::io_error));
    return unexpected(make_error_code(errc::empty_file));
#endif
}

/// Number of '\n' bytes; gzip detected by magic, not extension.
[[nodiscard]] inline result<std::size_t> line_count(const fs::path& p) {
    char buffer[1 << 16];
    if (file_is_gzipped(p)) {
#if SHRN_HAS_ZLIB
        errno = 0;
        detail::gz_guard gz(gzopen(p.c_str(), "rb"));
        if (!gz) return unexpected(detail::errno_code(std::errc::not_enough_memory));
        std::size_t count = 0;
        int n;
        while ((n = gzread(gz.gz, buffer, sizeof buffer)) > 0) {
            count += detail::count_newlines(buffer, static_cast<std::size_t>(n));
        }
        if (n < 0) return unexpected(detail::gz_error(gz.gz));
        return count;
#else
        return unexpected(make_error_code(errc::zlib_unavailable));
#endif
    }

    std::ifstream in(p, std::ios::binary);
    if (!in) return unexpected(detail::errno_code(std::errc::no_such_file_or_directory));
    std::size_t count = 0;
    while (in.read(buffer, sizeof buffer) || in.gcount() > 0) {
        count += detail::count_newlines(buffer, static_cast<std::size_t>(in.gcount()));
    }
    if (in.bad()) return unexpected(detail::errno_code());
    return count;
}

/// Create directory and parents; success if it already exists.
[[nodiscard]] inline result<void> ensure_directory(const fs::path& p) {
    std::error_code ec;
    fs::create_directories(p, ec);
    if (ec) return unexpected(ec);
    return {};
}

/// Remove a file; a missing file is not an error.
[[nodiscard]] inline result<void> remove_if_exists(const fs::path& p) {
    std::error_code ec;
    fs::remove(p, ec);
    if (ec) return unexpected(ec);
    return {};
}

/// Create a symlink, replacing an existing link/file at `link`.
[[nodiscard]] inline result<void> force_symlink(const fs::path& target, const fs::path& link) {
    if (auto r = remove_if_exists(link); !r) return r;
    std::error_code ec;
    fs::create_symlink(target, link, ec);
    if (ec) return unexpected(ec);
    return {};
}

enum class concat_mode {
    raw,         ///< byte-for-byte; gzip members concatenate into a valid multi-member stream
    decompress,  ///< decompress gzip inputs and copy plain inputs unchanged
};

/// Concatenate `inputs` into `output` (truncated first).
[[nodiscard]] inline result<void> concat_files(const std::vector<fs::path>& inputs,
                                               const fs::path& output,
                                               concat_mode mode = concat_mode::raw) {
    std::ofstream out(output, std::ios::binary | std::ios::trunc);
    if (!out) return unexpected(detail::errno_code());

    char buffer[1 << 16];
    for (const auto& input : inputs) {
        if (mode == concat_mode::decompress) {
#if SHRN_HAS_ZLIB
            errno = 0;
            detail::gz_guard gz(gzopen(input.c_str(), "rb"));
            if (!gz) return unexpected(detail::errno_code(std::errc::not_enough_memory));
            int n;
            while ((n = gzread(gz.gz, buffer, sizeof buffer)) > 0) {
                out.write(buffer, n);
                if (!out) return unexpected(detail::errno_code());
            }
            if (n < 0) return unexpected(detail::gz_error(gz.gz));
            continue;
#else
            if (file_is_gzipped(input)) return unexpected(make_error_code(errc::zlib_unavailable));
#endif
        }
        std::ifstream in(input, std::ios::binary);
        if (!in) return unexpected(detail::errno_code(std::errc::no_such_file_or_directory));
        while (in.read(buffer, sizeof buffer) || in.gcount() > 0) {
            out.write(buffer, in.gcount());
            if (!out) return unexpected(detail::errno_code());
        }
        if (in.bad()) return unexpected(detail::errno_code());
    }
    out.close();
    if (!out) return unexpected(detail::errno_code());
    return {};
}

// Temporary files

/// $TMPDIR if set and non-empty, else /tmp.
[[nodiscard]] inline fs::path temp_directory() {
    const char* t = std::getenv("TMPDIR");
    return (t && *t) ? fs::path(t) : fs::path("/tmp");
}

/// Atomically create an empty file `dir/<prefix>XXXXXX<suffix>` (mkstemps).
[[nodiscard]] inline result<fs::path> make_temp_file_in(const fs::path& dir,
                                                        std::string_view prefix = "shrn_",
                                                        std::string_view suffix = "") {
    std::string tmpl = (dir / std::string(prefix)).string();
    tmpl += "XXXXXX";
    tmpl.append(suffix);
    int fd = ::mkstemps(tmpl.data(), static_cast<int>(suffix.size()));
    if (fd == -1) return unexpected(detail::errno_code());
    ::close(fd);
    return fs::path(std::move(tmpl));
}

/// Create a file in temp_directory(); the caller is responsible for removal.
[[nodiscard]] inline result<fs::path> make_temp_file(std::string_view prefix = "shrn_",
                                                     std::string_view suffix = "") {
    return make_temp_file_in(temp_directory(), prefix, suffix);
}

/// Atomically create a directory `dir/<prefix>XXXXXX` (mkdtemp).
[[nodiscard]] inline result<fs::path> make_temp_dir_in(const fs::path& dir, std::string_view prefix = "shrn_") {
    std::string tmpl = (dir / std::string(prefix)).string();
    tmpl += "XXXXXX";
    if (!::mkdtemp(tmpl.data())) return unexpected(detail::errno_code());
    return fs::path(std::move(tmpl));
}

[[nodiscard]] inline result<fs::path> make_temp_dir(std::string_view prefix = "shrn_") {
    return make_temp_dir_in(temp_directory(), prefix);
}

/// Owns a temp file; removed on destruction unless released.
class TempFile {
public:
    TempFile() = default;

    [[nodiscard]] static result<TempFile> create(std::string_view suffix = "", std::string_view prefix = "shrn_") {
        return make_temp_file(prefix, suffix).transform([](fs::path p) { return TempFile(std::move(p)); });
    }
    [[nodiscard]] static result<TempFile> create_in(const fs::path& dir,
                                                    std::string_view suffix = "",
                                                    std::string_view prefix = "shrn_") {
        return make_temp_file_in(dir, prefix, suffix).transform([](fs::path p) { return TempFile(std::move(p)); });
    }

    ~TempFile() { reset(); }
    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;
    TempFile(TempFile&& o) noexcept : path_(std::move(o.path_)) { o.path_.clear(); }
    TempFile& operator=(TempFile&& o) noexcept {
        if (this != &o) {
            reset();
            path_ = std::move(o.path_);
            o.path_.clear();
        }
        return *this;
    }

    [[nodiscard]] const fs::path& path() const noexcept { return path_; }
    [[nodiscard]] std::string string() const { return path_.string(); }
    explicit operator bool() const noexcept { return !path_.empty(); }

    /// Remove the file and clear ownership; ignore cleanup errors.
    void reset() noexcept {
        if (!path_.empty()) {
            std::error_code ec;
            fs::remove(path_, ec);
            path_.clear();
        }
    }
    /// Return the path without removing the file.
    [[nodiscard]] fs::path release() noexcept {
        fs::path p = std::move(path_);
        path_.clear();
        return p;
    }

private:
    explicit TempFile(fs::path p) noexcept : path_(std::move(p)) {}
    fs::path path_;
};

/// Owns a temp directory tree; removed recursively on destruction unless released.
class TempDir {
public:
    TempDir() = default;

    [[nodiscard]] static result<TempDir> create(std::string_view prefix = "shrn_") {
        return make_temp_dir(prefix).transform([](fs::path p) { return TempDir(std::move(p)); });
    }
    [[nodiscard]] static result<TempDir> create_in(const fs::path& dir, std::string_view prefix = "shrn_") {
        return make_temp_dir_in(dir, prefix).transform([](fs::path p) { return TempDir(std::move(p)); });
    }

    ~TempDir() { reset(); }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    TempDir(TempDir&& o) noexcept : path_(std::move(o.path_)) { o.path_.clear(); }
    TempDir& operator=(TempDir&& o) noexcept {
        if (this != &o) {
            reset();
            path_ = std::move(o.path_);
            o.path_.clear();
        }
        return *this;
    }

    [[nodiscard]] const fs::path& path() const noexcept { return path_; }
    [[nodiscard]] std::string string() const { return path_.string(); }
    explicit operator bool() const noexcept { return !path_.empty(); }
    [[nodiscard]] fs::path operator/(const fs::path& rel) const { return path_ / rel; }

    void reset() noexcept {
        if (!path_.empty()) {
            std::error_code ec;
            fs::remove_all(path_, ec);
            path_.clear();
        }
    }
    [[nodiscard]] fs::path release() noexcept {
        fs::path p = std::move(path_);
        path_.clear();
        return p;
    }

private:
    explicit TempDir(fs::path p) noexcept : path_(std::move(p)) {}
    fs::path path_;
};

// Process execution

/// Shell-style quoting for display only.
[[nodiscard]] inline std::string format_command(const std::vector<std::string>& args) {
    std::string out;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (i > 0) out += ' ';
        const std::string& a = args[i];
        bool quote = a.empty() || a.find_first_of(" \t\n\"'\\$`*?[]<>|;&(){}") != std::string::npos;
        if (!quote) {
            out += a;
            continue;
        }
        out += '\'';
        for (char c : a) {
            if (c == '\'') out += "'\\''";
            else out += c;
        }
        out += '\'';
    }
    return out;
}

/// Resolve `name` against PATH (or verify it directly if it contains '/').
[[nodiscard]] inline std::optional<fs::path> which(std::string_view name) {
    if (name.empty()) return std::nullopt;
    auto executable = [](const fs::path& p) {
        std::error_code ec;
        return fs::is_regular_file(p, ec) && ::access(p.c_str(), X_OK) == 0;
    };
    if (name.find('/') != std::string_view::npos) {
        fs::path p(name);
        return executable(p) ? std::optional<fs::path>(std::move(p)) : std::nullopt;
    }
    const char* path_env = std::getenv("PATH");
    std::string default_path;
    if (!path_env) {
        auto size = ::confstr(_CS_PATH, nullptr, 0);
        if (size == 0) return std::nullopt;
        default_path.resize(size);
        ::confstr(_CS_PATH, default_path.data(), size);
        path_env = default_path.c_str();
    }
    std::string_view path(path_env);
    for (;;) {
        auto sep = path.find(':');
        std::string_view dir = path.substr(0, sep);
        fs::path candidate = fs::path(dir.empty() ? std::string_view(".") : dir) / name;
        if (executable(candidate)) return candidate;
        if (sep == std::string_view::npos) break;
        path.remove_prefix(sep + 1);
    }
    return std::nullopt;
}

/// Callback before starting a command: (stage name, formatted command).
using SpawnHook = std::function<void(std::string_view stage, std::string_view cmd)>;

/// Process-wide hook used when RunOptions::on_spawn is empty (e.g. a "[INFO] Running:" logger).
inline SpawnHook& default_spawn_hook() {
    static SpawnHook hook;
    return hook;
}

struct RunOptions {
    std::optional<fs::path> workdir;              ///< chdir before exec
    bool capture_stderr = true;                   ///< collect child's stderr into RunResult
    bool inherit_stdout = true;                   ///< false → /dev/null (ignored when stdout_file set)
    std::optional<fs::path> stdout_file;          ///< create/truncate; relative to caller, not workdir
    SpawnHook on_spawn;                           ///< overrides default_spawn_hook() for this run
};

struct RunResult {
    int exit_code = -1;        ///< WEXITSTATUS, or -1 if killed by a signal
    int signal = 0;            ///< WTERMSIG, 0 if exited normally
    std::string stderr_output; ///< captured stderr (empty unless capture_stderr)

    [[nodiscard]] bool success() const noexcept { return exit_code == 0 && signal == 0; }

    /// "exited with code N" / "killed by signal N (SIGKILL ...)"
    [[nodiscard]] std::string summary() const {
        if (success()) return "success";
        if (signal != 0) {
            std::string msg = "killed by signal " + std::to_string(signal);
            switch (signal) {
                case SIGKILL: msg += " (SIGKILL - possibly OOM killed)"; break;
                case SIGTERM: msg += " (SIGTERM)"; break;
                case SIGSEGV: msg += " (SIGSEGV - segmentation fault)"; break;
                case SIGABRT: msg += " (SIGABRT - aborted)"; break;
                case SIGPIPE: msg += " (SIGPIPE - broken pipe)"; break;
                default: break;
            }
            return msg;
        }
        return "exited with code " + std::to_string(exit_code);
    }
};

/// Result of a bounded wait: the child was reaped, or the wait window elapsed.
enum class wait_status {
    ready,   ///< the child finished; its status is cached by Process::wait()
    timeout, ///< the window elapsed with the child still running (nothing was killed)
};

namespace detail {

inline bool set_cloexec(int fd) noexcept {
    int flags = ::fcntl(fd, F_GETFD);
    return flags != -1 && ::fcntl(fd, F_SETFD, flags | FD_CLOEXEC) != -1;
}

/// Create a pipe with both ends close-on-exec; use pipe2() where available.
/// On failure `errno` describes the problem; the caller owns any opened ends.
inline bool make_cloexec_pipe(unique_fd& r, unique_fd& w) noexcept {
    int fds[2];
#if defined(__linux__) && defined(_GNU_SOURCE)
    if (::pipe2(fds, O_CLOEXEC) == 0) {
        r = unique_fd(fds[0]);
        w = unique_fd(fds[1]);
        return true;
    }
    if (errno != ENOSYS) return false;
#endif
    if (::pipe(fds) == -1) return false;
    r = unique_fd(fds[0]);
    w = unique_fd(fds[1]);
    return set_cloexec(r.fd) && set_cloexec(w.fd);
}

/// Move internal descriptors above 2 so dup2() cannot overwrite them
/// when the caller has closed a standard descriptor.
inline bool reserve_above_stdio(unique_fd& fd) noexcept {
    if (fd.fd < 0 || fd.fd > STDERR_FILENO) return true;
    int moved = ::fcntl(fd.fd, F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
    if (moved == -1) return false;
    fd = unique_fd(moved);  // closes the low descriptor
    return true;
}

inline pid_t waitpid_retry(pid_t pid, int* status, int options) noexcept {
    pid_t r;
    do {
        r = ::waitpid(pid, status, options);
    } while (r == -1 && errno == EINTR);
    return r;
}

/// Bound capture after child exit so descendants cannot delay the reader indefinitely.
inline std::size_t pipe_capacity(int fd) noexcept {
#ifdef F_GETPIPE_SZ
    int sz = ::fcntl(fd, F_GETPIPE_SZ);
    if (sz > 0) return static_cast<std::size_t>(sz);
#endif
    (void) fd;
    return std::size_t{1} << 20;
}

/// Reported when a Process holds no child (default-constructed or moved from).
inline std::error_code no_child_code() noexcept {
    return std::make_error_code(std::errc::no_child_process);
}

inline constexpr std::chrono::milliseconds wait_poll_interval{10};

/// Sleep at most `ms`; a signal may cut it short and the caller re-checks its deadline.
inline void nap(std::chrono::milliseconds ms) noexcept {
    if (ms.count() <= 0) return;
    timespec ts{static_cast<time_t>(ms.count() / 1000), static_cast<long>((ms.count() % 1000) * 1'000'000L)};
    ::nanosleep(&ts, nullptr);
}

// Stable storage for the reader. The owner reads data and errors only after join().
struct capture_state {
    unique_fd read_fd;             ///< stderr read end; closed by the worker when it stops
    unique_fd wake_r;              ///< stop-signal read end (polled by the worker)
    unique_fd wake_w;              ///< stop-signal write end (written by the owner)
    std::string data;              ///< captured stderr
    std::error_code error;         ///< poll/read failure
    std::exception_ptr exception;  ///< buffer growth failure, retained for the owner
};

// wake_r remains open until join, so this write cannot raise SIGPIPE.
inline void request_stop(capture_state& st) noexcept {
    if (st.wake_w.fd == -1) return;
    const char byte = 0;
    ssize_t n;
    do {
        n = ::write(st.wake_w.fd, &byte, 1);
    } while (n == -1 && errno == EINTR);
}

// Only the reader accesses read_fd, data, error, and exception while active.
inline void capture_worker(capture_state* st) noexcept {
    char buffer[1 << 14];
    bool stopping = false;      ///< stop requested: take only what is already buffered
    std::size_t budget = 0;     ///< bytes still allowed while stopping
    try {
        for (;;) {
            pollfd pfds[2];
            int nfds = 0;
            pfds[nfds++] = pollfd{st->read_fd.fd, POLLIN, 0};
            if (!stopping && st->wake_r.fd != -1) pfds[nfds++] = pollfd{st->wake_r.fd, POLLIN, 0};

            int pr = ::poll(pfds, static_cast<nfds_t>(nfds), stopping ? 0 : -1);
            if (pr == -1) {
                if (errno == EINTR) continue;
                st->error = errno_code();
                break;
            }
            if (pr == 0) break;  // only possible while stopping: nothing left buffered
            if (nfds == 2 && pfds[1].revents != 0) {
                stopping = true;
                budget = pipe_capacity(st->read_fd.fd);
                continue;
            }
            if (pfds[0].revents == 0) continue;

            ssize_t got = ::read(st->read_fd.fd, buffer, sizeof buffer);
            if (got > 0) {
                st->data.append(buffer, static_cast<std::size_t>(got));
                if (stopping) {
                    if (static_cast<std::size_t>(got) >= budget) break;
                    budget -= static_cast<std::size_t>(got);
                }
                continue;
            }
            if (got == -1 && (errno == EINTR || errno == EAGAIN)) continue;
            if (got == -1) {
                st->error = errno_code();
                break;
            }
            break;  // EOF: every writer closed the pipe
        }
    } catch (...) {
        // Only the buffer growth above can throw; keep it for the owner.
        st->exception = std::current_exception();
    }
    st->read_fd.reset();
}

/// Kill and reap the child if spawn() fails after fork().
struct child_reaper {
    pid_t pid;
    explicit child_reaper(pid_t p) noexcept : pid(p) {}
    child_reaper(const child_reaper&) = delete;
    child_reaper& operator=(const child_reaper&) = delete;
    ~child_reaper() {
        if (pid > 0) {
            ::kill(pid, SIGKILL);
            int status;
            waitpid_retry(pid, &status, 0);
        }
    }
    void release() noexcept { pid = -1; }
};

}  // namespace detail

class Process;

/// Start a child and return after the exec handshake, without waiting for completion.
[[nodiscard]] inline result<Process> spawn(const std::vector<std::string>& args,
                                           const RunOptions& opts = {},
                                           std::string_view stage = {});

/// Move-only child owner. Methods are not concurrently callable.
/// Destruction waits for the child and joins the optional stderr reader.
class Process {
public:
    Process() = default;

    Process(Process&& o) noexcept
        : pid_(o.pid_),
          status_(o.status_),
          reaped_(o.reaped_),
          cap_(std::move(o.cap_)),
          reader_(std::move(o.reader_)),
          result_(std::move(o.result_)),
          exception_(std::move(o.exception_)),
          finished_(o.finished_) {
        o.pid_ = -1;
        o.reaped_ = false;
        o.finished_ = false;
    }

    Process& operator=(Process&& o) noexcept {
        if (this != &o) {
            teardown();
            pid_ = o.pid_;
            status_ = o.status_;
            reaped_ = o.reaped_;
            cap_ = std::move(o.cap_);
            reader_ = std::move(o.reader_);
            result_ = std::move(o.result_);
            exception_ = std::move(o.exception_);
            finished_ = o.finished_;
            o.pid_ = -1;
            o.reaped_ = false;
            o.finished_ = false;
        }
        return *this;
    }

    Process(const Process&) = delete;
    Process& operator=(const Process&) = delete;

    ~Process() { teardown(); }

    /// Poll child exit without joining the capture reader.
    [[nodiscard]] result<bool> running() {
        if (finished_) {
            if (!result_) return unexpected(result_.error());
            return false;
        }
        if (reaped_) return false;
        if (pid_ <= 0) return unexpected(detail::no_child_code());
        int status = 0;
        pid_t r = detail::waitpid_retry(pid_, &status, WNOHANG);
        if (r == -1) {
            fail(detail::errno_code());
            return unexpected(result_.error());
        }
        if (r == 0) return true;
        status_ = status;
        reaped_ = true;
        pid_ = -1;
        return false;
    }

    /// Wait without killing on expiry; nonpositive durations only poll.
    [[nodiscard]] result<wait_status> wait_for(std::chrono::milliseconds budget) {
        if (finished_) {
            auto& completed = wait();
            if (!completed) return unexpected(completed.error());
            return wait_status::ready;
        }
        if (pid_ <= 0 && !reaped_) return unexpected(detail::no_child_code());
        const auto start = std::chrono::steady_clock::now();
        for (;;) {
            auto alive = running();
            if (!alive) return unexpected(alive.error());
            if (!*alive) {
                auto& r = wait();  // reaped already: finalizes the capture
                if (!r) return unexpected(r.error());
                return wait_status::ready;
            }
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start);
            if (budget <= elapsed) return wait_status::timeout;
            const auto left = budget - elapsed;
            detail::nap(left < detail::wait_poll_interval ? left : detail::wait_poll_interval);
        }
    }

    /// Wait and collect captured stderr; later calls return the cached result.
    result<RunResult>& wait() {
        if (!finished_) {
            if (!reaped_) {
                if (pid_ <= 0) fail(detail::no_child_code());
                else {
                    int status = 0;
                    if (detail::waitpid_retry(pid_, &status, 0) == -1) fail(detail::errno_code());
                    else {
                        status_ = status;
                        reaped_ = true;
                        pid_ = -1;
                    }
                }
            }
            if (!finished_) finalize();
        }
        if (cap_) {
            std::string discarded;
            collect_capture(discarded);
        }
        if (exception_) std::rethrow_exception(exception_);
        return result_;
    }

    /// SIGKILL and reap the direct child; descendants are not signalled.
    result<void> terminate() {
        if (!finished_ && pid_ <= 0 && !reaped_) return unexpected(detail::no_child_code());
        std::error_code kill_ec;
        if (pid_ > 0 && ::kill(pid_, SIGKILL) == -1 && errno != ESRCH) kill_ec = detail::errno_code();
        auto& r = wait();
        if (kill_ec) return unexpected(kill_ec);
        if (!r) return unexpected(r.error());
        return {};
    }


private:
    Process(pid_t pid, std::unique_ptr<detail::capture_state> cap, std::thread reader) noexcept
        : pid_(pid), cap_(std::move(cap)), reader_(std::move(reader)) {}

    friend result<Process> spawn(const std::vector<std::string>&, const RunOptions&, std::string_view);

    // Joining precedes access to the reader's state.
    void stop_and_join() noexcept {
        if (!reader_.joinable()) return;
        detail::request_stop(*cap_);
        reader_.join();
    }

    /// Hand the worker's buffer over after joining; report its failure, if any.
    std::error_code collect_capture(std::string& out) noexcept {
        stop_and_join();
        std::error_code ec;
        if (cap_) {
            ec = cap_->error;
            exception_ = cap_->exception;
            out = std::move(cap_->data);
            cap_.reset();
        }
        return ec;
    }

    /// Turn the reaped status plus the captured stderr into the cached result.
    void finalize() {
        RunResult res;
        if (WIFEXITED(status_)) {
            res.exit_code = WEXITSTATUS(status_);
        } else if (WIFSIGNALED(status_)) {
            res.signal = WTERMSIG(status_);
        }
        std::error_code ec = collect_capture(res.stderr_output);
        finished_ = true;
        if (ec) result_ = unexpected(ec);
        else result_ = std::move(res);
    }

    // Cache a waitpid error; wait() or destruction will join the reader.
    void fail(std::error_code ec) {
        pid_ = -1;
        finished_ = true;
        result_ = unexpected(ec);
    }

    // Destruction and move assignment wait without reporting errors.
    void teardown() noexcept {
        if (pid_ > 0) {
            int status = 0;
            detail::waitpid_retry(pid_, &status, 0);
            pid_ = -1;
        }
        stop_and_join();
        cap_.reset();
    }

    pid_t pid_ = -1;                                ///< > 0 while the child is unreaped
    int status_ = 0;                                ///< waitpid() status, valid once reaped_
    bool reaped_ = false;                           ///< child collected, capture not yet finalized
    std::unique_ptr<detail::capture_state> cap_;
    std::thread reader_;                            ///< joinable iff cap_ is set
    result<RunResult> result_;                      ///< valid once finished_
    std::exception_ptr exception_;
    bool finished_ = false;
};

inline result<Process> spawn(const std::vector<std::string>& args, const RunOptions& opts, std::string_view stage) {
    if (args.empty()) return unexpected(make_error_code(errc::empty_command));
    if (opts.on_spawn) opts.on_spawn(stage, format_command(args));
    else if (const auto& hook = default_spawn_hook()) hook(stage, format_command(args));

    // Exec-status pipe: CLOEXEC, so a successful exec closes it (EOF); a failure writes errno.
    detail::unique_fd exec_r, exec_w;
    if (!detail::make_cloexec_pipe(exec_r, exec_w)) return unexpected(detail::errno_code());

    // Allocate capture storage and close-on-exec pipes before fork.
    std::unique_ptr<detail::capture_state> cap;
    detail::unique_fd stderr_w;
    if (opts.capture_stderr) {
        cap = std::make_unique<detail::capture_state>();
        if (!detail::make_cloexec_pipe(cap->read_fd, stderr_w)) return unexpected(detail::errno_code());
        if (!detail::make_cloexec_pipe(cap->wake_r, cap->wake_w)) return unexpected(detail::errno_code());
    }

    detail::unique_fd stdout_fd;
    if (opts.stdout_file) {
        stdout_fd = detail::unique_fd(::open(opts.stdout_file->c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644));
        if (!stdout_fd) return unexpected(detail::errno_code());
    } else if (!opts.inherit_stdout) {
        stdout_fd = detail::unique_fd(::open("/dev/null", O_WRONLY | O_CLOEXEC));
        if (!stdout_fd) return unexpected(detail::errno_code());
    }

    if (!detail::reserve_above_stdio(exec_r) || !detail::reserve_above_stdio(exec_w) ||
        !detail::reserve_above_stdio(stderr_w) || !detail::reserve_above_stdio(stdout_fd))
        return unexpected(detail::errno_code());
    if (cap && (!detail::reserve_above_stdio(cap->read_fd) || !detail::reserve_above_stdio(cap->wake_r) ||
                !detail::reserve_above_stdio(cap->wake_w)))
        return unexpected(detail::errno_code());

    // Prepare argv before fork to avoid allocating in the child.
    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    const char* workdir = opts.workdir ? opts.workdir->c_str() : nullptr;
    const int exec_wfd = exec_w.fd;
    const int stderr_wfd = stderr_w.fd;
    const int stdout_wfd = stdout_fd.fd;

    pid_t pid = ::fork();
    if (pid == -1) return unexpected(detail::errno_code());

    if (pid == 0) {
        // Async-signal-safe only: no allocation, no locks, straight to exec.
        auto fail = [exec_wfd](int e) {
            ssize_t written;
            do {
                written = ::write(exec_wfd, &e, sizeof e);
            } while (written == -1 && errno == EINTR);
            ::_exit(127);
        };
        if (workdir && ::chdir(workdir) == -1) fail(errno);
        // Internal descriptors are above 2, separate from the dup2() destinations.
        if (stderr_wfd != -1) {
            if (::dup2(stderr_wfd, STDERR_FILENO) == -1) fail(errno);
            ::close(stderr_wfd);
        }
        if (stdout_wfd != -1) {
            if (::dup2(stdout_wfd, STDOUT_FILENO) == -1) fail(errno);
            ::close(stdout_wfd);
        }
        ::execvp(argv[0], argv.data());
        fail(errno);
    }

    // Parent.
    detail::child_reaper reaper(pid);
    exec_w.reset();
    stderr_w.reset();
    stdout_fd.reset();

    int child_errno = 0;
    {
        char buf[sizeof(int)];
        std::size_t have = 0;
        for (;;) {
            ssize_t n = ::read(exec_r.fd, buf + have, sizeof buf - have);
            if (n == -1) {
                if (errno == EINTR) continue;
                return unexpected(detail::errno_code());
            }
            if (n == 0) break;  // EOF: exec succeeded
            have += static_cast<std::size_t>(n);
            if (have == sizeof buf) break;
        }
        exec_r.reset();
        if (have != 0 && have != sizeof buf)
            return unexpected(std::make_error_code(std::errc::io_error));
        if (have == sizeof buf) std::memcpy(&child_errno, buf, sizeof child_errno);
    }
    if (child_errno != 0) return unexpected(std::error_code(child_errno, std::generic_category()));

    std::thread reader;
    if (cap) {
        try {
            reader = std::thread(detail::capture_worker, cap.get());
        } catch (const std::system_error& e) {
            cap->read_fd.reset();
            return unexpected(e.code());
        } catch (...) {
            cap->read_fd.reset();
            throw;
        }
    }

    reaper.release();  // the Process owns the child from here on
    return Process(pid, std::move(cap), std::move(reader));
}

/// Synchronous convenience: spawn() followed by wait().
[[nodiscard]] inline result<RunResult> run(const std::vector<std::string>& args,
                                           const RunOptions& opts = {},
                                           std::string_view stage = {}) {
    auto proc = spawn(args, opts, stage);
    if (!proc) return unexpected(proc.error());
    return std::move(proc->wait());
}

// Outcome: ordered checks and commands

enum class Expect : unsigned {
    EXISTS    = 0,       ///< readable regular file (always checked)
    NON_EMPTY = 1u << 0, ///< size > 0
    GZIPPED   = 1u << 1, ///< gzip magic bytes
};

[[nodiscard]] constexpr Expect operator|(Expect a, Expect b) noexcept {
    return static_cast<Expect>(static_cast<unsigned>(a) | static_cast<unsigned>(b));
}
[[nodiscard]] constexpr bool has_flag(Expect flags, Expect flag) noexcept {
    return (static_cast<unsigned>(flags) & static_cast<unsigned>(flag)) != 0;
}

/// Ordered asynchronous commands and checks; skip later operations after failure.
class Outcome {
public:
    Outcome() = default;
    explicit Outcome(std::string name) noexcept : name_(std::move(name)) {}
    Outcome(const Outcome&) = delete;
    Outcome& operator=(const Outcome&) = delete;
    Outcome(Outcome&&) noexcept = default;
    Outcome& operator=(Outcome&&) noexcept = default;

    /// Wait for the previous command, then start args without waiting for completion.
    Outcome& proc(const std::vector<std::string>& args, const RunOptions& opts = {}) & {
        if (!ok()) return *this;
        cmd_ = args.empty() ? std::string() : args.front();
        auto child = spawn(args, opts, name_);
        if (!child) fail("cannot execute: " + child.error().message());
        else pending_.emplace(std::move(*child));
        return *this;
    }
    Outcome&& proc(const std::vector<std::string>& args, const RunOptions& opts = {}) && {
        return std::move(proc(args, opts));
    }

    /// Wait for the previous command, then invoke on the caller's thread.
    template <class F, class... Args>
        requires std::is_invocable_r_v<int, F, Args...>
    Outcome& call(F&& fn, Args&&... args) & {
        if (!ok()) return *this;
        cmd_.clear();
        int status = std::invoke(std::forward<F>(fn), std::forward<Args>(args)...);
        if (status != 0) fail("function returned code " + std::to_string(status));
        return *this;
    }
    template <class F, class... Args>
        requires std::is_invocable_r_v<int, F, Args...>
    Outcome&& call(F&& fn, Args&&... args) && {
        return std::move(call(std::forward<F>(fn), std::forward<Args>(args)...));
    }

    /// Join all prerequisites, preserving the first failure in argument order.
    template <class... Stages>
        requires (std::is_same_v<Stages, Outcome> && ...)
    Outcome& after(const Stages&... prerequisites) & {
        finish();
        (prerequisites.finish(), ...);
        auto check = [this](const Outcome& prerequisite) {
            if (code_ != 0 || prerequisite.code_ == 0) return;
            const auto& label = prerequisite.name_.empty() ? prerequisite.cmd_ : prerequisite.name_;
            fail("prerequisite " + (label.empty() ? std::string("(anonymous)") : label) +
                 " failed: " + prerequisite.detail_);
        };
        (check(prerequisites), ...);
        return *this;
    }
    template <class... Stages>
        requires (std::is_same_v<Stages, Outcome> && ...)
    Outcome&& after(const Stages&... prerequisites) && {
        return std::move(after(prerequisites...));
    }

    /// Wait without a deadline, including after a failed expectation.
    Outcome& wait() & {
        finish();
        return *this;
    }
    Outcome&& wait() && { return std::move(wait()); }

    /// Expiry leaves the child running and does not record failure.
    [[nodiscard]] wait_status wait_for(std::chrono::milliseconds duration) {
        if (!pending_) return wait_status::ready;
        auto status = pending_->wait_for(duration);
        if (!status) {
            fail("cannot wait: " + status.error().message());
            finish();
            return wait_status::ready;
        }
        if (*status == wait_status::ready) finish();
        return *status;
    }

    /// Check child status without waiting or collecting captured output.
    [[nodiscard]] bool running() const {
        if (!pending_) return false;
        auto active = pending_->running();
        if (!active) {
            fail("cannot wait: " + active.error().message());
            return false;
        }
        return *active;
    }

    /// Record failure on expiry; use or_terminate() to stop the child.
    Outcome& expect_done_within(std::chrono::milliseconds duration) & {
        if (code_ == 0 && wait_for(duration) == wait_status::timeout)
            fail("did not finish within " + std::to_string(duration.count()) + " ms");
        return *this;
    }
    Outcome&& expect_done_within(std::chrono::milliseconds duration) && {
        return std::move(expect_done_within(duration));
    }

    /// Kill and reap this stage's child on recorded failure, without waiting first.
    Outcome& or_terminate() & {
        if (code_ != 0) terminate_pending();
        return *this;
    }
    Outcome&& or_terminate() && { return std::move(or_terminate()); }

    Outcome& expect_success(int exit_code) & {
        if (ok() && exit_code != 0) fail("exited with code " + std::to_string(exit_code));
        return *this;
    }
    Outcome&& expect_success(int exit_code) && { return std::move(expect_success(exit_code)); }

    Outcome& expect_which(std::string_view name) & {
        if (ok() && !which(name)) fail("executable not found: " + std::string(name));
        return *this;
    }
    Outcome&& expect_which(std::string_view name) && { return std::move(expect_which(name)); }

    /// Replace the detail text without changing the success/failure state.
    Outcome& with_error(std::string detail) & {
        if (code_ == 0) finish();
        detail_ = std::move(detail);
        return *this;
    }
    Outcome&& with_error(std::string detail) && { return std::move(with_error(std::move(detail))); }

    Outcome& with_stderr(std::string captured) & {
        if (code_ == 0) finish();
        stderr_ = std::move(captured);
        return *this;
    }
    Outcome&& with_stderr(std::string captured) && { return std::move(with_stderr(std::move(captured))); }

    /// The bool argument is evaluated before the call; use a callable to defer it.
    Outcome& expect(bool cond, std::string_view detail) & {
        if (ok() && !cond) fail(std::string(detail));
        return *this;
    }
    Outcome&& expect(bool cond, std::string_view detail) && {
        return std::move(expect(cond, detail));
    }

    template <class Pred>
        requires std::is_invocable_r_v<bool, Pred>
    Outcome& expect(Pred&& pred, std::string_view detail) & {
        if (ok() && !std::invoke(std::forward<Pred>(pred))) fail(std::string(detail));
        return *this;
    }
    template <class Pred>
        requires std::is_invocable_r_v<bool, Pred>
    Outcome&& expect(Pred&& pred, std::string_view detail) && {
        return std::move(expect(std::forward<Pred>(pred), detail));
    }

    Outcome& expect_file(const fs::path& p, Expect flags = Expect::EXISTS) & {
        if (!ok()) return *this;
        if (!file_readable(p)) fail("missing: " + p.string());
        else if (has_flag(flags, Expect::NON_EMPTY) && !file_non_empty(p)) fail("empty: " + p.string());
        else if (has_flag(flags, Expect::GZIPPED) && !file_is_gzipped(p)) fail("not gzipped: " + p.string());
        return *this;
    }
    Outcome&& expect_file(const fs::path& p, Expect flags = Expect::EXISTS) && {
        return std::move(expect_file(p, flags));
    }

    template <class Pred>
        requires std::is_invocable_r_v<bool, Pred, const fs::path&>
    Outcome& expect_file(const fs::path& p, Pred&& pred, std::string_view what) & {
        if (!ok()) return *this;
        if (!file_readable(p)) fail("missing: " + p.string());
        else if (!std::invoke(std::forward<Pred>(pred), p)) fail("not " + std::string(what) + ": " + p.string());
        return *this;
    }
    template <class Pred>
        requires std::is_invocable_r_v<bool, Pred, const fs::path&>
    Outcome&& expect_file(const fs::path& p, Pred&& pred, std::string_view what) && {
        return std::move(expect_file(p, std::forward<Pred>(pred), what));
    }

    template <class F>
        requires std::is_invocable_v<F>
    Outcome& or_execute(F&& fn) & {
        if (!ok()) std::invoke(std::forward<F>(fn));
        return *this;
    }
    template <class F>
        requires std::is_invocable_v<F>
    Outcome&& or_execute(F&& fn) && { return std::move(or_execute(std::forward<F>(fn))); }

    /// On fatal failure, reap this stage's child before exiting the caller.
    Outcome& or_die_if(bool condition, std::string_view stage = {}) & {
        if (ok()) return *this;
        if (condition) terminate_pending();
        if (stage.empty()) stage = !name_.empty() ? name_ : !cmd_.empty() ? cmd_ : std::string_view("command");
        if (condition) {
            std::fprintf(stderr, "\n[ERROR] %.*s failed", static_cast<int>(stage.size()), stage.data());
            if (!detail_.empty()) std::fprintf(stderr, " (%s)", detail_.c_str());
            std::fprintf(stderr, "\n");
            if (!stderr_.empty()) std::fprintf(stderr, "[ERROR] stderr:\n%s\n", stderr_.c_str());
            std::fflush(stderr);
            std::exit(EXIT_FAILURE);
        }
        std::fprintf(stderr, "\n[WARNING] %.*s failed", static_cast<int>(stage.size()), stage.data());
        if (!detail_.empty()) std::fprintf(stderr, " (%s)", detail_.c_str());
        std::fprintf(stderr, " — soft-fail active\n");
        return *this;
    }
    Outcome&& or_die_if(bool condition, std::string_view stage = {}) && {
        return std::move(or_die_if(condition, stage));
    }

    /// Final-result access waits unless a failure has already been recorded.
    [[nodiscard]] bool ok() const {
        if (code_ == 0) finish();
        return code_ == 0;
    }
    explicit operator bool() const { return ok(); }
    [[nodiscard]] int code() const {
        if (code_ == 0) finish();
        return code_;
    }
    [[nodiscard]] const std::string& name() const noexcept { return name_; }
    [[nodiscard]] const std::string& detail() const {
        if (code_ == 0) finish();
        return detail_;
    }
    [[nodiscard]] const std::string& stderr_output() const {
        if (code_ == 0) finish();
        return stderr_;
    }

private:
    void fail(std::string detail) const {
        if (code_ != 0) return;
        code_ = EXIT_FAILURE;
        detail_ = cmd_.empty() ? std::move(detail) : cmd_ + ": " + detail;
    }

    void finish() const {
        if (!pending_) return;
        auto& completed = pending_->wait();
        if (!completed) fail("cannot wait: " + completed.error().message());
        else {
            stderr_ = std::move(completed->stderr_output);
            if (!completed->success()) fail(completed->summary());
        }
        pending_.reset();
    }

    void terminate_pending() {
        if (!pending_) return;
        auto stopped = pending_->terminate();
        if (!stopped) detail_ += "; cannot terminate: " + stopped.error().message();
        finish();
    }

    mutable int code_ = 0;
    std::string name_;
    std::string cmd_;
    mutable std::string detail_;
    mutable std::string stderr_;
    mutable std::optional<Process> pending_;
};

/// Start a named stage.
[[nodiscard]] inline Outcome stage(std::string name) {
    return Outcome(std::move(name));
}

/// Anonymous single-command stage: proc(args, opts) == Outcome().proc(args, opts).
[[nodiscard]] inline Outcome proc(const std::vector<std::string>& args, const RunOptions& opts = {}) {
    Outcome outcome;
    outcome.proc(args, opts);
    return outcome;
}

}  // namespace shrn

#endif  // SHRN_HPP
