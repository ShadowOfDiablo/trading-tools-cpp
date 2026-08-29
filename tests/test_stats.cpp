#include "harness.hpp"

#include "fixtures.hpp"
#include "stats.hpp"

#include <span>
#include <vector>

using namespace tb;
using namespace tb::fixtures;

namespace {
template <std::size_t N>
std::span<const double> sp(const double (&a)[N]) {
    return std::span<const double>(a, N);
}
}  // namespace

TEST(ols_matches_statsmodels) {
    const std::size_t n = std::size(ols_y);
    std::vector<double> X;
    X.reserve(n * 3);
    for (std::size_t i = 0; i < n; ++i) {
        X.push_back(1.0);
        X.push_back(ols_x1[i]);
        X.push_back(ols_x2[i]);
    }
    const OlsResult r = ols(sp(ols_y), X, 3);
    CHECK(r.ok);
    for (std::size_t i = 0; i < 3; ++i) {
        CHECK_NEAR(r.coefficients[i], ols_params[i], 1e-10);
        // Standard errors matter more here than usual: the ADF statistic is a
        // t-value, so an error in bse is an error in every p-value.
        CHECK_NEAR(r.standard_errors[i], ols_bse[i], 1e-10);
    }
    CHECK_NEAR(r.sigma2, ols_sigma2, 1e-10);
}

TEST(ols_reports_failure_on_a_singular_design) {
    // Two identical columns: no unique solution exists, and silently returning
    // some arbitrary one would produce a confident, meaningless hedge ratio.
    const std::vector<double> y{1, 2, 3, 4, 5};
    std::vector<double> X;
    for (int i = 0; i < 5; ++i) { X.push_back(1.0); X.push_back(1.0); }
    CHECK(!ols(y, X, 2).ok);
}

TEST(correlation_matches_numpy) {
    CHECK_NEAR(correlation(sp(cointegrated_y0), sp(cointegrated_y1)), cointegrated_correlation, 1e-12);
    CHECK_NEAR(correlation(sp(independent_y0), sp(independent_y1)), independent_correlation, 1e-12);
    CHECK_NEAR(correlation(sp(autocorrelated_y0), sp(autocorrelated_y1)), autocorrelated_correlation, 1e-12);
}

TEST(correlation_of_a_constant_series_is_zero_not_nan) {
    const std::vector<double> flat(50, 3.0);
    std::vector<double> ramp(50);
    for (std::size_t i = 0; i < ramp.size(); ++i) ramp[i] = static_cast<double>(i);
    CHECK_EQ(correlation(flat, ramp), 0.0);
}

TEST(mackinnon_pvalue_saturates_outside_the_tabulated_range) {
    // Beyond the table's ends statsmodels clamps rather than extrapolating.
    CHECK_EQ(mackinnon_pvalue(50.0, 1, "c"), 1.0);
    CHECK_EQ(mackinnon_pvalue(-500.0, 1, "c"), 0.0);
}

TEST(mackinnon_pvalue_is_stricter_for_two_variables) {
    // The whole reason a cointegration test cannot reuse ADF's own table: the
    // same statistic must be less significant once a hedge ratio was fitted.
    const double one = mackinnon_pvalue(-3.4, 1, "c");
    const double two = mackinnon_pvalue(-3.4, 2, "c");
    CHECK(two > one);
}

TEST(adf_on_the_cointegrating_residual_matches_statsmodels) {
    // Includes the autolag (AIC) search, which selects a different lag in each
    // of these three cases (3, 0 and 1) - so this covers the search, not just
    // one lucky path through it.
    struct Case { std::span<const double> y0, y1; double stat; int usedlag; };
    const Case cases[] = {
        {sp(cointegrated_y0), sp(cointegrated_y1), cointegrated_adf_stat, cointegrated_adf_usedlag},
        {sp(independent_y0), sp(independent_y1), independent_adf_stat, independent_adf_usedlag},
        {sp(autocorrelated_y0), sp(autocorrelated_y1), autocorrelated_adf_stat, autocorrelated_adf_usedlag},
    };
    for (const Case& c : cases) {
        const std::size_t n = c.y0.size();
        std::vector<double> X(n * 2);
        for (std::size_t i = 0; i < n; ++i) { X[i * 2] = c.y1[i]; X[i * 2 + 1] = 1.0; }
        const OlsResult stage1 = ols(c.y0, X, 2);
        CHECK(stage1.ok);
        const AdfResult a = adfuller(stage1.residuals, -1, "n");
        CHECK(a.ok);
        CHECK_EQ(static_cast<int>(a.used_lags), c.usedlag);
        CHECK_NEAR(a.stat, c.stat, 1e-9);
    }
}

TEST(coint_matches_statsmodels_on_every_fixture) {
    struct Case { std::span<const double> y0, y1; double stat, pvalue, hedge, intercept; };
    const Case cases[] = {
        {sp(cointegrated_y0), sp(cointegrated_y1), cointegrated_coint_stat,
         cointegrated_coint_pvalue, cointegrated_hedge_ratio, cointegrated_intercept},
        {sp(independent_y0), sp(independent_y1), independent_coint_stat,
         independent_coint_pvalue, independent_hedge_ratio, independent_intercept},
        {sp(autocorrelated_y0), sp(autocorrelated_y1), autocorrelated_coint_stat,
         autocorrelated_coint_pvalue, autocorrelated_hedge_ratio, autocorrelated_intercept},
    };
    for (const Case& c : cases) {
        const CointResult r = coint(c.y0, c.y1);
        CHECK(r.ok);
        CHECK_NEAR(r.stat, c.stat, 1e-9);
        CHECK_NEAR(r.pvalue, c.pvalue, 1e-9);
        CHECK_NEAR(r.hedge_ratio, c.hedge, 1e-10);
        CHECK_NEAR(r.intercept, c.intercept, 1e-10);
    }
}

TEST(coint_separates_cointegrated_from_independent_series) {
    // The property any caller actually depends on, stated independently of the
    // exact numbers: a real relationship clears a 0.05 alpha and two unrelated
    // random walks do not.
    CHECK(coint(sp(cointegrated_y0), sp(cointegrated_y1)).pvalue < 0.05);
    CHECK(coint(sp(independent_y0), sp(independent_y1)).pvalue > 0.05);
}

TEST(coint_refuses_perfectly_colinear_series) {
    // statsmodels warns and bails here; the residual is pure numerical noise
    // and the test would report spurious cointegration with high confidence.
    std::vector<double> a(200), b(200);
    for (std::size_t i = 0; i < a.size(); ++i) {
        a[i] = 100.0 + 0.37 * static_cast<double>(i);
        b[i] = 2.0 * a[i] + 5.0;
    }
    CHECK(!coint(a, b).ok);
}

TEST(coint_declines_a_series_too_short_to_test) {
    const std::vector<double> a{1, 2, 3}, b{2, 4, 6};
    CHECK(!coint(a, b).ok);
}
