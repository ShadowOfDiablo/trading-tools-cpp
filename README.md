# trading-tools-cpp

Offline quantitative tools in C++ — fast, dependency-free, and validated against
`statsmodels` rather than against my own confidence.

Two tools live here:

- **`coint_scan`** — scans a directory of OHLCV bar CSVs for cointegrated pairs
  (Engle–Granger, with the ADF lag length chosen by AIC). Built for
  whole-universe scans, where the pair count is O(n²).
- **`backtest`** — replays a recorded signal stream against a portfolio
  simulation: position sizing, regime-based take-profit/stop-loss, a trailing
  stop, correlation-group caps, a total capital-at-risk cap and a daily
  drawdown kill switch.

Both are **advisory and offline**. Nothing here connects to a broker, places an
order, or needs credentials or network access. They read CSVs and write reports.

## Build

```bash
cmake -B build
cmake --build build
./build/tb_tests        # the full test suite
```

Requires only a C++20 compiler and CMake. **No external dependencies** — no
gtest, no Eigen, no Boost. CMake picks whatever compiler you have; GCC, Clang
and MSVC all work. On Windows with MinGW the binaries link `-static` so they run
from any shell, not only one with the compiler's `bin` on `PATH`.

## `coint_scan`

```bash
./build/coint_scan --dir /path/to/bars --interval 1h --out pairs.csv
./build/coint_scan --dir /path/to/bars --alpha 0.01 --threads 8
./build/coint_scan --help
```

Input files are named `<SYMBOL>__<interval>.csv` with a header of
`Datetime,open,high,low,close,volume` (`Date` is also accepted). Timestamps may
carry a UTC offset — `2021-08-30 00:00:00-04:00` and `2026-06-10T18:10:00Z` both
parse.

Output is a CSV of qualifying pairs sorted by p-value, with the correlation,
cointegration statistic, hedge ratio and the number of aligned bars used.

### Performance

Measured on 59 symbols with the correlation prefilter disabled so both sides do
identical work (12-core machine, ~2000 hourly bars per symbol):

| | pairs tested | time | per pair |
|---|---|---|---|
| Python (`statsmodels.coint`, 1 thread) | 1247 | 69.5 s | 55.8 ms |
| this, 1 thread | 1261 | 5.5 s | 4.4 ms |
| this, 12 threads | 1261 | 1.3 s | — |

**~12.7× per core**, and both find the same 100 qualifying pairs. A full
581-symbol scan — 168,490 pairs — completes in about a second with the
correlation prefilter enabled, against roughly 2.6 hours for the equivalent
single-threaded Python. That is the difference between a whole-universe scan
being something you run whenever you like and something whose scope you have to
restrict to make it tractable.

## `backtest`

```bash
./build/backtest --dir backtest_data --cash 10000
./build/backtest --dir backtest_data --cost 0.0005   # 5bps per side
./build/backtest --dir backtest_data --trailing 1    # override the recorded setting
./build/backtest --help
```

Reads `signals.csv` (one row per symbol per bar, already scored) and
`backtest_config.json` (the risk caps that run was recorded under) from `--dir`,
and writes a trade log and an equity curve.

**The split is the point: whatever owns the model produces the signals; this
only simulates the portfolio.** It never decides whether a symbol looks
attractive, so it cannot disagree with production about what the model said.
Reimplementing model inference in C++ was considered and rejected — it is a
large surface for silent divergence, and a backtest that quietly scores
differently from production is worse than no backtest at all.

The exporter that produces the signal stream lives with the model, not here.
Its contract is the CSV columns: `timestamp, symbol, t212, group, price,
confidence, threshold, qualifies, signal, tp_pct, sl_pct, sl_distance, risk,
risk_scale` (plus an optional `trend_sl_pct`). Columns are located by name, so
a reordered export cannot silently swap two of them.

### It reports why signals did not become trades

A backtest that says "197 trades" without saying that 13,475 signals were
refused, and by which rule, is not telling you what you need to know. Every run
breaks the blocked signals down by cause — already held, model not qualifying,
correlation-group cap, portfolio risk cap, insufficient size or cash, kill
switch. On a real six-symbol run this immediately showed that only two of the
six had statistically qualifying models, which is a fact about the models rather
than about the strategy, and is invisible in a headline return.

### Costs are charged on both sides, and matter more than they look

`--cost` is a fraction of notional charged on entry and exit, defaulting to zero
so a run reports gross behaviour unless costs are asked for. On the same real
six-symbol run, +0.70% gross became **-4.07%** at 5bps per side across 197
trades. A high-turnover strategy's entire edge can live inside its transaction
costs, so a gross-only backtest is worth very little on its own.

## Correctness

A cointegration p-value that is quietly a little wrong does not crash. It calls
unrelated series cointegrated, and every downstream decision inherits that
silently. So `statsmodels` is pinned as the oracle:

- **`src/adfvalues.hpp` is generated**, not transcribed, by
  `tools/gen_adfvalues.py` from `statsmodels/tsa/adfvalues.py`. These are
  MacKinnon's response-surface coefficients; one mistyped digit produces
  plausible, wrong p-values and no visible failure.
- **`tests/fixtures.hpp` is generated** by `tools/gen_fixtures.py` and holds
  three input series together with statsmodels' own answers for each. The AIC
  lag search selects 3, 0 and 1 lags across them, so the search itself is
  covered rather than one lucky path through it. The generator warns if a change
  ever collapses them onto the same selection.

Both generated headers are **committed**, so a fresh clone builds and self-tests
with no Python, numpy or statsmodels installed. Regenerate them only when the
algorithm or the statsmodels version changes.

Against real market data (not included here — see below), the twelve strongest
pairs from a full universe scan agreed with `statsmodels.coint` to **2.6e-8**
relative on the p-value, the remaining difference being output print precision.

## No market data in this repository

Every test series is synthetic and seeded. Bar data from the usual free
providers is licensed for personal, non-commercial use and grants no
redistribution rights, so none is committed here. Seeded synthetic series have
the added benefit of being reproducible by anyone who clones this, which
committed price history would not be.

Point `coint_scan --dir` at your own bars.

## Two details that are easy to get wrong

Both live in `src/bars.cpp`, and both fail *silently* rather than loudly, which
is why they are called out:

- **Timestamps are parsed with their UTC offset applied.** Bar files often carry
  local exchange offsets. Getting the sign wrong misaligns US against European
  symbols by hours and produces garbage spreads — with no error anywhere.
- **`align()` intersects on timestamps, never by position.** Two symbols on
  different exchanges have different holidays and different first-traded dates.
  Zipping their close columns by index drifts one row further out of alignment
  after every mismatched session, and the resulting spread then looks exactly
  like a slow trend — that is, like strong evidence of cointegration.

## Layout

```
src/bars.{hpp,cpp}    CSV loading, timestamp parsing, timestamp alignment
src/stats.{hpp,cpp}   OLS, correlation, ADF, Engle-Granger cointegration
src/portfolio.{hpp,cpp}  the portfolio rules: sizing, stops, caps, kill switch
src/coint_scan.cpp    the pair scanner
src/backtest.cpp      the backtest driver (CSV/JSON in, reports out)
tests/harness.hpp     ~70 lines of test harness, in place of a framework
tools/gen_*.py        the two generators (need numpy + statsmodels)
```

Bars are stored columnar (struct-of-arrays) because every consumer walks one or
two series end to end, which is what both the algorithms and the cache want.

## License

MIT — see [LICENSE](LICENSE).
