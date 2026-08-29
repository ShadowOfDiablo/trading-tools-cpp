#pragma once
//
// Portfolio simulation: the rules a signal stream is replayed against.
//
// This is deliberately a library rather than code inside main(). Every rule
// here is a place where a plausible-looking mistake changes the reported
// return without changing anything visible, so each one is unit-tested against
// a hand-worked example.
//
// The division of labour matters: the ML lives upstream. This file never
// decides whether a symbol looks attractive - it is handed that decision, per
// bar, already made, and its whole job is what a portfolio does with it:
// sizing, stops, caps, and the kill switch.
//
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace tb {

// One bar's already-scored view of one symbol.
struct SignalRow {
    std::int64_t timestamp = 0;
    std::string symbol;
    std::string ticker;      // the broker's own ticker; positions are keyed on this
    std::string group;       // correlation group, for the per-group cap
    double price = 0.0;
    double confidence = 0.0;
    double threshold = 0.0;
    bool qualifies = false;  // did the model clear its own statistical bar
    bool is_long = false;    // confidence >= threshold
    double natr = 0.0;
    double tp_pct = 0.0;
    double sl_pct = 0.0;
    double sl_distance = 0.0;
    double trend_sl_pct = 0.0;  // the stop OTHER positions are valued at, see below
    double risk = 0.01;
    double risk_scale = 1.0;
};

struct BacktestConfig {
    int max_positions_per_group = 2;
    int max_open_positions = 10;       // heat-scaling reference, NOT a hard cap
    double max_position_allocation = 0.25;
    double max_portfolio_risk_pct = 0.08;
    double max_daily_loss = 0.03;
    bool portfolio_heat_scaling = false;
    double portfolio_heat_min_scale = 0.4;
    bool trailing_stop_enabled = false;
    // Absent means "use the regime's own tp_pct / sl_pct", which is what the
    // live configuration does when these are unset.
    std::optional<double> trailing_activation_pct;
    std::optional<double> trailing_distance_pct;
    bool allow_low_edge_trading = false;
    double starting_cash = 10000.0;
    // Charged on both sides of every round trip, as a fraction of notional.
    // Zero by default so a run reports the strategy's gross behaviour unless
    // costs are asked for explicitly.
    double cost_per_side = 0.0;
};

struct Position {
    std::string ticker;
    std::string symbol;
    std::string group;
    double quantity = 0.0;
    double average_price = 0.0;
    double current_price = 0.0;
    double peak_price = 0.0;     // best price seen since entry; drives the trailing stop
    double trend_sl_pct = 0.01;  // this symbol's natr=0 stop, for the risk cap
    std::int64_t opened_at = 0;
};

struct Trade {
    std::string symbol;
    std::string ticker;
    std::int64_t opened_at = 0;
    std::int64_t closed_at = 0;
    double quantity = 0.0;
    double entry_price = 0.0;
    double exit_price = 0.0;
    double pnl = 0.0;
    double costs = 0.0;
    std::string exit_reason;
};

struct EquityPoint {
    std::int64_t timestamp = 0;
    double equity = 0.0;
    double cash = 0.0;
    int open_positions = 0;
};

struct BacktestResult {
    std::vector<Trade> trades;
    std::vector<EquityPoint> equity_curve;
    double starting_cash = 0.0;
    double ending_equity = 0.0;
    double max_drawdown = 0.0;      // fraction, peak-to-trough on the equity curve
    int kill_switch_days = 0;
    // Counts of why a LONG signal did not become a trade. A backtest that
    // reports "3 trades" without saying that 40,000 signals were blocked by one
    // cap is not telling you what you need to know.
    std::int64_t signals_long = 0;
    std::int64_t blocked_not_qualifying = 0;
    std::int64_t blocked_group_cap = 0;
    std::int64_t blocked_risk_cap = 0;
    std::int64_t blocked_zero_size = 0;
    std::int64_t blocked_kill_switch = 0;
    std::int64_t blocked_already_held = 0;
    std::unordered_map<std::string, int> exit_reasons;
};

// ── The individual rules, exposed so they can be tested in isolation ─────────

// Fraction-of-cash sizing, scaled by the confidence margin over the threshold
// and by portfolio heat, then clamped by a hard per-position allocation
// ceiling. Quantity is floored to 4 decimals, matching fractional-share
// handling.
double position_size(double cash, double price, double risk_fraction,
                     std::optional<double> confidence, std::optional<double> threshold,
                     std::optional<double> sl_distance, double max_position_pct,
                     int open_positions, int max_open_positions,
                     bool heat_scaling, double heat_min_scale);

// Returns the exit reason ("STOP_LOSS", "TAKE_PROFIT", "TRAILING_STOP") or
// nullopt to hold.
//
// With trailing enabled and the position far enough into profit, the fixed
// take-profit is deliberately NOT taken - the position rides until it gives
// back trailing_distance_pct off its own PEAK. The fixed stop-loss, measured
// off entry rather than off the peak, still applies underneath as a floor.
std::optional<std::string> check_position_safety(
    const Position& pos, double tp_pct, double sl_pct, bool trailing_enabled,
    std::optional<double> trailing_activation_pct, std::optional<double> trailing_distance_pct);

// Total capital at risk if every open position hit its stop at once.
//
// Note the asymmetry, which mirrors the live implementation: the position being
// opened is valued at its CURRENT regime stop, while positions already open are
// valued at their natr=0 (wider, TREND_FOLLOW) stop, because the live caller
// has no current NATR for them. Valuing them at the tighter stop would
// understate portfolio risk and let the cap through trades it should refuse.
double total_portfolio_risk_cash(const std::vector<Position>& positions);

BacktestResult run(std::span<const SignalRow> rows, const BacktestConfig& config);

}  // namespace tb
