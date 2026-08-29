#pragma once
//
// Loading OHLCV bars from CSV files named <SYMBOL>__<interval>.csv, the
// layout most bar exporters (yfinance-based ones included) produce.
//
// Columnar, not a vector of Bar structs: every consumer here walks one or two
// series end to end (closes for a spread, highs/lows for a stop check), so
// keeping each field contiguous is both what the algorithms want and what the
// cache wants. A struct-of-arrays also lets a caller hand a bare
// std::span<const double> to the numeric code in stats.hpp without that code
// needing to know anything about bars at all.
//
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace tb {

// One symbol's price history. All vectors are the same length and share an
// index: row i of `close` belongs to `timestamp[i]`.
struct Bars {
    std::string symbol;
    std::vector<std::int64_t> timestamp;  // Unix seconds, UTC
    std::vector<double> open;
    std::vector<double> high;
    std::vector<double> low;
    std::vector<double> close;
    std::vector<double> volume;

    std::size_t size() const noexcept { return close.size(); }
    bool empty() const noexcept { return close.empty(); }

    std::span<const double> closes() const noexcept { return close; }
};

// Thrown for anything that means "the file cannot be trusted": a missing
// required column, a malformed number, a row with the wrong field count.
//
// Deliberately NOT used for an empty or short file - that is a normal state
// for a symbol the archiver has only just started following, and callers skip
// those rather than aborting a 576-symbol scan over one thin ticker.
class BarParseError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Parses one CSV. The first column is the timestamp and may be named either
// "Datetime" (intraday files) or "Date" (daily files) - yfinance names it
// differently per interval and both land in these directories.
Bars load_csv(const std::string& path, std::string symbol = {});

// Parses an ISO-8601 timestamp of the shapes these files actually contain:
//   2026-06-10 18:10:00+00:00     (archive, intraday)
//   2021-08-30 00:00:00-04:00     (cache, daily, local exchange offset)
//   2026-06-10T18:10:00Z
// Returns Unix seconds UTC, or nullopt if it does not parse. The offset is
// applied - two symbols on different exchanges must end up on one shared
// clock or every cross-symbol calculation silently compares misaligned rows.
std::optional<std::int64_t> parse_timestamp(std::string_view s);

// Intersects two symbols' histories on their common timestamps, returning the
// aligned closes. Both inputs must be sorted ascending by timestamp (which the
// Python writers guarantee; load_csv verifies it).
//
// This is the step it is easiest to get quietly wrong: two symbols listed on
// different exchanges have different holidays, different half-days, and
// different first-traded dates. Zipping their close columns by position
// instead of by timestamp yields a spread that drifts a row further out of
// alignment after every mismatched session, which then looks exactly like a
// slow trend and reads as strong evidence of cointegration.
struct AlignedPair {
    std::vector<std::int64_t> timestamp;
    std::vector<double> a;
    std::vector<double> b;
    std::size_t size() const noexcept { return a.size(); }
};
AlignedPair align(const Bars& a, const Bars& b);

}  // namespace tb
