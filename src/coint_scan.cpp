// Cointegration scanner: finds tradable pairs across the whole universe.
//
// Why this exists as a separate C++ tool rather than more Python: the search is
// O(n^2) in the universe size, and statsmodels' coint() costs ~55ms on 2000
// bars. Across 574 symbols that is 164,451 pairs, or about 2.5 core-hours of
// pure cointegration testing before any prefilter. That is exactly the shape of
// work C++ is for - a tight numeric loop over data already on disk, with no
// network anywhere in it.
//
// This is ADVISORY. It writes a report; it never places an order, opens a
// network connection, or reads a credential. What you do with the pairs it
// finds is entirely outside this program.
//
#include "bars.hpp"
#include "stats.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using namespace tb;

namespace {

struct Options {
    std::string dir = "data_archive";
    std::string interval;             // empty = whatever suffix the files carry
    std::string out = "coint_pairs.csv";
    double min_correlation = 0.7;     // cheap prefilter, not the gate
    double alpha = 0.05;              // the actual significance gate
    std::size_t min_bars = 250;
    std::size_t max_bars = 2000;
    unsigned threads = 0;             // 0 = hardware concurrency
    bool help = false;
};

void usage() {
    std::printf(
        "coint_scan - scan a bar directory for cointegrated pairs (advisory; places no orders)\n"
        "\n"
        "Usage: coint_scan [options]\n"
        "  --dir PATH           directory of <SYMBOL>__<interval>.csv files (default data_archive)\n"
        "  --interval NAME      only files with this interval suffix, e.g. 5m, 1h, 1d\n"
        "  --out PATH           output CSV (default coint_pairs.csv)\n"
        "  --min-correlation X  cheap prefilter on return correlation (default 0.70)\n"
        "  --alpha X            cointegration significance level (default 0.05)\n"
        "  --min-bars N         skip symbols with fewer aligned bars (default 250)\n"
        "  --max-bars N         use only the most recent N aligned bars (default 2000)\n"
        "  --threads N          worker threads (default: all cores)\n"
        "  -h, --help           this text\n"
        "\n"
        "The correlation prefilter is NOT the gate - it only decides which pairs are\n"
        "worth paying for a cointegration test. A pair still has to clear --alpha.\n");
}

bool parse_args(int argc, char** argv, Options& o) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "%s needs a value\n", name);
                return nullptr;
            }
            return argv[++i];
        };
        if (a == "-h" || a == "--help") { o.help = true; return true; }
        else if (a == "--dir") { const char* v = next("--dir"); if (!v) return false; o.dir = v; }
        else if (a == "--interval") { const char* v = next("--interval"); if (!v) return false; o.interval = v; }
        else if (a == "--out") { const char* v = next("--out"); if (!v) return false; o.out = v; }
        else if (a == "--min-correlation") { const char* v = next("--min-correlation"); if (!v) return false; o.min_correlation = std::atof(v); }
        else if (a == "--alpha") { const char* v = next("--alpha"); if (!v) return false; o.alpha = std::atof(v); }
        else if (a == "--min-bars") { const char* v = next("--min-bars"); if (!v) return false; o.min_bars = static_cast<std::size_t>(std::atol(v)); }
        else if (a == "--max-bars") { const char* v = next("--max-bars"); if (!v) return false; o.max_bars = static_cast<std::size_t>(std::atol(v)); }
        else if (a == "--threads") { const char* v = next("--threads"); if (!v) return false; o.threads = static_cast<unsigned>(std::atol(v)); }
        else { std::fprintf(stderr, "unknown argument: %s\n", a.c_str()); return false; }
    }
    return true;
}

struct Symbol {
    std::string name;
    Bars bars;
    std::vector<double> log_close;
    std::vector<double> returns;  // for the cheap correlation prefilter
};

struct Hit {
    std::string a, b;
    double correlation, pvalue, stat, hedge_ratio;
    std::size_t nobs, used_lags;
};

// "AAPL__5m.csv" -> ("AAPL", "5m"). Matches historical_cache._safe_filename.
bool split_name(const std::string& stem, std::string& symbol, std::string& interval) {
    const auto pos = stem.rfind("__");
    if (pos == std::string::npos) return false;
    symbol = stem.substr(0, pos);
    interval = stem.substr(pos + 2);
    return !symbol.empty() && !interval.empty();
}

}  // namespace

int main(int argc, char** argv) {
    Options opt;
    if (!parse_args(argc, argv, opt)) return 2;
    if (opt.help) { usage(); return 0; }

    if (!fs::is_directory(opt.dir)) {
        std::fprintf(stderr, "not a directory: %s\n", opt.dir.c_str());
        return 2;
    }

    // ── Load ──────────────────────────────────────────────────────────────
    std::vector<Symbol> symbols;
    std::size_t skipped_short = 0, skipped_bad = 0;
    for (const auto& entry : fs::directory_iterator(opt.dir)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".csv") continue;
        std::string name, interval;
        if (!split_name(entry.path().stem().string(), name, interval)) continue;
        if (!opt.interval.empty() && interval != opt.interval) continue;

        Symbol s;
        s.name = name;
        try {
            s.bars = load_csv(entry.path().string(), name);
        } catch (const BarParseError& e) {
            // One malformed file must not abort a scan of 576.
            std::fprintf(stderr, "skipping %s: %s\n", name.c_str(), e.what());
            ++skipped_bad;
            continue;
        }
        if (s.bars.size() < opt.min_bars) { ++skipped_short; continue; }
        symbols.push_back(std::move(s));
    }

    if (symbols.size() < 2) {
        std::fprintf(stderr, "need at least 2 usable symbols in %s (found %zu)\n",
                     opt.dir.c_str(), symbols.size());
        return 1;
    }
    std::sort(symbols.begin(), symbols.end(),
              [](const Symbol& a, const Symbol& b) { return a.name < b.name; });

    const std::size_t n = symbols.size();
    const std::size_t total_pairs = n * (n - 1) / 2;
    std::printf("loaded %zu symbols from %s (%zu too short, %zu unreadable) - %zu pairs to consider\n",
                n, opt.dir.c_str(), skipped_short, skipped_bad, total_pairs);

    // ── Scan ──────────────────────────────────────────────────────────────
    // Pairs are independent, so this is embarrassingly parallel. Work is
    // handed out by atomic index over the flattened upper triangle rather than
    // split into equal blocks: pair cost varies a lot with overlap length, and
    // equal blocks would leave most threads idle waiting for the slowest.
    const unsigned nthreads = opt.threads ? opt.threads
                                          : std::max(1u, std::thread::hardware_concurrency());
    std::atomic<std::size_t> cursor{0};
    std::atomic<std::size_t> tested{0}, prefiltered{0};
    std::mutex hits_mutex;
    std::vector<Hit> hits;

    const auto started = std::chrono::steady_clock::now();

    auto worker = [&]() {
        std::vector<double> ra, rb;
        for (;;) {
            const std::size_t k = cursor.fetch_add(1, std::memory_order_relaxed);
            if (k >= total_pairs) break;

            // Unflatten k into (i, j) with i < j.
            std::size_t i = 0, rem = k;
            while (rem >= n - 1 - i) { rem -= (n - 1 - i); ++i; }
            const std::size_t j = i + 1 + rem;

            const AlignedPair al = align(symbols[i].bars, symbols[j].bars);
            if (al.size() < opt.min_bars) continue;

            const std::size_t take = std::min(al.size(), opt.max_bars);
            const std::size_t off = al.size() - take;

            // Log prices, matching the Python pairs model - the spread it
            // trades is a log-price spread, so the test has to be on the same
            // quantity or the hedge ratio means something different.
            std::vector<double> la(take), lb(take);
            for (std::size_t t = 0; t < take; ++t) {
                la[t] = std::log(al.a[off + t]);
                lb[t] = std::log(al.b[off + t]);
            }

            // Cheap prefilter first: correlation of RETURNS, not of levels.
            // Two unrelated series that both drift upward have high level
            // correlation and no relationship whatsoever.
            ra.resize(take - 1);
            rb.resize(take - 1);
            for (std::size_t t = 1; t < take; ++t) {
                ra[t - 1] = la[t] - la[t - 1];
                rb[t - 1] = lb[t] - lb[t - 1];
            }
            const double corr = correlation(ra, rb);
            if (!(std::fabs(corr) >= opt.min_correlation)) { prefiltered.fetch_add(1, std::memory_order_relaxed); continue; }

            const CointResult r = coint(la, lb);
            tested.fetch_add(1, std::memory_order_relaxed);
            if (!r.ok || !(r.pvalue <= opt.alpha)) continue;

            Hit h{symbols[i].name, symbols[j].name, corr, r.pvalue, r.stat,
                  r.hedge_ratio, take, r.used_lags};
            const std::lock_guard<std::mutex> lock(hits_mutex);
            hits.push_back(std::move(h));
        }
    };

    std::vector<std::thread> pool;
    pool.reserve(nthreads);
    for (unsigned t = 0; t < nthreads; ++t) pool.emplace_back(worker);
    for (auto& t : pool) t.join();

    const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();

    // ── Report ────────────────────────────────────────────────────────────
    std::sort(hits.begin(), hits.end(),
              [](const Hit& a, const Hit& b) { return a.pvalue < b.pvalue; });

    std::FILE* out = std::fopen(opt.out.c_str(), "w");
    if (!out) {
        std::fprintf(stderr, "cannot write %s\n", opt.out.c_str());
        return 1;
    }
    std::fprintf(out, "symbol_a,symbol_b,correlation,coint_pvalue,coint_stat,hedge_ratio,nobs,used_lags\n");
    for (const Hit& h : hits) {
        std::fprintf(out, "%s,%s,%.6f,%.8g,%.6f,%.6f,%zu,%zu\n", h.a.c_str(), h.b.c_str(),
                     h.correlation, h.pvalue, h.stat, h.hedge_ratio, h.nobs, h.used_lags);
    }
    std::fclose(out);

    std::printf("%zu pairs cleared correlation >= %.2f and were tested; %zu prefiltered out\n",
                tested.load(), opt.min_correlation, prefiltered.load());
    std::printf("%zu pairs qualify at alpha <= %.3f -> %s\n", hits.size(), opt.alpha, opt.out.c_str());
    std::printf("scanned in %.1fs on %u threads\n", elapsed, nthreads);

    const std::size_t show = std::min<std::size_t>(hits.size(), 10);
    if (show) std::printf("\nstrongest:\n");
    for (std::size_t i = 0; i < show; ++i) {
        std::printf("  %-10s %-10s  p=%-10.3g corr=%+.3f  hedge=%+.4f  n=%zu\n",
                    hits[i].a.c_str(), hits[i].b.c_str(), hits[i].pvalue,
                    hits[i].correlation, hits[i].hedge_ratio, hits[i].nobs);
    }
    return 0;
}
