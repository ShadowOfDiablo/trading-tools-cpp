#include "harness.hpp"

#include "bars.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

using namespace tb;

namespace {

// Writes a CSV into a temp path that is removed when the guard goes out of
// scope, so a failing test cannot leave litter behind.
struct TempCsv {
    std::filesystem::path path;
    explicit TempCsv(const std::string& contents) {
        static int counter = 0;
        path = std::filesystem::temp_directory_path() /
               ("tb_test_" + std::to_string(++counter) + ".csv");
        std::ofstream out(path, std::ios::binary);
        out << contents;
    }
    ~TempCsv() { std::error_code ec; std::filesystem::remove(path, ec); }
    std::string str() const { return path.string(); }
};

}  // namespace

TEST(parses_utc_timestamps) {
    const auto t = parse_timestamp("2026-06-10 18:10:00+00:00");
    CHECK(t.has_value());
    // 2026-06-10T18:10:00Z
    CHECK_EQ(*t, 1781115000LL);
}

TEST(applies_the_utc_offset) {
    // The cache files carry local exchange offsets. "-04:00" means local is
    // behind UTC, so the UTC instant is four hours LATER. Getting the sign
    // wrong misaligns every US symbol against every European one by 8 hours,
    // which silently produces garbage spreads rather than an error.
    const auto utc = parse_timestamp("2021-08-30 00:00:00+00:00");
    const auto ny = parse_timestamp("2021-08-30 00:00:00-04:00");
    CHECK(utc.has_value() && ny.has_value());
    CHECK_EQ(*ny - *utc, 4 * 3600LL);

    const auto berlin = parse_timestamp("2021-08-30 00:00:00+02:00");
    CHECK(berlin.has_value());
    CHECK_EQ(*utc - *berlin, 2 * 3600LL);
}

TEST(accepts_the_timestamp_shapes_these_files_actually_contain) {
    CHECK(parse_timestamp("2026-06-10T18:10:00Z").has_value());
    CHECK(parse_timestamp("2026-06-10 18:10:00").has_value());   // naive: treated as UTC
    CHECK(parse_timestamp("2026-06-10").has_value());            // date only
    CHECK(parse_timestamp("2026-06-10 18:10:00.123+00:00").has_value());
    CHECK_EQ(*parse_timestamp("2026-06-10T18:10:00Z"), *parse_timestamp("2026-06-10 18:10:00+00:00"));
}

TEST(rejects_junk_timestamps) {
    CHECK(!parse_timestamp("not a date").has_value());
    CHECK(!parse_timestamp("2026-13-10 00:00:00+00:00").has_value());  // month 13
    CHECK(!parse_timestamp("2026-06-10 25:00:00+00:00").has_value());  // hour 25
    CHECK(!parse_timestamp("2026-06-10 18:10:00+00:00 trailing").has_value());
    CHECK(!parse_timestamp("").has_value());
}

TEST(loads_a_well_formed_file) {
    TempCsv f(
        "Datetime,open,high,low,close,volume\n"
        "2026-06-10 18:10:00+00:00,1,2,0.5,1.5,100\n"
        "2026-06-10 18:15:00+00:00,1.5,2.5,1,2,200\n");
    const Bars b = load_csv(f.str(), "TEST");
    CHECK_EQ(b.size(), std::size_t{2});
    CHECK_EQ(b.symbol, std::string("TEST"));
    CHECK_NEAR(b.close[0], 1.5, 1e-12);
    CHECK_NEAR(b.high[1], 2.5, 1e-12);
    CHECK_EQ(b.timestamp[1] - b.timestamp[0], 300LL);
}

TEST(reads_columns_by_name_not_position) {
    // A hardcoded column order would keep "working" against a reordered file
    // and quietly swap high for low.
    TempCsv f(
        "Date,volume,close,low,high,open\n"
        "2026-06-10,100,1.5,0.5,2,1\n");
    const Bars b = load_csv(f.str());
    CHECK_NEAR(b.close[0], 1.5, 1e-12);
    CHECK_NEAR(b.high[0], 2.0, 1e-12);
    CHECK_NEAR(b.low[0], 0.5, 1e-12);
    CHECK_NEAR(b.volume[0], 100.0, 1e-12);
}

TEST(skips_rows_whose_numbers_do_not_parse) {
    // Real yfinance output contains occasional all-NaN rows for halted or
    // untraded sessions; one of those must not abort a 576-symbol scan.
    TempCsv f(
        "Datetime,open,high,low,close,volume\n"
        "2026-06-10 18:10:00+00:00,1,2,0.5,1.5,100\n"
        "2026-06-10 18:15:00+00:00,,,,,\n"
        "2026-06-10 18:20:00+00:00,nan,nan,nan,nan,nan\n"
        "2026-06-10 18:25:00+00:00,1.5,2.5,1,2,200\n");
    const Bars b = load_csv(f.str());
    CHECK_EQ(b.size(), std::size_t{2});
}

TEST(rejects_a_header_with_no_close_column) {
    TempCsv f("Datetime,open,high,low,volume\n2026-06-10,1,2,0.5,100\n");
    CHECK_THROWS(load_csv(f.str()));
}

TEST(rejects_out_of_order_timestamps) {
    // Every rolling window downstream assumes monotonic time. The Python
    // writers sort and dedupe; if that regresses this says so loudly instead
    // of producing plausible nonsense.
    TempCsv f(
        "Datetime,open,high,low,close,volume\n"
        "2026-06-10 18:15:00+00:00,1,2,0.5,1.5,100\n"
        "2026-06-10 18:10:00+00:00,1,2,0.5,1.5,100\n");
    CHECK_THROWS(load_csv(f.str()));
}

TEST(rejects_duplicate_timestamps) {
    TempCsv f(
        "Datetime,open,high,low,close,volume\n"
        "2026-06-10 18:10:00+00:00,1,2,0.5,1.5,100\n"
        "2026-06-10 18:10:00+00:00,1,2,0.5,1.5,100\n");
    CHECK_THROWS(load_csv(f.str()));
}

TEST(an_empty_file_is_no_bars_not_an_error) {
    // A symbol the archiver has only just started following is normal.
    TempCsv f("");
    CHECK(load_csv(f.str()).empty());
    TempCsv header_only("Datetime,open,high,low,close,volume\n");
    CHECK(load_csv(header_only.str()).empty());
}

TEST(a_missing_file_is_an_error) {
    CHECK_THROWS(load_csv("no_such_file_anywhere.csv"));
}

TEST(align_intersects_on_timestamps_not_position) {
    // The bug this exists to prevent: two symbols with different holidays
    // zipped by position drift one row further apart after every mismatched
    // session, and the resulting spread looks exactly like a slow trend.
    Bars a, b;
    a.timestamp = {100, 200, 300, 400};
    a.close = {1, 2, 3, 4};
    b.timestamp = {100, 300, 400, 500};
    b.close = {10, 30, 40, 50};

    const AlignedPair p = align(a, b);
    CHECK_EQ(p.size(), std::size_t{3});
    CHECK_EQ(p.timestamp[1], 300LL);
    CHECK_NEAR(p.a[1], 3.0, 1e-12);
    CHECK_NEAR(p.b[1], 30.0, 1e-12);
    // Position-zipping would have paired a=2 with b=30 here.
    CHECK_NEAR(p.a[0], 1.0, 1e-12);
    CHECK_NEAR(p.b[0], 10.0, 1e-12);
}

TEST(align_of_disjoint_histories_is_empty) {
    Bars a, b;
    a.timestamp = {100, 200};
    a.close = {1, 2};
    b.timestamp = {300, 400};
    b.close = {3, 4};
    CHECK_EQ(align(a, b).size(), std::size_t{0});
}
