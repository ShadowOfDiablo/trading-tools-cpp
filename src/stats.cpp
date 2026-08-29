#include "stats.hpp"

#include "adfvalues.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <string_view>

namespace tb {
namespace {

double normal_cdf(double x) noexcept {
    // 0.5 * erfc(-x/sqrt(2)) is the stable form; erf(x/sqrt(2)) loses
    // precision in the far left tail, which is exactly where a very negative
    // ADF statistic lives.
    return 0.5 * std::erfc(-x * 0.70710678118654752440);
}

// Solves (A + small ridge if needed) z = b for a symmetric positive-definite A
// by Cholesky. Returns false if A is not positive definite, which for a normal
// -equations matrix means the design was rank deficient.
bool cholesky_solve(std::vector<double>& A, std::size_t n, std::vector<double>& b) {
    // In-place Cholesky: A = L L^T, lower triangle.
    for (std::size_t i = 0; i < n; ++i) {
        for (std::size_t j = 0; j <= i; ++j) {
            double sum = A[i * n + j];
            for (std::size_t k = 0; k < j; ++k) sum -= A[i * n + k] * A[j * n + k];
            if (i == j) {
                if (!(sum > 0.0)) return false;
                A[i * n + j] = std::sqrt(sum);
            } else {
                A[i * n + j] = sum / A[j * n + j];
            }
        }
    }
    // Forward substitution L y = b.
    for (std::size_t i = 0; i < n; ++i) {
        double sum = b[i];
        for (std::size_t k = 0; k < i; ++k) sum -= A[i * n + k] * b[k];
        b[i] = sum / A[i * n + i];
    }
    // Back substitution L^T x = y.
    for (std::size_t ii = n; ii-- > 0;) {
        double sum = b[ii];
        for (std::size_t k = ii + 1; k < n; ++k) sum -= A[k * n + ii] * b[k];
        b[ii] = sum / A[ii * n + ii];
    }
    return true;
}

// Inverse of a symmetric positive-definite matrix, needed only for the
// coefficient standard errors (the ADF statistic is a t-value, so the standard
// error is not optional here the way it often is).
bool spd_inverse(const std::vector<double>& A_in, std::size_t n, std::vector<double>& inv) {
    inv.assign(n * n, 0.0);
    std::vector<double> A = A_in;
    std::vector<double> col(n);
    // Factor once, then solve n unit vectors.
    std::vector<double> L = A;
    for (std::size_t i = 0; i < n; ++i) {
        for (std::size_t j = 0; j <= i; ++j) {
            double sum = L[i * n + j];
            for (std::size_t k = 0; k < j; ++k) sum -= L[i * n + k] * L[j * n + k];
            if (i == j) {
                if (!(sum > 0.0)) return false;
                L[i * n + j] = std::sqrt(sum);
            } else {
                L[i * n + j] = sum / L[j * n + j];
            }
        }
    }
    for (std::size_t c = 0; c < n; ++c) {
        std::fill(col.begin(), col.end(), 0.0);
        col[c] = 1.0;
        for (std::size_t i = 0; i < n; ++i) {
            double sum = col[i];
            for (std::size_t k = 0; k < i; ++k) sum -= L[i * n + k] * col[k];
            col[i] = sum / L[i * n + i];
        }
        for (std::size_t ii = n; ii-- > 0;) {
            double sum = col[ii];
            for (std::size_t k = ii + 1; k < n; ++k) sum -= L[k * n + ii] * col[k];
            col[ii] = sum / L[ii * n + ii];
        }
        for (std::size_t r = 0; r < n; ++r) inv[r * n + c] = col[r];
    }
    return true;
}

struct TrendSpec {
    int order;  // 'n' -> -1, 'c' -> 0, 'ct' -> 1, 'ctt' -> 2
    int ncols;  // deterministic columns added
};

TrendSpec trend_spec(const char* trend) {
    const std::string_view t{trend ? trend : "c"};
    if (t == "n") return {-1, 0};
    if (t == "c") return {0, 1};
    if (t == "ct") return {1, 2};
    if (t == "ctt") return {2, 3};
    return {0, 1};
}

int trend_index(const char* trend) {
    const std::string_view t{trend ? trend : "c"};
    if (t == "n") return 0;
    if (t == "c") return 1;
    if (t == "ct") return 2;
    return 3;  // ctt
}

// statsmodels' OLS AIC, up to a constant that is identical across the
// candidate models _autolag compares. Only the argmin matters, and every
// candidate is fitted on the same rows, so the dropped constant cannot change
// which lag is selected.
double aic_from_ssr(double ssr, std::size_t nobs, std::size_t k) {
    if (!(ssr > 0.0) || nobs == 0) return std::numeric_limits<double>::infinity();
    const double n = static_cast<double>(nobs);
    return n * std::log(ssr / n) + 2.0 * static_cast<double>(k);
}

}  // namespace

OlsResult ols(std::span<const double> y, std::span<const double> X, std::size_t ncols) {
    OlsResult r;
    if (ncols == 0) return r;
    const std::size_t n = y.size();
    if (n == 0 || X.size() != n * ncols || n <= ncols) return r;

    // Normal equations: (X'X) beta = X'y.
    std::vector<double> xtx(ncols * ncols, 0.0);
    std::vector<double> xty(ncols, 0.0);
    for (std::size_t i = 0; i < n; ++i) {
        const double* row = X.data() + i * ncols;
        for (std::size_t a = 0; a < ncols; ++a) {
            xty[a] += row[a] * y[i];
            for (std::size_t b = a; b < ncols; ++b) xtx[a * ncols + b] += row[a] * row[b];
        }
    }
    for (std::size_t a = 0; a < ncols; ++a)
        for (std::size_t b = 0; b < a; ++b) xtx[a * ncols + b] = xtx[b * ncols + a];

    const std::vector<double> xtx_copy = xtx;
    std::vector<double> beta = xty;
    if (!cholesky_solve(xtx, ncols, beta)) return r;

    r.coefficients = beta;
    r.residuals.resize(n);
    double ssr = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double* row = X.data() + i * ncols;
        double fit = 0.0;
        for (std::size_t a = 0; a < ncols; ++a) fit += row[a] * beta[a];
        r.residuals[i] = y[i] - fit;
        ssr += r.residuals[i] * r.residuals[i];
    }

    r.nobs = n;
    r.df_resid = n - ncols;
    r.sigma2 = ssr / static_cast<double>(r.df_resid);

    std::vector<double> inv;
    if (spd_inverse(xtx_copy, ncols, inv)) {
        r.standard_errors.resize(ncols);
        for (std::size_t a = 0; a < ncols; ++a) {
            const double var = r.sigma2 * inv[a * ncols + a];
            r.standard_errors[a] = var > 0.0 ? std::sqrt(var) : 0.0;
        }
    } else {
        r.standard_errors.assign(ncols, 0.0);
    }
    r.ok = true;
    return r;
}

double correlation(std::span<const double> a, std::span<const double> b) {
    const std::size_t n = std::min(a.size(), b.size());
    if (n < 2) return 0.0;
    double ma = 0.0, mb = 0.0;
    for (std::size_t i = 0; i < n; ++i) { ma += a[i]; mb += b[i]; }
    ma /= static_cast<double>(n);
    mb /= static_cast<double>(n);
    double sab = 0.0, saa = 0.0, sbb = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double da = a[i] - ma, db = b[i] - mb;
        sab += da * db;
        saa += da * da;
        sbb += db * db;
    }
    if (!(saa > 0.0) || !(sbb > 0.0)) return 0.0;
    return sab / std::sqrt(saa * sbb);
}

double mackinnon_pvalue(double stat, int n_variables, const char* trend) {
    using namespace adfvalues;
    const int ti = trend_index(trend);
    const int idx = n_variables - 1;
    if (idx < 0 || idx > 5) return std::numeric_limits<double>::quiet_NaN();

    const double* maxs[] = {tau_max_nc, tau_max_c, tau_max_ct, tau_max_ctt};
    const double* mins[] = {tau_min_nc, tau_min_c, tau_min_ct, tau_min_ctt};
    const double* stars[] = {tau_star_nc, tau_star_c, tau_star_ct, tau_star_ctt};

    if (stat > maxs[ti][idx]) return 1.0;
    if (stat < mins[ti][idx]) return 0.0;

    // Ascending-order polynomial coefficients; statsmodels evaluates
    // polyval(tau_coef[::-1], stat), i.e. c0 + c1*t + c2*t^2 (+ c3*t^3).
    const double* coefs = nullptr;
    std::size_t ncoef = 0;
    if (stat <= stars[ti][idx]) {
        const double(*smallps[])[3] = {tau_nc_smallp, tau_c_smallp, tau_ct_smallp, tau_ctt_smallp};
        coefs = smallps[ti][idx];
        ncoef = 3;
    } else {
        const double(*largeps[])[4] = {tau_nc_largep, tau_c_largep, tau_ct_largep, tau_ctt_largep};
        coefs = largeps[ti][idx];
        ncoef = 4;
    }
    double acc = 0.0;
    for (std::size_t i = ncoef; i-- > 0;) acc = acc * stat + coefs[i];
    return normal_cdf(acc);
}

AdfResult adfuller(std::span<const double> series, int lags, const char* trend) {
    AdfResult out;
    const std::size_t n = series.size();
    if (n < 4) return out;

    const auto spec = trend_spec(trend);
    const std::size_t ntrend = static_cast<std::size_t>(spec.ncols);

    // Δx, the regressand and the source of every lagged regressor.
    const std::size_t m = n - 1;
    std::vector<double> xdiff(m);
    for (std::size_t i = 0; i < m; ++i) xdiff[i] = series[i + 1] - series[i];

    // Schwert's rule, exactly as statsmodels applies it when maxlag is None.
    int maxlag = lags;
    if (maxlag < 0) {
        maxlag = static_cast<int>(std::ceil(12.0 * std::pow(static_cast<double>(n) / 100.0, 0.25)));
        const long limit = static_cast<long>(n / 2) - static_cast<long>(ntrend) - 1;
        if (limit < 0) return out;
        maxlag = static_cast<int>(std::min<long>(maxlag, limit));
        if (maxlag < 0) return out;
    }
    if (static_cast<std::size_t>(maxlag) + 2 >= m) return out;

    // Builds the design for a given lag count L:
    //   rows   = m - L
    //   col 0  = the LEVEL x_{t-1}          (statsmodels overwrites the first
    //            lagmat column with it)
    //   col j  = Δx_{t-j} for j = 1..L
    //   then the deterministic trend columns, prepended for the lag search to
    //   match add_trend(..., prepend=True).
    auto build = [&](int L, bool with_trend, std::vector<double>& X, std::vector<double>& y,
                     std::size_t& rows, std::size_t& ncols) {
        rows = m - static_cast<std::size_t>(L);
        const std::size_t nlagcols = static_cast<std::size_t>(L) + 1;
        ncols = nlagcols + (with_trend ? ntrend : 0);
        X.assign(rows * ncols, 0.0);
        y.resize(rows);
        for (std::size_t i = 0; i < rows; ++i) {
            double* row = X.data() + i * ncols;
            std::size_t c = 0;
            if (with_trend) {
                for (std::size_t d = 0; d < ntrend; ++d) {
                    // constant, then t, then t^2 as required by 'c'/'ct'/'ctt'.
                    row[c++] = (d == 0) ? 1.0 : std::pow(static_cast<double>(i + 1), static_cast<double>(d));
                }
            }
            row[c++] = series[static_cast<std::size_t>(L) + i];  // x_{t-1}
            for (int j = 1; j <= L; ++j) row[c++] = xdiff[i + static_cast<std::size_t>(L) - static_cast<std::size_t>(j)];
            y[i] = xdiff[static_cast<std::size_t>(L) + i];
        }
    };

    int usedlag = maxlag;
    if (lags < 0) {
        // _autolag: fit the SAME rows for every candidate (those available at
        // maxlag), adding one column at a time, and take the smallest AIC.
        std::vector<double> Xfull, yfull;
        std::size_t rows = 0, ncols = 0;
        build(maxlag, true, Xfull, yfull, rows, ncols);
        const std::size_t startlag = ncols - (static_cast<std::size_t>(maxlag) + 1) + 1;

        double best_ic = std::numeric_limits<double>::infinity();
        std::size_t best_k = startlag;
        std::vector<double> Xsub;
        for (std::size_t k = startlag; k <= startlag + static_cast<std::size_t>(maxlag); ++k) {
            Xsub.assign(rows * k, 0.0);
            for (std::size_t i = 0; i < rows; ++i)
                std::memcpy(Xsub.data() + i * k, Xfull.data() + i * ncols, k * sizeof(double));
            const OlsResult fit = ols(yfull, Xsub, k);
            if (!fit.ok) continue;
            double ssr = 0.0;
            for (double e : fit.residuals) ssr += e * e;
            const double ic = aic_from_ssr(ssr, rows, k);
            if (ic < best_ic) { best_ic = ic; best_k = k; }
        }
        usedlag = static_cast<int>(best_k - startlag);
    }

    std::vector<double> X, y;
    std::size_t rows = 0, ncols = 0;
    build(usedlag, spec.order >= 0, X, y, rows, ncols);
    const OlsResult fit = ols(y, X, ncols);
    if (!fit.ok) return out;

    // The statistic is the t-value on the LEVEL term, which sits after the
    // deterministic columns in this layout.
    const std::size_t level_col = (spec.order >= 0) ? ntrend : 0;
    if (fit.standard_errors.size() <= level_col || !(fit.standard_errors[level_col] > 0.0)) return out;

    out.stat = fit.coefficients[level_col] / fit.standard_errors[level_col];
    out.pvalue = mackinnon_pvalue(out.stat, 1, trend);
    out.used_lags = static_cast<std::size_t>(usedlag);
    out.nobs = rows;
    out.ok = true;
    return out;
}

CointResult coint(std::span<const double> y0, std::span<const double> y1) {
    CointResult out;
    const std::size_t n = std::min(y0.size(), y1.size());
    if (n < 10) return out;

    // Step 1: y0 = a + b*y1 + resid, with the constant LAST to match
    // add_trend(..., prepend=False).
    std::vector<double> X(n * 2);
    for (std::size_t i = 0; i < n; ++i) {
        X[i * 2 + 0] = y1[i];
        X[i * 2 + 1] = 1.0;
    }
    const OlsResult stage1 = ols(std::span<const double>(y0.data(), n), X, 2);
    if (!stage1.ok) return out;

    // statsmodels refuses the test when the two series are (almost) perfectly
    // colinear, because the residual is then numerical noise and the test
    // reports spurious cointegration with great confidence.
    double tss = 0.0, mean = 0.0, ssr = 0.0;
    for (std::size_t i = 0; i < n; ++i) mean += y0[i];
    mean /= static_cast<double>(n);
    for (std::size_t i = 0; i < n; ++i) {
        const double d = y0[i] - mean;
        tss += d * d;
        ssr += stage1.residuals[i] * stage1.residuals[i];
    }
    constexpr double kSqrtEps = 1.4901161193847656e-08;  // np.sqrt(np.finfo(float).eps)
    const double rsquared = (tss > 0.0) ? 1.0 - ssr / tss : 0.0;
    if (!(rsquared < 1.0 - 100.0 * kSqrtEps)) return out;

    // Step 2: ADF on the residual with NO deterministic term - the intercept
    // was already removed in step 1 - but scored against the two-variable
    // critical values, which are markedly stricter than ADF's own.
    const AdfResult adf = adfuller(stage1.residuals, -1, "n");
    if (!adf.ok) return out;

    out.stat = adf.stat;
    out.pvalue = mackinnon_pvalue(adf.stat, 2, "c");
    out.hedge_ratio = stage1.coefficients[0];
    out.intercept = stage1.coefficients[1];
    out.used_lags = adf.used_lags;
    out.ok = true;
    return out;
}

}  // namespace tb
