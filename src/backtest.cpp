// Backtester: replays a recorded signal stream against the portfolio rules.
//
// The signal stream is produced upstream by whatever owns the model (for the
// trading bot this repo was extracted from, that is export_signals.py, which
// scores the real trained pickles). That split is the whole point: this program
// never decides whether a symbol looks attractive, so it cannot disagree with
// production about what the model said. It only simulates what a portfolio does
// with those decisions.
//
// Advisory and offline. It reads two files and writes three.
//
#include "portfolio.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

using namespace tb;

namespace {

struct Options {
    std::string dir = "backtest_data";
    std::string out_prefix = "backtest";
    double cash = 10000.0;
    double cost_per_side = 0.0;
    std::optional<bool> trailing;   // overrides the recorded config when set
    long long min_hold = 0;         // seconds; 0 = signal exits are never delayed
    bool help = false;
};

void usage() {
    std::printf(
        "backtest - replay a recorded signal stream against the portfolio rules\n"
        "\n"
        "Usage: backtest [options]\n"
        "  --dir PATH        directory holding signals.csv and backtest_config.json\n"
        "                    (default backtest_data)\n"
        "  --out PREFIX      output prefix (default 'backtest' -> backtest_trades.csv,\n"
        "                    backtest_equity.csv)\n"
        "  --cash X          starting cash (default 10000)\n"
        "  --cost X          per-side transaction cost as a fraction of notional,\n"
        "                    e.g. 0.0005 for 5bps (default 0 - gross)\n"
        "  --trailing 0|1    override the recorded trailing-stop setting\n"
        "  -h, --help        this text\n"
        "\n"
        "Risk caps come from backtest_config.json, recorded when the signals were\n"
        "exported. Replaying against different caps than the ones the signals were\n"
        "recorded under would not be a backtest of that configuration.\n");
}

bool parse_args(int argc, char** argv, Options& o) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](const char* name) -> const char* {
            if (i + 1 >= argc) { std::fprintf(stderr, "%s needs a value\n", name); return nullptr; }
            return argv[++i];
        };
        if (a == "-h" || a == "--help") { o.help = true; return true; }
        else if (a == "--dir") { const char* v = next("--dir"); if (!v) return false; o.dir = v; }
        else if (a == "--out") { const char* v = next("--out"); if (!v) return false; o.out_prefix = v; }
        else if (a == "--cash") { const char* v = next("--cash"); if (!v) return false; o.cash = std::atof(v); }
        else if (a == "--cost") { const char* v = next("--cost"); if (!v) return false; o.cost_per_side = std::atof(v); }
        else if (a == "--trailing") { const char* v = next("--trailing"); if (!v) return false; o.trailing = (std::atoi(v) != 0); }
        else if (a == "--min-hold") { const char* v = next("--min-hold"); if (!v) return false; o.min_hold = std::atoll(v); }
        else { std::fprintf(stderr, "unknown argument: %s\n", a.c_str()); return false; }
    }
    return true;
}

// ── A deliberately small JSON reader ────────────────────────────────────────
// The config file is a flat object of numbers, booleans, strings and nulls
// written by our own exporter. Pulling in a JSON library to read that would
// cost this project its "no dependencies" property for no benefit. It is not a
// general JSON parser and does not pretend to be: nesting and arrays are simply
// not read.
class FlatJson {
public:
    bool load(const std::string& path) {
        std::ifstream in(path);
        if (!in) return false;
        std::stringstream buf;
        buf << in.rdbuf();
        const std::string s = buf.str();

        std::size_t i = 0;
        while (i < s.size()) {
            if (s[i] != '"') { ++i; continue; }
            const std::size_t key_start = ++i;
            while (i < s.size() && s[i] != '"') ++i;
            if (i >= s.size()) break;
            const std::string key = s.substr(key_start, i - key_start);
            ++i;
            while (i < s.size() && (std::isspace(static_cast<unsigned char>(s[i])) || s[i] == ':')) ++i;
            if (i >= s.size()) break;

            std::string value;
            if (s[i] == '"') {
                const std::size_t vs = ++i;
                while (i < s.size() && s[i] != '"') ++i;
                value = s.substr(vs, i - vs);
                ++i;
            } else {
                const std::size_t vs = i;
                while (i < s.size() && s[i] != ',' && s[i] != '}' && s[i] != '\n') ++i;
                value = s.substr(vs, i - vs);
                while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) value.pop_back();
            }
            values_[key] = value;
        }
        return true;
    }

    std::optional<double> number(const std::string& key) const {
        const auto it = values_.find(key);
        if (it == values_.end() || it->second.empty() || it->second == "null") return std::nullopt;
        try { return std::stod(it->second); } catch (...) { return std::nullopt; }
    }
    bool boolean(const std::string& key, bool fallback) const {
        const auto it = values_.find(key);
        if (it == values_.end()) return fallback;
        if (it->second == "true") return true;
        if (it->second == "false") return false;
        return fallback;
    }
    std::string text(const std::string& key) const {
        const auto it = values_.find(key);
        return it == values_.end() ? std::string{} : it->second;
    }

private:
    std::unordered_map<std::string, std::string> values_;
};

std::vector<std::string> split(const std::string& line) {
    std::vector<std::string> out;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= line.size(); ++i) {
        if (i == line.size() || line[i] == ',') {
            std::string f = line.substr(start, i - start);
            while (!f.empty() && (f.back() == '\r' || f.back() == ' ')) f.pop_back();
            out.push_back(std::move(f));
            start = i + 1;
        }
    }
    return out;
}

bool load_signals(const std::string& path, std::vector<SignalRow>& rows, std::string& error) {
    std::ifstream in(path);
    if (!in) { error = "cannot open " + path; return false; }

    std::string line;
    if (!std::getline(in, line)) { error = path + " is empty"; return false; }

    // Columns are located by name. A positional reader would keep "working"
    // against a reordered export and silently swap, say, threshold for price.
    std::unordered_map<std::string, int> col;
    const std::vector<std::string> header = split(line);
    for (std::size_t i = 0; i < header.size(); ++i) col[header[i]] = static_cast<int>(i);

    for (const char* required : {"timestamp", "symbol", "t212", "group", "price", "confidence",
                                 "threshold", "qualifies", "signal", "tp_pct", "sl_pct",
                                 "sl_distance", "risk", "risk_scale"}) {
        if (!col.count(required)) {
            error = std::string(path) + ": missing required column '" + required + "'";
            return false;
        }
    }
    const bool has_trend_sl = col.count("trend_sl_pct") > 0;

    std::size_t lineno = 1;
    while (std::getline(in, line)) {
        ++lineno;
        if (line.empty()) continue;
        const std::vector<std::string> f = split(line);
        auto at = [&](const char* name) -> const std::string& { return f[static_cast<std::size_t>(col[name])]; };
        if (f.size() < header.size()) {
            error = path + ":" + std::to_string(lineno) + ": expected " +
                    std::to_string(header.size()) + " fields, got " + std::to_string(f.size());
            return false;
        }
        SignalRow r;
        try {
            r.timestamp = std::stoll(at("timestamp"));
            r.symbol = at("symbol");
            r.ticker = at("t212");
            r.group = at("group");
            r.price = std::stod(at("price"));
            r.confidence = std::stod(at("confidence"));
            r.threshold = std::stod(at("threshold"));
            r.qualifies = at("qualifies") == "1" || at("qualifies") == "True";
            r.is_long = at("signal") == "LONG";
            r.tp_pct = std::stod(at("tp_pct"));
            r.sl_pct = std::stod(at("sl_pct"));
            r.sl_distance = std::stod(at("sl_distance"));
            r.trend_sl_pct = has_trend_sl ? std::stod(at("trend_sl_pct")) : r.sl_pct;
            r.risk = std::stod(at("risk"));
            r.risk_scale = std::stod(at("risk_scale"));
        } catch (const std::exception& e) {
            error = path + ":" + std::to_string(lineno) + ": " + e.what();
            return false;
        }
        rows.push_back(std::move(r));
    }
    return true;
}

double annualised_return(double start, double end, double days) {
    if (start <= 0.0 || end <= 0.0 || days <= 0.0) return 0.0;
    return (std::pow(end / start, 365.0 / days) - 1.0) * 100.0;
}

}  // namespace

int main(int argc, char** argv) {
    Options opt;
    if (!parse_args(argc, argv, opt)) return 2;
    if (opt.help) { usage(); return 0; }

    const std::string signals_path = opt.dir + "/signals.csv";
    const std::string config_path = opt.dir + "/backtest_config.json";

    std::vector<SignalRow> rows;
    std::string error;
    if (!load_signals(signals_path, rows, error)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }
    if (rows.empty()) {
        std::fprintf(stderr, "%s contains no signal rows\n", signals_path.c_str());
        return 1;
    }

    BacktestConfig config;
    config.starting_cash = opt.cash;
    config.cost_per_side = opt.cost_per_side;

    FlatJson json;
    if (json.load(config_path)) {
        auto num = [&](const char* k, double fallback) { return json.number(k).value_or(fallback); };
        config.max_positions_per_group = static_cast<int>(num("max_positions_per_group", 2));
        config.max_open_positions = static_cast<int>(num("max_open_positions", 10));
        config.max_position_allocation = num("max_position_allocation", 0.25);
        config.max_portfolio_risk_pct = num("max_portfolio_risk_pct", 0.08);
        config.max_daily_loss = num("max_daily_loss", 0.03);
        config.portfolio_heat_scaling = json.boolean("portfolio_heat_scaling", false);
        config.portfolio_heat_min_scale = num("portfolio_heat_min_scale", 0.4);
        config.trailing_stop_enabled = json.boolean("trailing_stop_enabled", false);
        config.trailing_activation_pct = json.number("trailing_activation_pct");
        config.trailing_distance_pct = json.number("trailing_distance_pct");
        // Absent in a config recorded before the profit lock existed; zero
        // (off) is then the faithful reading, not a guess.
        config.profit_lock_at = num("profit_lock_at", 0.0);
        config.profit_lock_give_back = num("profit_lock_give_back", 0.0);
        config.allow_low_edge_trading = json.boolean("allow_low_edge_trading", false);
    } else {
        // Loudly, because silently substituting defaults for the caps a run was
        // recorded under produces a confident number for a configuration that
        // was never tested.
        std::fprintf(stderr,
                     "WARNING: could not read %s - falling back to built-in defaults. "
                     "These are almost certainly NOT the caps these signals were recorded "
                     "under, and the result will not describe that configuration.\n",
                     config_path.c_str());
    }
    if (opt.trailing) config.trailing_stop_enabled = *opt.trailing;
    config.min_hold_seconds = opt.min_hold;

    const BacktestResult r = run(rows, config);

    // ── Report ───────────────────────────────────────────────────────────
    const double days = static_cast<double>(rows.back().timestamp - rows.front().timestamp) / 86400.0;
    const double total_return = (r.ending_equity / r.starting_cash - 1.0) * 100.0;

    int wins = 0;
    double gross_win = 0.0, gross_loss = 0.0, total_costs = 0.0;
    for (const Trade& t : r.trades) {
        if (t.pnl > 0) { ++wins; gross_win += t.pnl; } else { gross_loss -= t.pnl; }
        total_costs += t.costs;
    }
    const std::size_t n = r.trades.size();

    std::printf("\n=== Backtest ===\n");
    std::printf("signals            %zu rows, %zu symbols, %.0f days\n", rows.size(),
                [&] {
                    std::unordered_map<std::string, int> seen;
                    for (const SignalRow& s : rows) seen[s.ticker] = 1;
                    return seen.size();
                }(),
                days);
    std::printf("config             group cap %d | alloc %.0f%% | portfolio risk %.1f%% | "
                "daily loss %.1f%% | trailing %s | low-edge %s\n",
                config.max_positions_per_group, config.max_position_allocation * 100.0,
                config.max_portfolio_risk_pct * 100.0, config.max_daily_loss * 100.0,
                config.trailing_stop_enabled ? "on" : "off",
                config.allow_low_edge_trading ? "on" : "off");
    if (opt.cost_per_side > 0.0)
        std::printf("costs              %.4f%% per side (%.2f total)\n", opt.cost_per_side * 100.0, total_costs);

    std::printf("\nstarting cash      %10.2f\n", r.starting_cash);
    std::printf("ending equity      %10.2f\n", r.ending_equity);
    std::printf("total return       %10.2f%%\n", total_return);
    std::printf("annualised         %10.2f%%\n", annualised_return(r.starting_cash, r.ending_equity, days));
    std::printf("max drawdown       %10.2f%%\n", r.max_drawdown * 100.0);
    std::printf("kill-switch days   %10d\n", r.kill_switch_days);

    std::printf("\ntrades             %10zu\n", n);
    if (n) {
        std::printf("win rate           %10.1f%%\n", 100.0 * wins / static_cast<double>(n));
        std::printf("profit factor      %10.2f\n", gross_loss > 0 ? gross_win / gross_loss : 0.0);
        std::printf("avg pnl per trade  %10.2f\n",
                    (gross_win - gross_loss) / static_cast<double>(n));
    }

    std::printf("\nexits:\n");
    for (const auto& [reason, count] : r.exit_reasons) std::printf("  %-14s %6d\n", reason.c_str(), count);

    // Why signals did not become trades. Without this a run reports "4 trades"
    // out of 14,000 LONG signals and gives you no way to find out why.
    std::printf("\nLONG signals       %10lld\n", static_cast<long long>(r.signals_long));
    std::printf("  already held     %10lld\n", static_cast<long long>(r.blocked_already_held));
    std::printf("  not qualifying   %10lld\n", static_cast<long long>(r.blocked_not_qualifying));
    std::printf("  group cap        %10lld\n", static_cast<long long>(r.blocked_group_cap));
    std::printf("  portfolio risk   %10lld\n", static_cast<long long>(r.blocked_risk_cap));
    std::printf("  zero size/cash   %10lld\n", static_cast<long long>(r.blocked_zero_size));
    std::printf("  kill switch      %10lld\n", static_cast<long long>(r.blocked_kill_switch));

    const std::string trades_path = opt.out_prefix + "_trades.csv";
    if (std::FILE* f = std::fopen(trades_path.c_str(), "w")) {
        std::fprintf(f, "symbol,ticker,opened_at,closed_at,quantity,entry_price,exit_price,pnl,costs,exit_reason\n");
        for (const Trade& t : r.trades) {
            std::fprintf(f, "%s,%s,%lld,%lld,%.4f,%.6f,%.6f,%.4f,%.4f,%s\n", t.symbol.c_str(),
                         t.ticker.c_str(), static_cast<long long>(t.opened_at),
                         static_cast<long long>(t.closed_at), t.quantity, t.entry_price,
                         t.exit_price, t.pnl, t.costs, t.exit_reason.c_str());
        }
        std::fclose(f);
    }

    const std::string equity_path = opt.out_prefix + "_equity.csv";
    if (std::FILE* f = std::fopen(equity_path.c_str(), "w")) {
        std::fprintf(f, "timestamp,equity,cash,open_positions\n");
        for (const EquityPoint& e : r.equity_curve) {
            std::fprintf(f, "%lld,%.4f,%.4f,%d\n", static_cast<long long>(e.timestamp), e.equity,
                         e.cash, e.open_positions);
        }
        std::fclose(f);
    }

    std::printf("\nwrote %s and %s\n", trades_path.c_str(), equity_path.c_str());
    return 0;
}
