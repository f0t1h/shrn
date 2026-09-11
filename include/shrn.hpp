/**
 * @file shrn.hpp
 * @brief shrn — run subcommands and verify their outputs.
 *
 * Single-header, C++20, POSIX. Optional zlib support (auto-detected via
 * __has_include, disable with -DSHRN_NO_ZLIB=1).
 *
 * Layers:
 *   - shrn::expected      std::expected when available, in-header polyfill otherwise
 *                         (force the polyfill with -DSHRN_FORCE_POLYFILL=1)
 *   - file queries        file_readable, file_is_gzipped, read_file, line_count, ...
 *   - temp files          make_temp_file (mkstemps), TempFile, TempDir
 *   - process             run(), which(), format_command()
 *   - Outcome             stage(name).expect_file(in).proc(cmd).expect_file(out).or_die_if(cond)
 *
 * Failures are reported as shrn::result<T> = expected<T, std::error_code>.
 * Only Outcome::or_die_if() writes to stderr / exits; everything else is silent.
 */

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
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
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

#if !defined(SHRN_FORCE_POLYFILL) && defined(__cpp_lib_expected) && __cpp_lib_expected >= 202202L
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

// ============================================================================
// expected
// ============================================================================

#if SHRN_HAS_STD_EXPECTED

template <class T, class E> using expected = std::expected<T, E>;
template <class E> using unexpected = std::unexpected<E>;
template <class E> using bad_expected_access = std::bad_expected_access<E>;
using unexpect_t = std::unexpect_t;
using std::unexpect;

#else  // polyfill

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
    constexpr expected& operator=(const expected&) = default;
    constexpr expected& operator=(expected&&) = default;

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
                 std::is_constructible_v<T, U> && std::is_assignable_v<T&, U>)
    constexpr expected& operator=(U&& u) {
        v_.template emplace<0>(std::forward<U>(u));
        return *this;
    }
    template <class G>
    constexpr expected& operator=(const unexpected<G>& u) {
        v_.template emplace<1>(u.error());
        return *this;
    }
    template <class G>
    constexpr expected& operator=(unexpected<G>&& u) {
        v_.template emplace<1>(std::move(u).error());
        return *this;
    }

    template <class... Args>
    constexpr T& emplace(Args&&... args) noexcept(std::is_nothrow_constructible_v<T, Args...>) {
        return v_.template emplace<0>(std::forward<Args>(args)...);
    }

    constexpr void swap(expected& other) noexcept(std::is_nothrow_swappable_v<std::variant<T, E>>) {
        v_.swap(other.v_);
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

    // --- monadic ---
    template <class F>
    constexpr auto and_then(F&& f) const& {
        using U = std::remove_cvref_t<std::invoke_result_t<F, const T&>>;
        static_assert(detail::is_expected<U>::value);
        if (has_value()) return std::invoke(std::forward<F>(f), **this);
        return U(unexpect, error());
    }
    template <class F>
    constexpr auto and_then(F&& f) && {
        using U = std::remove_cvref_t<std::invoke_result_t<F, T&&>>;
        static_assert(detail::is_expected<U>::value);
        if (has_value()) return std::invoke(std::forward<F>(f), std::move(**this));
        return U(unexpect, std::move(error()));
    }
    template <class F>
    constexpr auto transform(F&& f) const& {
        using U = std::remove_cv_t<std::invoke_result_t<F, const T&>>;
        if (!has_value()) return expected<U, E>(unexpect, error());
        if constexpr (std::is_void_v<U>) {
            std::invoke(std::forward<F>(f), **this);
            return expected<void, E>();
        } else {
            return expected<U, E>(std::in_place, std::invoke(std::forward<F>(f), **this));
        }
    }
    template <class F>
    constexpr auto transform(F&& f) && {
        using U = std::remove_cv_t<std::invoke_result_t<F, T&&>>;
        if (!has_value()) return expected<U, E>(unexpect, std::move(error()));
        if constexpr (std::is_void_v<U>) {
            std::invoke(std::forward<F>(f), std::move(**this));
            return expected<void, E>();
        } else {
            return expected<U, E>(std::in_place, std::invoke(std::forward<F>(f), std::move(**this)));
        }
    }
    template <class F>
    constexpr auto or_else(F&& f) const& {
        using G = std::remove_cvref_t<std::invoke_result_t<F, const E&>>;
        static_assert(detail::is_expected<G>::value);
        if (has_value()) return G(std::in_place, **this);
        return std::invoke(std::forward<F>(f), error());
    }
    template <class F>
    constexpr auto or_else(F&& f) && {
        using G = std::remove_cvref_t<std::invoke_result_t<F, E&&>>;
        static_assert(detail::is_expected<G>::value);
        if (has_value()) return G(std::in_place, std::move(**this));
        return std::invoke(std::forward<F>(f), std::move(error()));
    }
    template <class F>
    constexpr auto transform_error(F&& f) const& {
        using G = std::remove_cv_t<std::invoke_result_t<F, const E&>>;
        if (has_value()) return expected<T, G>(std::in_place, **this);
        return expected<T, G>(unexpect, std::invoke(std::forward<F>(f), error()));
    }
    template <class F>
    constexpr auto transform_error(F&& f) && {
        using G = std::remove_cv_t<std::invoke_result_t<F, E&&>>;
        if (has_value()) return expected<T, G>(std::in_place, std::move(**this));
        return expected<T, G>(unexpect, std::invoke(std::forward<F>(f), std::move(error())));
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
    std::variant<T, E> v_;
};

template <class E> class expected<void, E> {
    static_assert(!std::is_void_v<E>);

public:
    using value_type = void;
    using error_type = E;
    using unexpected_type = unexpected<E>;
    template <class U> using rebind = expected<U, E>;

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
    constexpr expected& operator=(const unexpected<G>& u) {
        e_.emplace(u.error());
        return *this;
    }
    template <class G>
    constexpr expected& operator=(unexpected<G>&& u) {
        e_.emplace(std::move(u).error());
        return *this;
    }

    constexpr void emplace() noexcept { e_.reset(); }
    constexpr void swap(expected& other) noexcept(std::is_nothrow_swappable_v<std::optional<E>>) {
        e_.swap(other.e_);
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

    template <class F>
    constexpr auto and_then(F&& f) const& {
        using U = std::remove_cvref_t<std::invoke_result_t<F>>;
        static_assert(detail::is_expected<U>::value);
        if (has_value()) return std::invoke(std::forward<F>(f));
        return U(unexpect, error());
    }
    template <class F>
    constexpr auto and_then(F&& f) && {
        using U = std::remove_cvref_t<std::invoke_result_t<F>>;
        static_assert(detail::is_expected<U>::value);
        if (has_value()) return std::invoke(std::forward<F>(f));
        return U(unexpect, std::move(error()));
    }
    template <class F>
    constexpr auto transform(F&& f) const& {
        using U = std::remove_cv_t<std::invoke_result_t<F>>;
        if (!has_value()) return expected<U, E>(unexpect, error());
        if constexpr (std::is_void_v<U>) {
            std::invoke(std::forward<F>(f));
            return expected<void, E>();
        } else {
            return expected<U, E>(std::in_place, std::invoke(std::forward<F>(f)));
        }
    }
    template <class F>
    constexpr auto transform(F&& f) && {
        using U = std::remove_cv_t<std::invoke_result_t<F>>;
        if (!has_value()) return expected<U, E>(unexpect, std::move(error()));
        if constexpr (std::is_void_v<U>) {
            std::invoke(std::forward<F>(f));
            return expected<void, E>();
        } else {
            return expected<U, E>(std::in_place, std::invoke(std::forward<F>(f)));
        }
    }
    template <class F>
    constexpr auto or_else(F&& f) const& {
        using G = std::remove_cvref_t<std::invoke_result_t<F, const E&>>;
        static_assert(detail::is_expected<G>::value);
        if (has_value()) return G();
        return std::invoke(std::forward<F>(f), error());
    }
    template <class F>
    constexpr auto or_else(F&& f) && {
        using G = std::remove_cvref_t<std::invoke_result_t<F, E&&>>;
        static_assert(detail::is_expected<G>::value);
        if (has_value()) return G();
        return std::invoke(std::forward<F>(f), std::move(error()));
    }
    template <class F>
    constexpr auto transform_error(F&& f) const& {
        using G = std::remove_cv_t<std::invoke_result_t<F, const E&>>;
        if (has_value()) return expected<void, G>();
        return expected<void, G>(unexpect, std::invoke(std::forward<F>(f), error()));
    }
    template <class F>
    constexpr auto transform_error(F&& f) && {
        using G = std::remove_cv_t<std::invoke_result_t<F, E&&>>;
        if (has_value()) return expected<void, G>();
        return expected<void, G>(unexpect, std::invoke(std::forward<F>(f), std::move(error())));
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
    std::optional<E> e_;
};

#endif  // SHRN_HAS_STD_EXPECTED

template <class T> using result = expected<T, std::error_code>;

// ============================================================================
// Error codes
// ============================================================================

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

// ============================================================================
// File queries
// ============================================================================

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
            return out;
        }
    }
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    if (in.bad()) return unexpected(detail::errno_code());
    return out;
}

/// First content byte, decompressing gzip transparently. errc::empty_file when there is none.
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
    decompress,  ///< inflate gzip inputs (plain inputs pass through) into a plain output
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
#else
            return unexpected(make_error_code(errc::zlib_unavailable));
#endif
        } else {
            std::ifstream in(input, std::ios::binary);
            if (!in) return unexpected(detail::errno_code(std::errc::no_such_file_or_directory));
            while (in.read(buffer, sizeof buffer) || in.gcount() > 0) {
                out.write(buffer, in.gcount());
                if (!out) return unexpected(detail::errno_code());
            }
            if (in.bad()) return unexpected(detail::errno_code());
        }
    }
    out.close();
    if (!out) return unexpected(detail::errno_code());
    return {};
}

// ============================================================================
// Temporary files
// ============================================================================

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

/// make_temp_file_in(temp_directory(), prefix, suffix)
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

    /// Delete now.
    void reset() noexcept {
        if (!path_.empty()) {
            std::error_code ec;
            fs::remove(path_, ec);
            path_.clear();
        }
    }
    /// Give up ownership; the file is kept.
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

// ============================================================================
// Process execution
// ============================================================================

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
    if (name.find('/') != std::string_view::npos) {
        fs::path p(name);
        if (::access(p.c_str(), X_OK) == 0) return p;
        return std::nullopt;
    }
    const char* path_env = std::getenv("PATH");
    if (!path_env) return std::nullopt;
    std::string_view path(path_env);
    while (!path.empty()) {
        auto sep = path.find(':');
        std::string_view dir = path.substr(0, sep);
        fs::path candidate = fs::path(dir.empty() ? std::string_view(".") : dir) / name;
        if (::access(candidate.c_str(), X_OK) == 0) return candidate;
        if (sep == std::string_view::npos) break;
        path.remove_prefix(sep + 1);
    }
    return std::nullopt;
}

/// Observer for commands about to be spawned: (stage name, formatted command).
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
    std::optional<fs::path> stdout_file;          ///< redirect stdout (created/truncated, 0644)
    std::chrono::milliseconds timeout{0};         ///< 0 = none; on expiry the child gets SIGKILL
    SpawnHook on_spawn;                           ///< overrides default_spawn_hook() for this run
};

struct RunResult {
    int exit_code = -1;        ///< WEXITSTATUS, or -1 if killed by a signal
    int signal = 0;            ///< WTERMSIG, 0 if exited normally
    bool timed_out = false;    ///< killed by run() because RunOptions::timeout elapsed
    std::string stderr_output; ///< captured stderr (empty unless capture_stderr)

    [[nodiscard]] bool success() const noexcept { return exit_code == 0 && signal == 0 && !timed_out; }

    /// "exited with code N" / "killed by signal N (SIGKILL ...)" / "timed out ..."
    [[nodiscard]] std::string summary() const {
        if (success()) return "success";
        if (timed_out) return "timed out; killed by signal " + std::to_string(signal);
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

namespace detail {

inline bool set_cloexec(int fd) noexcept {
    int flags = ::fcntl(fd, F_GETFD);
    return flags != -1 && ::fcntl(fd, F_SETFD, flags | FD_CLOEXEC) != -1;
}

inline pid_t waitpid_retry(pid_t pid, int* status, int options) noexcept {
    pid_t r;
    do {
        r = ::waitpid(pid, status, options);
    } while (r == -1 && errno == EINTR);
    return r;
}

inline int remaining_ms(std::chrono::steady_clock::time_point deadline) noexcept {
    auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
    return left <= 0 ? 0 : static_cast<int>(left > 0x7fffffff ? 0x7fffffff : left);
}

}  // namespace detail

/**
 * Fork/exec `args` (PATH search, environment inherited) and wait.
 *
 * The error path covers failures to *launch* (pipe/fork/chdir/exec errno).
 * A child that runs and fails is a successful `run()` with `!result->success()`.
 * `stage` is only forwarded to the spawn hook.
 */
[[nodiscard]] inline result<RunResult> run(const std::vector<std::string>& args,
                                           const RunOptions& opts = {},
                                           std::string_view stage = {}) {
    if (args.empty()) return unexpected(make_error_code(errc::empty_command));
    if (opts.on_spawn) opts.on_spawn(stage, format_command(args));
    else if (const auto& hook = default_spawn_hook()) hook(stage, format_command(args));

    // Exec-status pipe: CLOEXEC, so a successful exec closes it (EOF); a failure writes errno.
    int raw[2];
    if (::pipe(raw) == -1) return unexpected(detail::errno_code());
    detail::unique_fd err_r(raw[0]), err_w(raw[1]);
    if (!detail::set_cloexec(err_r.fd) || !detail::set_cloexec(err_w.fd)) return unexpected(detail::errno_code());

    detail::unique_fd stderr_r, stderr_w;
    if (opts.capture_stderr) {
        if (::pipe(raw) == -1) return unexpected(detail::errno_code());
        stderr_r = detail::unique_fd(raw[0]);
        stderr_w = detail::unique_fd(raw[1]);
        detail::set_cloexec(stderr_r.fd);
    }

    detail::unique_fd stdout_fd;
    if (opts.stdout_file) {
        stdout_fd = detail::unique_fd(::open(opts.stdout_file->c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644));
        if (!stdout_fd) return unexpected(detail::errno_code());
    } else if (!opts.inherit_stdout) {
        stdout_fd = detail::unique_fd(::open("/dev/null", O_WRONLY));
        if (!stdout_fd) return unexpected(detail::errno_code());
    }

    // Build argv before fork: no allocation in the child.
    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    const char* workdir = opts.workdir ? opts.workdir->c_str() : nullptr;

    pid_t pid = ::fork();
    if (pid == -1) return unexpected(detail::errno_code());

    if (pid == 0) {
        auto fail = [&](int e) {
            ssize_t ignored = ::write(err_w.fd, &e, sizeof e);
            (void) ignored;
            ::_exit(127);
        };
        if (workdir && ::chdir(workdir) == -1) fail(errno);
        if (stderr_w && ::dup2(stderr_w.fd, STDERR_FILENO) == -1) fail(errno);
        if (stdout_fd && ::dup2(stdout_fd.fd, STDOUT_FILENO) == -1) fail(errno);
        ::execvp(argv[0], argv.data());
        fail(errno);
    }

    // Parent.
    err_w.reset();
    stderr_w.reset();
    stdout_fd.reset();

    int child_errno = 0;
    ssize_t n;
    do {
        n = ::read(err_r.fd, &child_errno, sizeof child_errno);
    } while (n == -1 && errno == EINTR);
    err_r.reset();
    if (n == static_cast<ssize_t>(sizeof child_errno)) {
        int status;
        detail::waitpid_retry(pid, &status, 0);
        return unexpected(std::error_code(child_errno, std::generic_category()));
    }

    RunResult res;
    const bool has_deadline = opts.timeout.count() > 0;
    const auto deadline = std::chrono::steady_clock::now() + opts.timeout;
    bool killed = false;
    auto kill_child = [&] {
        if (!killed) {
            ::kill(pid, SIGKILL);
            killed = true;
        }
    };

    if (stderr_r) {
        char buffer[1 << 14];
        pollfd pfd{stderr_r.fd, POLLIN, 0};
        for (;;) {
            int timeout_ms = has_deadline ? detail::remaining_ms(deadline) : -1;
            if (has_deadline && timeout_ms == 0 && !killed) kill_child();
            int pr = ::poll(&pfd, 1, killed ? -1 : timeout_ms);
            if (pr == -1) {
                if (errno == EINTR) continue;
                break;
            }
            if (pr == 0) {
                kill_child();
                continue;
            }
            ssize_t got = ::read(stderr_r.fd, buffer, sizeof buffer);
            if (got > 0) {
                res.stderr_output.append(buffer, static_cast<std::size_t>(got));
                continue;
            }
            if (got == -1 && errno == EINTR) continue;
            break;  // EOF or error
        }
        stderr_r.reset();
    } else if (has_deadline) {
        // Nothing to drain: watch the child's state (without reaping) until it exits or the deadline passes.
        siginfo_t info;
        for (;;) {
            info.si_pid = 0;
            int r = ::waitid(P_PID, static_cast<id_t>(pid), &info, WEXITED | WNOHANG | WNOWAIT);
            if (r == -1 && errno == EINTR) continue;
            if (r == -1 || info.si_pid == pid) break;  // error, or exited (reaped below)
            if (detail::remaining_ms(deadline) == 0) {
                kill_child();
                break;
            }
            timespec ts{0, 5'000'000};  // 5 ms
            ::nanosleep(&ts, nullptr);
        }
    }

    int status;
    if (detail::waitpid_retry(pid, &status, 0) == -1) return unexpected(detail::errno_code());
    if (WIFEXITED(status)) {
        res.exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        res.signal = WTERMSIG(status);
    }
    res.timed_out = killed;
    return res;
}

// ============================================================================
// Outcome: a stage as a timeline of checks and commands
// ============================================================================

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

/**
 * A stage: checks and commands in the order they happen. The first failure
 * short-circuits everything after it (later commands are not spawned), so
 * `detail()` names the earliest problem. Once a command has run, details are
 * prefixed with its argv[0].
 *
 *   stage("minimap2 alignment")
 *       .expect_which("minimap2")                       // pre-run
 *       .expect_file(ref, Expect::NON_EMPTY)
 *       .expect_file(reads, is_fastq, "FASTQ")
 *       .proc({"minimap2", "-x", "map-ont", ref, reads}, opts)
 *       .expect_file(paf, Expect::NON_EMPTY)           // post-run
 *       .or_die_if(!soft_fail)                         // reports under the stage name
 *       .or_execute([&] { recover(); });
 */
class Outcome {
public:
    Outcome() = default;
    explicit Outcome(std::string name) noexcept : name_(std::move(name)) {}

    /// Spawn `args` unless the stage already failed; merges exit status and stderr.
    Outcome& proc(const std::vector<std::string>& args, const RunOptions& opts = {}) {
        if (!ok()) return *this;
        cmd_ = args.empty() ? std::string() : args.front();
        auto r = run(args, opts, name_);
        if (!r) return fail("cannot execute: " + r.error().message());
        stderr_ = std::move(r->stderr_output);
        if (!r->success()) return fail(r->summary());
        return *this;
    }

    /// Merge the exit code of an in-process subprogram (0 = success).
    Outcome& expect_success(int exit_code) {
        if (ok() && exit_code != 0) fail("exited with code " + std::to_string(exit_code));
        return *this;
    }

    /// Fail with "executable not found: <name>" unless which(name) resolves.
    Outcome& expect_which(std::string_view name) {
        if (ok() && !which(name)) fail("executable not found: " + std::string(name));
        return *this;
    }

    Outcome& with_error(std::string detail) {
        detail_ = std::move(detail);
        return *this;
    }
    Outcome& with_stderr(std::string captured) {
        stderr_ = std::move(captured);
        return *this;
    }

    /// Fail unless `cond`; `detail` is recorded verbatim.
    Outcome& expect(bool cond, std::string_view detail) {
        if (ok() && !cond) fail(std::string(detail));
        return *this;
    }

    Outcome& expect_file(const fs::path& p, Expect flags = Expect::EXISTS) {
        if (!ok()) return *this;
        if (!file_readable(p)) return fail("missing: " + p.string());
        if (has_flag(flags, Expect::NON_EMPTY) && !file_non_empty(p)) return fail("empty: " + p.string());
        if (has_flag(flags, Expect::GZIPPED) && !file_is_gzipped(p)) return fail("not gzipped: " + p.string());
        return *this;
    }

    /// Fail with "not <what>: <path>" unless the file is readable and `pred(p)` holds.
    template <class Pred>
        requires std::is_invocable_r_v<bool, Pred, const fs::path&>
    Outcome& expect_file(const fs::path& p, Pred&& pred, std::string_view what) {
        if (!ok()) return *this;
        if (!file_readable(p)) return fail("missing: " + p.string());
        if (!std::invoke(std::forward<Pred>(pred), p)) return fail("not " + std::string(what) + ": " + p.string());
        return *this;
    }

    /// Run `fn()` on failure; chain continues.
    template <class F>
        requires std::is_invocable_v<F>
    Outcome& or_execute(F&& fn) {
        if (!ok()) std::invoke(std::forward<F>(fn));
        return *this;
    }

    /**
     * On failure: report to stderr, then exit(EXIT_FAILURE) if `condition`,
     * otherwise warn that soft-fail is active and continue the chain.
     * `stage` defaults to the stage name, then the last command, then "command".
     */
    Outcome& or_die_if(bool condition, std::string_view stage = {}) {
        if (ok()) return *this;
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

    [[nodiscard]] bool ok() const noexcept { return code_ == 0; }
    explicit operator bool() const noexcept { return ok(); }
    [[nodiscard]] int code() const noexcept { return code_; }
    [[nodiscard]] const std::string& name() const noexcept { return name_; }
    [[nodiscard]] const std::string& detail() const noexcept { return detail_; }
    [[nodiscard]] const std::string& stderr_output() const noexcept { return stderr_; }

private:
    Outcome& fail(std::string detail) {
        code_ = EXIT_FAILURE;
        detail_ = cmd_.empty() ? std::move(detail) : cmd_ + ": " + detail;
        return *this;
    }

    int code_ = 0;
    std::string name_;
    std::string cmd_;  ///< argv[0] of the most recent proc; prefixes later details
    std::string detail_;
    std::string stderr_;
};

/// Start a named stage.
[[nodiscard]] inline Outcome stage(std::string name) {
    return Outcome(std::move(name));
}

/// Anonymous single-command stage: proc(args, opts) == Outcome().proc(args, opts).
[[nodiscard]] inline Outcome proc(const std::vector<std::string>& args, const RunOptions& opts = {}) {
    return Outcome().proc(args, opts);
}

}  // namespace shrn

#endif  // SHRN_HPP
