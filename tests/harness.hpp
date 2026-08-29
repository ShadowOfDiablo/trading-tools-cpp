#pragma once
//
// A ~70-line test harness instead of gtest. Zero external dependencies is the
// property that makes this build anywhere with nothing installed first, and
// pulling in a whole framework to run assertions would cost exactly that.
//
#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace tbtest {

struct Case {
    std::string name;
    std::function<void()> fn;
};

inline std::vector<Case>& registry() {
    static std::vector<Case> cases;
    return cases;
}

struct Registrar {
    Registrar(const char* name, std::function<void()> fn) { registry().push_back({name, std::move(fn)}); }
};

// Thrown by the check macros; caught per-case so one failure does not stop the
// rest of the suite from reporting.
struct Failure {
    std::string message;
};

inline void fail(const std::string& where, const std::string& what) { throw Failure{where + ": " + what}; }

// Failure messages report the offending values, and those are not always
// numbers (a symbol name, for instance), so this cannot just be std::to_string.
inline std::string to_text(const std::string& s) { return s; }
inline std::string to_text(const char* s) { return s ? s : "(null)"; }
inline std::string to_text(bool b) { return b ? "true" : "false"; }
template <typename T>
std::string to_text(const T& v) { return std::to_string(v); }

#define TB_STR2(x) #x
#define TB_STR(x) TB_STR2(x)
#define TB_WHERE (std::string(__FILE__) + ":" + TB_STR(__LINE__))

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) tbtest::fail(TB_WHERE, "expected " #cond);            \
    } while (0)

#define CHECK_EQ(a, b)                                                     \
    do {                                                                   \
        const auto _a = (a);                                               \
        const auto _b = (b);                                               \
        if (!(_a == _b))                                                   \
            tbtest::fail(TB_WHERE, std::string(#a " == " #b " (got ") +    \
                                       tbtest::to_text(_a) + " vs " +      \
                                       tbtest::to_text(_b) + ")");         \
    } while (0)

// Relative tolerance, because these are floating-point quantities compared
// against another implementation's output rather than against exact constants.
#define CHECK_NEAR(a, b, tol)                                              \
    do {                                                                   \
        const double _a = (a);                                             \
        const double _b = (b);                                             \
        const double _d = std::fabs(_a - _b);                              \
        const double _s = std::fmax(1.0, std::fmax(std::fabs(_a), std::fabs(_b))); \
        if (!(_d / _s <= (tol)))                                           \
            tbtest::fail(TB_WHERE, std::string(#a " ~= " #b " (got ") +    \
                                       std::to_string(_a) + " vs " +       \
                                       std::to_string(_b) + ", rel diff " + \
                                       std::to_string(_d / _s) + ")");     \
    } while (0)

#define CHECK_THROWS(expr)                                                 \
    do {                                                                   \
        bool _threw = false;                                               \
        try {                                                              \
            (void)(expr);                                                  \
        } catch (const tbtest::Failure&) {                                 \
            throw;                                                         \
        } catch (...) {                                                    \
            _threw = true;                                                 \
        }                                                                  \
        if (!_threw) tbtest::fail(TB_WHERE, #expr " did not throw");       \
    } while (0)

#define TEST(name)                                                         \
    static void name();                                                    \
    static tbtest::Registrar reg_##name(#name, name);                      \
    static void name()

}  // namespace tbtest
