#include "harness.hpp"

#include "portfolio.hpp"

#include <string>
#include <vector>

using namespace tb;

namespace {

SignalRow bar(std::int64_t ts, const char* sym, double price, bool is_long,
              bool qualifies = true, const char* group = "G1") {
    SignalRow r;
    r.timestamp = ts;
    r.symbol = sym;
    r.ticker = std::string(sym) + "_EQ";
    r.group = group;
    r.price = price;
    r.confidence = is_long ? 0.80 : 0.20;
    r.threshold = 0.55;
    r.qualifies = qualifies;
    r.is_long = is_long;
    r.tp_pct = 0.018;
    r.sl_pct = 0.010;
    r.sl_distance = price * 0.010;
    r.trend_sl_pct = 0.010;
    r.risk = 0.02;
    r.risk_scale = 1.0;
    return r;
}

BacktestConfig base_config() {
    BacktestConfig c;
    c.starting_cash = 10000.0;
    c.max_positions_per_group = 2;
    c.max_open_positions = 10;
    c.max_position_allocation = 0.25;
    c.max_portfolio_risk_pct = 0.08;
    c.max_daily_loss = 0.03;
    return c;
}

Position pos_at(double entry, double current, double peak) {
    Position p;
    p.ticker = "X_EQ";
    p.symbol = "X";
    p.quantity = 10.0;
    p.average_price = entry;
    p.current_price = current;
    p.peak_price = peak;
    p.trend_sl_pct = 0.010;
    return p;
}

constexpr std::int64_t DAY = 86400;

}  // namespace

/* ── position sizing ─────────────────────────────────────────────────────── */

TEST(sizing_is_risk_budget_divided_by_stop_distance) {
    // 10000 cash, 2% risk = 200 at risk. Stop is 1.00 away, so 200 shares -
    // except the 25% allocation ceiling caps notional at 2500, i.e. 25 shares
    // at 100. The ceiling is meant to bind here.
    const double qty = position_size(10000, 100, 0.02, std::nullopt, std::nullopt,
                                     1.00, 0.25, 0, 10, false, 0.4);
    CHECK_NEAR(qty, 25.0, 1e-9);
}

TEST(sizing_uses_the_risk_budget_when_the_ceiling_does_not_bind) {
    // Stop 10.00 away: 200/10 = 20 shares = 2000 notional, under the 2500 cap.
    const double qty = position_size(10000, 100, 0.02, std::nullopt, std::nullopt,
                                     10.00, 0.25, 0, 10, false, 0.4);
    CHECK_NEAR(qty, 20.0, 1e-9);
}

TEST(sizing_scales_with_confidence_margin_over_the_threshold) {
    // At exactly the threshold the scale is 0.5; far above it approaches 1.0.
    const double at_threshold = position_size(10000, 100, 0.02, 0.55, 0.55, 10.0, 0.25, 0, 10, false, 0.4);
    const double well_above = position_size(10000, 100, 0.02, 0.95, 0.55, 10.0, 0.25, 0, 10, false, 0.4);
    CHECK_NEAR(at_threshold, 10.0, 1e-9);           // 0.5 * 20
    CHECK(well_above > at_threshold);
    CHECK(well_above <= 20.0);
}

TEST(sizing_confidence_scale_has_a_floor) {
    // Below the threshold the scale clamps at 0.1 rather than going negative
    // and producing a nonsensical (or negative) size.
    const double qty = position_size(10000, 100, 0.02, 0.10, 0.90, 10.0, 0.25, 0, 10, false, 0.4);
    CHECK(qty > 0.0);
    CHECK_NEAR(qty, 2.0, 1e-9);  // 0.1 * 20
}

TEST(portfolio_heat_shrinks_later_positions) {
    const double first = position_size(10000, 100, 0.02, std::nullopt, std::nullopt,
                                       10.0, 0.25, 0, 10, true, 0.4);
    const double tenth = position_size(10000, 100, 0.02, std::nullopt, std::nullopt,
                                       10.0, 0.25, 10, 10, true, 0.4);
    CHECK_NEAR(first, 20.0, 1e-9);
    CHECK_NEAR(tenth, 8.0, 1e-9);  // heat_min_scale 0.4 at the reference count
}

TEST(sizing_refuses_nonsense_inputs) {
    CHECK_EQ(position_size(10000, 0, 0.02, std::nullopt, std::nullopt, 1.0, 0.25, 0, 10, false, 0.4), 0.0);
    CHECK_EQ(position_size(0, 100, 0.02, std::nullopt, std::nullopt, 1.0, 0.25, 0, 10, false, 0.4), 0.0);
    CHECK_EQ(position_size(-5, 100, 0.02, std::nullopt, std::nullopt, 1.0, 0.25, 0, 10, false, 0.4), 0.0);
}

/* ── stops ───────────────────────────────────────────────────────────────── */

TEST(fixed_stop_and_target_fire_at_their_thresholds) {
    CHECK(!check_position_safety(pos_at(100, 100.5, 100.5), 0.018, 0.010, false, {}, {}).has_value());
    CHECK_EQ(*check_position_safety(pos_at(100, 98.9, 100), 0.018, 0.010, false, {}, {}), std::string("STOP_LOSS"));
    CHECK_EQ(*check_position_safety(pos_at(100, 101.9, 101.9), 0.018, 0.010, false, {}, {}), std::string("TAKE_PROFIT"));
}

TEST(trailing_disabled_takes_the_fixed_profit) {
    // Same position, trailing off: the fixed take-profit must still fire, so
    // enabling trailing is the ONLY thing that changes this behaviour.
    CHECK_EQ(*check_position_safety(pos_at(100, 105, 110), 0.018, 0.010, false, {}, {}),
             std::string("TAKE_PROFIT"));
}

TEST(trailing_lets_a_winner_run_past_the_fixed_target) {
    // Peak 110 is +10%, well past the 1.8% activation, so trailing is armed and
    // the fixed take-profit no longer applies. At 109.5 the position is above
    // the trailing stop (110 * 0.99 = 108.9), so it holds - where with trailing
    // off the same position would have been closed at +1.8%.
    CHECK(!check_position_safety(pos_at(100, 109.5, 110), 0.018, 0.010, true, {}, {}).has_value());
}

TEST(trailing_stop_fires_on_a_pullback_from_the_peak) {
    CHECK_EQ(*check_position_safety(pos_at(100, 108.0, 110), 0.018, 0.010, true, {}, {}),
             std::string("TRAILING_STOP"));
}

TEST(fixed_stop_still_floors_a_loose_trailing_distance) {
    // A trailing distance deliberately set looser than the stop-loss must not
    // be able to hold a position past its own fixed stop.
    const auto r = check_position_safety(pos_at(100, 98.0, 110), 0.018, 0.010, true, {}, 0.50);
    CHECK_EQ(*r, std::string("STOP_LOSS"));
}

TEST(trailing_with_no_peak_above_entry_behaves_exactly_as_fixed) {
    // The lost-peak-state fallback: if peak tracking was never populated, the
    // whole trailing block is skipped rather than misbehaving.
    Position p = pos_at(100, 101.9, 0.0);
    CHECK_EQ(*check_position_safety(p, 0.018, 0.010, true, {}, {}), std::string("TAKE_PROFIT"));
}

/* ── portfolio-level risk ────────────────────────────────────────────────── */

TEST(portfolio_risk_values_positions_at_their_trend_stop) {
    std::vector<Position> ps{pos_at(100, 100, 100), pos_at(50, 50, 50)};
    ps[1].ticker = "Y_EQ";
    // 10 * 100 * 0.01 + 10 * 50 * 0.01 = 10 + 5
    CHECK_NEAR(total_portfolio_risk_cash(ps), 15.0, 1e-9);
}

/* ── the replay loop ─────────────────────────────────────────────────────── */

TEST(a_simple_winning_trade_is_opened_and_closed_at_the_target) {
    std::vector<SignalRow> rows{
        bar(0, "AAA", 100.0, true),
        bar(3600, "AAA", 102.0, true),  // +2% >= 1.8% target
    };
    const BacktestResult r = run(rows, base_config());
    CHECK_EQ(r.trades.size(), std::size_t{1});
    CHECK_EQ(r.trades[0].exit_reason, std::string("TAKE_PROFIT"));
    CHECK(r.trades[0].pnl > 0.0);
    CHECK(r.ending_equity > r.starting_cash);
}

TEST(a_losing_trade_stops_out) {
    std::vector<SignalRow> rows{
        bar(0, "AAA", 100.0, true),
        bar(3600, "AAA", 98.5, true),  // -1.5% <= -1.0% stop
    };
    const BacktestResult r = run(rows, base_config());
    CHECK_EQ(r.trades.size(), std::size_t{1});
    CHECK_EQ(r.trades[0].exit_reason, std::string("STOP_LOSS"));
    CHECK(r.trades[0].pnl < 0.0);
}

TEST(an_unqualified_model_cannot_open_a_position_by_default) {
    std::vector<SignalRow> rows{
        bar(0, "AAA", 100.0, true, /*qualifies=*/false),
        bar(3600, "AAA", 102.0, true, false),
    };
    const BacktestResult r = run(rows, base_config());
    CHECK_EQ(r.trades.size(), std::size_t{0});
    CHECK(r.blocked_not_qualifying > 0);
}

TEST(low_edge_trading_lets_an_unqualified_model_trade) {
    BacktestConfig c = base_config();
    c.allow_low_edge_trading = true;
    std::vector<SignalRow> rows{
        bar(0, "AAA", 100.0, true, false),
        bar(3600, "AAA", 102.0, true, false),
    };
    const BacktestResult r = run(rows, c);
    CHECK_EQ(r.trades.size(), std::size_t{1});
}

TEST(an_unqualified_flat_never_forces_an_exit) {
    // The rule this protects: an unqualified engine's FLAT means "no
    // trustworthy opinion this cycle", not "get out". Only the price-based
    // stops may close such a position.
    std::vector<SignalRow> rows{
        bar(0, "AAA", 100.0, true, true),
        bar(3600, "AAA", 100.2, false, /*qualifies=*/false),
        bar(7200, "AAA", 100.3, false, false),
    };
    const BacktestResult r = run(rows, base_config());
    CHECK_EQ(r.trades.size(), std::size_t{1});
    // Still open at the end, so it closes on END_OF_DATA rather than ML_EXIT.
    CHECK_EQ(r.trades[0].exit_reason, std::string("END_OF_DATA"));
}

TEST(a_qualified_flat_closes_the_position) {
    std::vector<SignalRow> rows{
        bar(0, "AAA", 100.0, true, true),
        bar(3600, "AAA", 100.2, false, /*qualifies=*/true),
    };
    const BacktestResult r = run(rows, base_config());
    CHECK_EQ(r.trades.size(), std::size_t{1});
    CHECK_EQ(r.trades[0].exit_reason, std::string("ML_EXIT"));
}

TEST(the_correlation_group_cap_blocks_a_third_name_in_one_group) {
    std::vector<SignalRow> rows{
        bar(0, "AAA", 100.0, true, true, "TECH"),
        bar(1, "BBB", 100.0, true, true, "TECH"),
        bar(2, "CCC", 100.0, true, true, "TECH"),   // cap is 2
        bar(3, "DDD", 100.0, true, true, "BANKS"),  // different group, allowed
    };
    const BacktestResult r = run(rows, base_config());
    CHECK(r.blocked_group_cap >= 1);
    // AAA, BBB and DDD opened; CCC did not.
    CHECK_EQ(r.trades.size(), std::size_t{3});
    for (const Trade& t : r.trades) CHECK(t.symbol != std::string("CCC"));
}

TEST(the_same_ticker_is_never_opened_twice) {
    std::vector<SignalRow> rows{
        bar(0, "AAA", 100.0, true),
        bar(3600, "AAA", 100.1, true),
        bar(7200, "AAA", 100.2, true),
    };
    const BacktestResult r = run(rows, base_config());
    CHECK_EQ(r.trades.size(), std::size_t{1});
    CHECK(r.blocked_already_held >= 1);
}

TEST(the_portfolio_risk_cap_blocks_further_entries) {
    // A 0.1% cap cannot accommodate even one position's stop exposure.
    BacktestConfig c = base_config();
    c.max_portfolio_risk_pct = 0.001;
    std::vector<SignalRow> rows{bar(0, "AAA", 100.0, true), bar(1, "BBB", 100.0, true, true, "G2")};
    const BacktestResult r = run(rows, c);
    CHECK_EQ(r.trades.size(), std::size_t{0});
    CHECK(r.blocked_risk_cap >= 1);
}

TEST(the_kill_switch_flattens_everything_and_stops_the_day) {
    BacktestConfig c = base_config();
    c.max_daily_loss = 0.005;  // 0.5%: a single stop-out is enough to trip it
    std::vector<SignalRow> rows{
        bar(0, "AAA", 100.0, true),
        bar(3600, "AAA", 90.0, true),      // a big adverse move
        bar(7200, "BBB", 100.0, true, true, "G2"),  // same day: must be refused
    };
    const BacktestResult r = run(rows, c);
    CHECK(r.kill_switch_days >= 1);
    CHECK(r.blocked_kill_switch >= 1);
}

TEST(the_kill_switch_resets_on_a_new_utc_day) {
    BacktestConfig c = base_config();
    c.max_daily_loss = 0.005;
    std::vector<SignalRow> rows{
        bar(0, "AAA", 100.0, true),
        bar(3600, "AAA", 90.0, true),
        bar(7200, "BBB", 100.0, true, true, "G2"),  // blocked, same day
        bar(DAY + 3600, "BBB", 100.0, true, true, "G2"),  // next day: allowed
        bar(DAY + 7200, "BBB", 102.0, true, true, "G2"),
    };
    const BacktestResult r = run(rows, c);
    bool traded_bbb = false;
    for (const Trade& t : r.trades) if (t.symbol == "BBB") traded_bbb = true;
    CHECK(traded_bbb);
}

TEST(open_positions_are_closed_at_the_end_so_the_return_is_realised) {
    std::vector<SignalRow> rows{bar(0, "AAA", 100.0, true), bar(3600, "AAA", 100.5, true)};
    const BacktestResult r = run(rows, base_config());
    CHECK_EQ(r.trades.size(), std::size_t{1});
    CHECK_EQ(r.trades[0].exit_reason, std::string("END_OF_DATA"));
}

TEST(transaction_costs_reduce_pnl_on_both_sides) {
    std::vector<SignalRow> rows{bar(0, "AAA", 100.0, true), bar(3600, "AAA", 102.0, true)};
    BacktestConfig with_costs = base_config();
    with_costs.cost_per_side = 0.001;
    const BacktestResult free_run = run(rows, base_config());
    const BacktestResult paid = run(rows, with_costs);
    CHECK(paid.trades[0].pnl < free_run.trades[0].pnl);
    CHECK(paid.trades[0].costs > 0.0);
}

TEST(cash_never_goes_negative) {
    // Many simultaneous entries across distinct groups, with a cap high enough
    // not to bind - the account must still never be overdrawn.
    BacktestConfig c = base_config();
    c.max_portfolio_risk_pct = 1.0;
    c.max_positions_per_group = 100;
    std::vector<SignalRow> rows;
    for (int i = 0; i < 40; ++i) {
        rows.push_back(bar(i, ("S" + std::to_string(i)).c_str(), 100.0, true));
    }
    const BacktestResult r = run(rows, c);
    for (const EquityPoint& e : r.equity_curve) CHECK(e.cash >= -1e-6);
}

TEST(an_empty_signal_stream_is_handled) {
    const BacktestResult r = run(std::span<const SignalRow>{}, base_config());
    CHECK_EQ(r.trades.size(), std::size_t{0});
    CHECK_NEAR(r.ending_equity, 10000.0, 1e-9);
}
