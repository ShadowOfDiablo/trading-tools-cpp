#include "portfolio.hpp"

#include <algorithm>
#include <cmath>

namespace tb {
namespace {

// UTC calendar day. The kill switch resets on a new day, so this decides when
// the drawdown budget is refreshed.
std::int64_t utc_day(std::int64_t timestamp) noexcept {
    // Floor division: negative timestamps (pre-1970) would otherwise round
    // toward zero and put two different days in the same bucket.
    return timestamp >= 0 ? timestamp / 86400 : -(((-timestamp) + 86399) / 86400);
}

double positions_value(const std::vector<Position>& positions) noexcept {
    double total = 0.0;
    for (const Position& p : positions) total += p.quantity * p.current_price;
    return total;
}

std::vector<Position>::iterator find_position(std::vector<Position>& positions,
                                              const std::string& ticker) {
    return std::find_if(positions.begin(), positions.end(),
                        [&](const Position& p) { return p.ticker == ticker; });
}

}  // namespace

double position_size(double cash, double price, double risk_fraction,
                     std::optional<double> confidence, std::optional<double> threshold,
                     std::optional<double> sl_distance, double max_position_pct,
                     int open_positions, int max_open_positions,
                     bool heat_scaling, double heat_min_scale) {
    if (price <= 0.0 || cash <= 0.0) return 0.0;

    double max_risk_cash = cash * risk_fraction;

    if (confidence && threshold) {
        const double span = 1.0 - *threshold;
        if (span > 0.0) {
            const double scale = 0.5 + 0.5 * ((*confidence - *threshold) / span);
            max_risk_cash *= std::max(0.1, std::min(1.0, scale));
        }
    }

    if (heat_scaling && max_open_positions > 0) {
        const double heat_ratio =
            std::min(1.0, static_cast<double>(std::max(0, open_positions)) /
                              static_cast<double>(max_open_positions));
        max_risk_cash *= 1.0 - heat_ratio * (1.0 - heat_min_scale);
    }

    // Shares implied by the cash-at-risk budget and the distance to the stop.
    // With no stop distance, fall back to assuming a 1% move.
    const double target_shares = (sl_distance && *sl_distance > 0.0)
                                     ? max_risk_cash / *sl_distance
                                     : max_risk_cash / (price * 0.010);

    // Hard allocation ceiling: notional cannot exceed max_position_pct of cash,
    // no matter how tight the stop makes the risk-based size look.
    const double max_allowed_shares = (cash * max_position_pct) / price;

    const double final_shares = std::min(target_shares, max_allowed_shares);
    return std::floor(final_shares * 10000.0) / 10000.0;
}

std::optional<std::string> check_position_safety(
    const Position& pos, double tp_pct, double sl_pct, bool trailing_enabled,
    std::optional<double> trailing_activation_pct, std::optional<double> trailing_distance_pct) {
    const double initial = pos.average_price;
    const double current = pos.current_price;
    if (initial <= 0.0) return std::nullopt;

    const double trade_return = (current / initial) - 1.0;

    if (trailing_enabled && pos.peak_price > initial) {
        const double peak_gain = (pos.peak_price / initial) - 1.0;
        const double activation = trailing_activation_pct.value_or(tp_pct);
        if (peak_gain >= activation) {
            const double distance = trailing_distance_pct.value_or(sl_pct);
            if (current <= pos.peak_price * (1.0 - distance)) return "TRAILING_STOP";
            // Trailing is armed but not hit. The fixed stop off ENTRY is still
            // the floor; the fixed take-profit is deliberately skipped so a
            // running trend is not capped at the first regime target.
            if (trade_return <= -sl_pct) return "STOP_LOSS";
            return std::nullopt;
        }
    }

    if (trade_return <= -sl_pct) return "STOP_LOSS";
    if (trade_return >= tp_pct) return "TAKE_PROFIT";
    return std::nullopt;
}

double total_portfolio_risk_cash(const std::vector<Position>& positions) {
    double total = 0.0;
    for (const Position& p : positions) {
        if (p.quantity <= 0.0 || p.current_price <= 0.0) continue;
        total += p.quantity * p.current_price * p.trend_sl_pct;
    }
    return total;
}

BacktestResult run(std::span<const SignalRow> rows, const BacktestConfig& config) {
    BacktestResult result;
    result.starting_cash = config.starting_cash;
    result.ending_equity = config.starting_cash;
    if (rows.empty()) return result;

    double cash = config.starting_cash;
    std::vector<Position> positions;

    // Last seen price per ticker, so equity can be marked to market even on
    // bars belonging to other symbols.
    std::unordered_map<std::string, double> last_price;

    std::int64_t current_day = utc_day(rows.front().timestamp);
    double day_start_equity = config.starting_cash;
    bool killed_today = false;

    double peak_equity = config.starting_cash;

    auto equity_now = [&]() { return cash + positions_value(positions); };

    auto close_position = [&](std::vector<Position>::iterator it, double price,
                              std::int64_t timestamp, const std::string& reason) {
        const double gross = it->quantity * price;
        const double cost = gross * config.cost_per_side;
        cash += gross - cost;

        Trade t;
        t.symbol = it->symbol;
        t.ticker = it->ticker;
        t.opened_at = it->opened_at;
        t.closed_at = timestamp;
        t.quantity = it->quantity;
        t.entry_price = it->average_price;
        t.exit_price = price;
        // Entry cost was already charged when the position opened; both sides
        // are attributed to the trade so per-trade P&L is the true net.
        t.costs = cost + it->quantity * it->average_price * config.cost_per_side;
        t.pnl = it->quantity * (price - it->average_price) - t.costs;
        t.exit_reason = reason;
        result.trades.push_back(std::move(t));
        result.exit_reasons[reason] += 1;

        positions.erase(it);
    };

    for (std::size_t i = 0; i < rows.size(); ++i) {
        const SignalRow& row = rows[i];
        if (row.price <= 0.0) continue;

        last_price[row.ticker] = row.price;

        // ── Roll the day, resetting the drawdown budget ──────────────────
        const std::int64_t day = utc_day(row.timestamp);
        if (day != current_day) {
            current_day = day;
            day_start_equity = equity_now();
            killed_today = false;
        }

        // Mark this symbol to market before any rule looks at it.
        for (Position& p : positions) {
            if (p.ticker == row.ticker) {
                p.current_price = row.price;
                p.peak_price = std::max(p.peak_price, row.price);
            }
        }

        // ── Kill switch, on total equity (cash + positions) ──────────────
        if (!killed_today && day_start_equity > 0.0) {
            const double drawdown = (day_start_equity - equity_now()) / day_start_equity;
            if (drawdown >= config.max_daily_loss) {
                killed_today = true;
                ++result.kill_switch_days;
                // Flatten everything, valuing each position at its own last
                // known price rather than this row's (which belongs to one
                // symbol only).
                while (!positions.empty()) {
                    auto it = positions.begin();
                    const auto known = last_price.find(it->ticker);
                    const double px = (known != last_price.end()) ? known->second : it->current_price;
                    close_position(it, px, row.timestamp, "KILL_SWITCH");
                }
            }
        }

        auto held = find_position(positions, row.ticker);
        const bool in_position = held != positions.end();

        // ── Exits come before entries, exactly as the live loop orders it ─
        if (in_position) {
            const auto reason = check_position_safety(
                *held, row.tp_pct, row.sl_pct, config.trailing_stop_enabled,
                config.trailing_activation_pct, config.trailing_distance_pct);
            if (reason) {
                close_position(held, row.price, row.timestamp, *reason);
                result.equity_curve.push_back({row.timestamp, equity_now(), cash,
                                               static_cast<int>(positions.size())});
                continue;
            }
        }

        if (row.is_long) ++result.signals_long;

        if (row.is_long && !in_position) {
            // An unqualified model's LONG is only actionable when low-edge
            // trading is switched on, and then at reduced size.
            if (!row.qualifies && !config.allow_low_edge_trading) {
                ++result.blocked_not_qualifying;
            } else if (killed_today) {
                ++result.blocked_kill_switch;
            } else {
                // Correlation-group cap: do not stack exposure in names that
                // move together.
                const int group_open = static_cast<int>(std::count_if(
                    positions.begin(), positions.end(),
                    [&](const Position& p) { return p.group == row.group; }));
                if (group_open >= config.max_positions_per_group) {
                    ++result.blocked_group_cap;
                } else {
                    const double qty = position_size(
                        cash, row.price, row.risk * row.risk_scale, row.confidence,
                        row.threshold, row.sl_distance, config.max_position_allocation,
                        static_cast<int>(positions.size()), config.max_open_positions,
                        config.portfolio_heat_scaling, config.portfolio_heat_min_scale);

                    if (qty <= 0.0) {
                        ++result.blocked_zero_size;
                    } else {
                        // Portfolio-wide capital-at-risk cap, independent of
                        // position count.
                        const double equity = cash + positions_value(positions);
                        const double incoming = qty * row.sl_distance;
                        const bool has_capacity =
                            equity > 0.0 &&
                            (total_portfolio_risk_cash(positions) + incoming) <=
                                equity * config.max_portfolio_risk_pct;

                        const double notional = qty * row.price;
                        const double entry_cost = notional * config.cost_per_side;

                        if (!has_capacity) {
                            ++result.blocked_risk_cap;
                        } else if (notional + entry_cost > cash) {
                            // Sizing is a fraction of cash, so this is rare -
                            // but a backtest that lets cash go negative is
                            // reporting leverage the account cannot take.
                            ++result.blocked_zero_size;
                        } else {
                            cash -= notional + entry_cost;
                            Position p;
                            p.ticker = row.ticker;
                            p.symbol = row.symbol;
                            p.group = row.group;
                            p.quantity = qty;
                            p.average_price = row.price;
                            p.current_price = row.price;
                            p.peak_price = row.price;
                            p.trend_sl_pct = row.trend_sl_pct > 0.0 ? row.trend_sl_pct : row.sl_pct;
                            p.opened_at = row.timestamp;
                            positions.push_back(std::move(p));
                        }
                    }
                }
            }
        } else if (row.is_long && in_position) {
            ++result.blocked_already_held;
        } else if (!row.is_long && in_position && row.qualifies) {
            // A FLAT from an UNQUALIFIED engine means "no trustworthy opinion",
            // not "get out" - so it must never force a close on its own.
            close_position(held, row.price, row.timestamp, "ML_EXIT");
        }

        const double eq = equity_now();
        peak_equity = std::max(peak_equity, eq);
        if (peak_equity > 0.0) {
            result.max_drawdown = std::max(result.max_drawdown, (peak_equity - eq) / peak_equity);
        }
        result.equity_curve.push_back({row.timestamp, eq, cash, static_cast<int>(positions.size())});
    }

    // Close whatever is still open at the final price, so the reported return
    // is realised rather than partly paper.
    const std::int64_t last_ts = rows.back().timestamp;
    while (!positions.empty()) {
        auto it = positions.begin();
        const auto known = last_price.find(it->ticker);
        const double px = (known != last_price.end()) ? known->second : it->current_price;
        close_position(it, px, last_ts, "END_OF_DATA");
    }

    result.ending_equity = cash;
    return result;
}

}  // namespace tb
