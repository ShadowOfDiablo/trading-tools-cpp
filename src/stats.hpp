#pragma once
//
// The numerics behind the cointegration scanner: OLS, the Augmented
// Dickey-Fuller test, and Engle-Granger two-step cointegration.
//
// The reference implementation is statsmodels.tsa.stattools.coint. This is a
// reimplementation of the same procedure, NOT an approximation of it: a
// cointegration p-value that is quietly a little wrong does not crash, it just
// calls unrelated series cointegrated, and every decision downstream inherits
// that silently. A statistical gate that looks like it is doing its job and is
// not is worse than no gate at all - so every function here is checked against
// statsmodels' own output in the test suite rather than being eyeballed.
//
#include <cstddef>
#include <span>
#include <vector>

namespace tb {

// Ordinary least squares of y on X (X already includes any intercept column
// the caller wants, as a column of ones). Solved by forming the normal
// equations and Cholesky-decomposing them: the designs here are tiny (at most
// a handful of columns) and well conditioned after the caller's own
// differencing, so the numerical fragility that normally argues for QR does
// not bite - and this keeps the file dependency-free.
struct OlsResult {
    std::vector<double> coefficients;
    std::vector<double> residuals;
    std::vector<double> standard_errors;
    double sigma2 = 0.0;      // residual variance, with the df correction
    std::size_t nobs = 0;
    std::size_t df_resid = 0;
    bool ok = false;          // false if the design was singular or rank-deficient
};

// X is row-major: X[i * ncols + j] is row i, column j.
OlsResult ols(std::span<const double> y, std::span<const double> X, std::size_t ncols);

// Pearson correlation. Returns 0 if either series is constant.
double correlation(std::span<const double> a, std::span<const double> b);

// Augmented Dickey-Fuller test statistic and p-value.
//
// `lags` is the number of lagged differences included. `trend` selects the
// deterministic terms: 'n' none, 'c' constant, 'ct' constant and linear trend.
// The p-value is interpolated from MacKinnon's response-surface tables, which
// is what statsmodels does; see mackinnon_pvalue below.
struct AdfResult {
    double stat = 0.0;
    double pvalue = 1.0;
    std::size_t used_lags = 0;
    std::size_t nobs = 0;
    bool ok = false;
};
AdfResult adfuller(std::span<const double> series, int lags, const char* trend = "c");

// MacKinnon (1994/2010) approximate p-value for a unit-root test statistic.
// `n_variables` is 1 for a plain ADF test, 2 for a two-variable Engle-Granger
// cointegration test, and so on - the critical values shift substantially with
// it, which is exactly why a cointegration test cannot just reuse ADF's own
// table.
double mackinnon_pvalue(double stat, int n_variables, const char* trend = "c");

// Engle-Granger two-step cointegration test, matching
// statsmodels.tsa.stattools.coint(y0, y1, trend='c', method='aeg',
// maxlag=None, autolag='aic').
//
// Step 1: regress y0 on [1, y1] by OLS.
// Step 2: ADF-test the residual for a unit root, with no deterministic term
//         (the intercept was already removed in step 1) and the critical
//         values for 2 variables.
//
// Returns a p-value; small means "the residual is stationary", i.e. the two
// series are cointegrated. `hedge_ratio` is the step-1 slope - the ratio a
// spread between the two would be built on.
struct CointResult {
    double stat = 0.0;
    double pvalue = 1.0;
    double hedge_ratio = 0.0;
    double intercept = 0.0;
    std::size_t used_lags = 0;
    bool ok = false;
};
CointResult coint(std::span<const double> y0, std::span<const double> y1);

}  // namespace tb
