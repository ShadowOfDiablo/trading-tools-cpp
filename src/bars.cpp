#include "bars.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstdio>
#include <fstream>
#include <stdexcept>

namespace tb {
namespace {

// Days since the Unix epoch for a civil date, by Howard Hinnant's days_from_civil.
// std::chrono::year_month_day would do this, but libstdc++ support for the
// calendar types is uneven across the GCC versions this may be built with, and
// this is twelve lines with no dependency risk.
std::int64_t days_from_civil(std::int64_t y, unsigned m, unsigned d) noexcept {
    y -= m <= 2;
    const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153u * (m + (m > 2 ? -3 : 9)) + 2u) / 5u + d - 1u;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097LL + static_cast<std::int64_t>(doe) - 719468LL;
}

// Reads exactly `digits` ASCII digits starting at `pos`, advancing it.
bool read_uint(std::string_view s, std::size_t& pos, int digits, int& out) noexcept {
    if (pos + static_cast<std::size_t>(digits) > s.size()) return false;
    int value = 0;
    for (int i = 0; i < digits; ++i) {
        const char c = s[pos + static_cast<std::size_t>(i)];
        if (c < '0' || c > '9') return false;
        value = value * 10 + (c - '0');
    }
    pos += static_cast<std::size_t>(digits);
    out = value;
    return true;
}

bool parse_double(std::string_view field, double& out) noexcept {
    while (!field.empty() && (field.front() == ' ' || field.front() == '\t')) field.remove_prefix(1);
    while (!field.empty() && (field.back() == ' ' || field.back() == '\t' || field.back() == '\r'))
        field.remove_suffix(1);
    if (field.empty()) return false;
    const char* first = field.data();
    const char* last = field.data() + field.size();
    auto [ptr, ec] = std::from_chars(first, last, out);
    return ec == std::errc{} && ptr == last;
}

void split_fields(std::string_view line, std::vector<std::string_view>& out) {
    out.clear();
    std::size_t start = 0;
    for (std::size_t i = 0; i <= line.size(); ++i) {
        if (i == line.size() || line[i] == ',') {
            out.push_back(line.substr(start, i - start));
            start = i + 1;
        }
    }
}

std::string_view trim(std::string_view s) noexcept {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\xef')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}

}  // namespace

std::optional<std::int64_t> parse_timestamp(std::string_view s) {
    s = trim(s);
    // "YYYY-MM-DD" is the shortest thing we accept.
    if (s.size() < 10) return std::nullopt;

    std::size_t pos = 0;
    int year = 0, month = 0, day = 0;
    if (!read_uint(s, pos, 4, year)) return std::nullopt;
    if (pos >= s.size() || s[pos] != '-') return std::nullopt;
    ++pos;
    if (!read_uint(s, pos, 2, month)) return std::nullopt;
    if (pos >= s.size() || s[pos] != '-') return std::nullopt;
    ++pos;
    if (!read_uint(s, pos, 2, day)) return std::nullopt;
    if (month < 1 || month > 12 || day < 1 || day > 31) return std::nullopt;

    int hour = 0, minute = 0, second = 0;
    if (pos < s.size() && (s[pos] == ' ' || s[pos] == 'T')) {
        ++pos;
        if (!read_uint(s, pos, 2, hour)) return std::nullopt;
        if (pos >= s.size() || s[pos] != ':') return std::nullopt;
        ++pos;
        if (!read_uint(s, pos, 2, minute)) return std::nullopt;
        if (pos < s.size() && s[pos] == ':') {
            ++pos;
            if (!read_uint(s, pos, 2, second)) return std::nullopt;
        }
        // Fractional seconds, if present, are irrelevant at bar resolution.
        if (pos < s.size() && s[pos] == '.') {
            ++pos;
            while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9') ++pos;
        }
    }
    if (hour > 23 || minute > 59 || second > 60) return std::nullopt;

    std::int64_t utc = days_from_civil(year, static_cast<unsigned>(month), static_cast<unsigned>(day)) * 86400LL +
                       hour * 3600LL + minute * 60LL + second;

    // Trailing zone: Z, +HH:MM, -HH:MM, or +HHMM. A naive timestamp with no
    // zone at all is treated as UTC, which is what pandas'
    // pd.to_datetime(..., utc=True) does with the same files.
    if (pos < s.size()) {
        const char c = s[pos];
        if (c == 'Z' || c == 'z') {
            ++pos;
        } else if (c == '+' || c == '-') {
            ++pos;
            int oh = 0, om = 0;
            if (!read_uint(s, pos, 2, oh)) return std::nullopt;
            if (pos < s.size() && s[pos] == ':') ++pos;
            if (pos < s.size()) {
                if (!read_uint(s, pos, 2, om)) return std::nullopt;
            }
            const std::int64_t offset = oh * 3600LL + om * 60LL;
            // "-04:00" means local is BEHIND UTC, so UTC is later.
            utc += (c == '-') ? offset : -offset;
        }
    }
    if (pos != s.size()) return std::nullopt;
    return utc;
}

Bars load_csv(const std::string& path, std::string symbol) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw BarParseError("cannot open " + path);

    Bars bars;
    bars.symbol = std::move(symbol);

    std::string line;
    if (!std::getline(in, line)) return bars;  // empty file: no bars, not an error

    std::vector<std::string_view> fields;
    split_fields(line, fields);

    // Column names, not positions: the archive and cache files agree today,
    // but a hardcoded order would break silently and produce plausible-looking
    // numbers rather than an error.
    int idx_ts = -1, idx_open = -1, idx_high = -1, idx_low = -1, idx_close = -1, idx_volume = -1;
    for (std::size_t i = 0; i < fields.size(); ++i) {
        std::string name;
        for (char c : trim(fields[i])) name.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        const int at = static_cast<int>(i);
        if (name == "datetime" || name == "date" || name.empty()) idx_ts = (idx_ts < 0) ? at : idx_ts;
        else if (name == "open") idx_open = at;
        else if (name == "high") idx_high = at;
        else if (name == "low") idx_low = at;
        else if (name == "close") idx_close = at;
        else if (name == "volume") idx_volume = at;
    }
    if (idx_ts < 0 || idx_close < 0) {
        throw BarParseError(path + ": header has no timestamp and/or close column (found: " + line + ")");
    }

    const int widest = std::max({idx_ts, idx_open, idx_high, idx_low, idx_close, idx_volume});

    std::size_t lineno = 1;
    while (std::getline(in, line)) {
        ++lineno;
        if (trim(line).empty()) continue;
        split_fields(line, fields);
        if (static_cast<int>(fields.size()) <= widest) {
            throw BarParseError(path + ":" + std::to_string(lineno) + ": expected at least " +
                                std::to_string(widest + 1) + " fields, got " + std::to_string(fields.size()));
        }

        const auto ts = parse_timestamp(fields[static_cast<std::size_t>(idx_ts)]);
        if (!ts) {
            throw BarParseError(path + ":" + std::to_string(lineno) + ": unparseable timestamp '" +
                                std::string(trim(fields[static_cast<std::size_t>(idx_ts)])) + "'");
        }

        // A row whose numbers do not parse is dropped rather than fatal: real
        // yfinance output contains occasional all-NaN rows for halted or
        // untraded sessions, and one of those must not kill a whole scan.
        auto get = [&](int idx, double& out) {
            if (idx < 0) { out = 0.0; return true; }
            return parse_double(fields[static_cast<std::size_t>(idx)], out);
        };
        double o = 0, h = 0, l = 0, c = 0, v = 0;
        if (!get(idx_open, o) || !get(idx_high, h) || !get(idx_low, l) ||
            !get(idx_close, c) || !get(idx_volume, v)) {
            continue;
        }
        if (!(c > 0.0)) continue;  // a zero/negative close is not a price

        if (!bars.timestamp.empty() && *ts <= bars.timestamp.back()) {
            // Out-of-order or duplicate rows would corrupt every rolling
            // window downstream. The Python writers sort and dedupe; if that
            // ever regresses, this says so instead of producing junk.
            throw BarParseError(path + ":" + std::to_string(lineno) +
                                ": timestamps are not strictly increasing");
        }

        bars.timestamp.push_back(*ts);
        bars.open.push_back(o);
        bars.high.push_back(h);
        bars.low.push_back(l);
        bars.close.push_back(c);
        bars.volume.push_back(v);
    }

    return bars;
}

AlignedPair align(const Bars& a, const Bars& b) {
    AlignedPair out;
    const std::size_t na = a.size(), nb = b.size();
    out.timestamp.reserve(std::min(na, nb));
    out.a.reserve(std::min(na, nb));
    out.b.reserve(std::min(na, nb));

    std::size_t i = 0, j = 0;
    while (i < na && j < nb) {
        const std::int64_t ta = a.timestamp[i], tb = b.timestamp[j];
        if (ta == tb) {
            out.timestamp.push_back(ta);
            out.a.push_back(a.close[i]);
            out.b.push_back(b.close[j]);
            ++i;
            ++j;
        } else if (ta < tb) {
            ++i;
        } else {
            ++j;
        }
    }
    return out;
}

}  // namespace tb
