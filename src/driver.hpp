#pragma once
//
// The backtest driver: a simulated clock, a historical data feed and a
// recorder. Nothing else.
//
// It makes no trading decision and does no accounting. Every decision belongs
// to a Strategy and every fill, fee and balance to a Broker, both supplied by
// the caller. That split is the design: a backtester that re-implements a
// strategy's rules holds a second copy of them, and second copies drift. The
// driver only replays history through the real ones and writes down what
// they did.
//
// Runs are idempotent. A run is identified by its inputs (every bar file's
// bytes, plus the strategy's and the broker's names), it writes into a
// directory named after that identity, and the same inputs produce
// byte-identical output files. There is no wall clock and no randomness
// anywhere in the loop: time is the timestamp of the bars being replayed.
//
#include "bars.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace tb::driver {

// One bar of one symbol, as the strategy and the broker see it.
struct MarketBar {
    std::string symbol;
    std::int64_t timestamp = 0;  // Unix seconds, UTC; the bar's own timestamp
    double open = 0.0;
    double high = 0.0;
    double low = 0.0;
    double close = 0.0;
    double volume = 0.0;
};

// What a strategy asks for. Positive quantity buys, negative sells.
struct Order {
    std::string symbol;
    double quantity = 0.0;
};

// What a broker reports back for an order it executed.
struct Fill {
    std::int64_t timestamp = 0;
    std::string symbol;
    double quantity = 0.0;
    double price = 0.0;
    double fee = 0.0;
};

// The broker's view of the account, recorded after every step.
struct AccountSnapshot {
    double cash = 0.0;
    double equity = 0.0;
};

// The simulated broker. The sandbox implements this; the driver never looks
// inside it.
class Broker {
public:
    virtual ~Broker() = default;
    // A stable name, part of the run's identity. Change it whenever the
    // broker's behaviour changes, or a rerun will overwrite an old result
    // with a different one under the same identity.
    virtual std::string brokerName() const = 0;
    // Called first at every step, with every bar that closed at this time.
    virtual void markPrices(std::int64_t time, std::span<const MarketBar> bars) = 0;
    // Called with the strategy's orders for this step.
    virtual std::vector<Fill> submitOrders(std::int64_t time, std::span<const Order> orders) = 0;
    virtual AccountSnapshot accountSnapshot() const = 0;
};

// The strategy under test.
class Strategy {
public:
    virtual ~Strategy() = default;
    // A stable name, part of the run's identity, for the same reason as
    // Broker::brokerName.
    virtual std::string strategyName() const = 0;
    // Called once per step with the bars that closed at `time`. It never
    // sees a bar from the future: the driver only hands over what has closed.
    virtual std::vector<Order> decideOrders(std::int64_t time, std::span<const MarketBar> bars,
                                            const AccountSnapshot& account) = 0;
};

// Every bar of every symbol on one clock, ordered by timestamp and then by
// symbol, so the order never depends on which file happened to load first.
struct Timeline {
    std::vector<MarketBar> bars;
};

Timeline buildTimeline(std::span<const Bars> histories);

struct EquityPoint {
    std::int64_t time = 0;
    AccountSnapshot account;
};

struct ReplayResult {
    std::vector<Fill> fills;
    std::vector<EquityPoint> equityCurve;
    size_t stepCount = 0;
};

// The loop. For each distinct timestamp, in order:
//   1. broker.markPrices(time, barsAtTime)
//   2. orders = strategy.decideOrders(time, barsAtTime, broker.accountSnapshot())
//   3. fills  = broker.submitOrders(time, orders)
//   4. record the fills and broker.accountSnapshot()
ReplayResult runReplay(const Timeline& timeline, Strategy& strategy, Broker& broker);

// One input file of a run: its name and a fingerprint of its bytes.
struct InputFile {
    std::string fileName;
    std::uint64_t contentFingerprint = 0;
};

struct RunManifest {
    std::string runIdentifier;  // 16 hex digits
    std::string strategyName;
    std::string brokerName;
    std::vector<InputFile> inputFiles;  // sorted by file name
    size_t barCount = 0;
    size_t stepCount = 0;
    std::int64_t firstTime = 0;
    std::int64_t lastTime = 0;
};

// 64-bit FNV-1a. Not cryptographic, and does not need to be: it only has to
// make two different inputs land in different run directories.
std::uint64_t fingerprintBytes(std::span<const char> bytes, std::uint64_t seed);

// The run's identity, from its input files and the two names. Input order
// does not matter: the files are sorted by name before they are combined.
RunManifest describeRun(std::vector<InputFile> inputFiles, const std::string& strategyName,
                        const std::string& brokerName, const Timeline& timeline,
                        const ReplayResult& result);

// Reads each file's bytes (for its fingerprint) and parses it as bars. A file
// that cannot be read or parsed is reported on stderr and skipped, and the
// function returns false so the caller knows the run is incomplete.
bool loadHistories(std::span<const std::string> filePaths, std::vector<Bars>& histories,
                   std::vector<InputFile>& inputFiles);

// Writes manifest.txt, fills.csv and equity.csv into
// <outputRoot>/<runIdentifier>/, replacing any earlier copy. Returns false
// and reports on stderr if a file cannot be written.
bool writeRun(const std::string& outputRoot, const RunManifest& manifest, const ReplayResult& result);

}  // namespace tb::driver
