#include "harness.hpp"

#include "driver.hpp"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace drivertest {

tb::Bars makeHistory(const std::string& symbol, const std::vector<std::int64_t>& timestamps,
                     const std::vector<double>& closes) {
    tb::Bars history;
    history.symbol = symbol;
    history.timestamp = timestamps;
    history.close = closes;
    history.open = closes;
    history.high = closes;
    history.low = closes;
    history.volume = std::vector<double>(closes.size(), 1.0);
    return history;
}

// Records every call in order, and fills any order at the last marked close.
class RecordingBroker : public tb::driver::Broker {
public:
    std::vector<std::string> calls;
    std::vector<tb::driver::MarketBar> lastBars;
    double cash = 1000.0;

    std::string brokerName() const override {
        return "recording-broker-v1";
    }

    void markPrices(std::int64_t, std::span<const tb::driver::MarketBar> bars) override {
        calls.push_back("markPrices");
        lastBars.assign(bars.begin(), bars.end());
    }

    std::vector<tb::driver::Fill> submitOrders(std::int64_t time,
                                               std::span<const tb::driver::Order> orders) override {
        calls.push_back("submitOrders");
        std::vector<tb::driver::Fill> fills;
        for (const tb::driver::Order& order : orders) {
            for (const tb::driver::MarketBar& bar : lastBars) {
                if (bar.symbol == order.symbol) {
                    tb::driver::Fill fill;
                    fill.timestamp = time;
                    fill.symbol = order.symbol;
                    fill.quantity = order.quantity;
                    fill.price = bar.close;
                    cash -= order.quantity * bar.close;
                    fills.push_back(fill);
                }
            }
        }
        return fills;
    }

    tb::driver::AccountSnapshot accountSnapshot() const override {
        tb::driver::AccountSnapshot snapshot;
        snapshot.cash = cash;
        snapshot.equity = cash;
        return snapshot;
    }
};

// Buys one unit of `symbolToBuy` at the first step only, and remembers every
// bar it was shown.
class RecordingStrategy : public tb::driver::Strategy {
public:
    explicit RecordingStrategy(RecordingBroker& brokerToLog, std::string symbolToBuy)
        : broker(brokerToLog), symbol(std::move(symbolToBuy)) {}

    std::vector<std::int64_t> decisionTimes;
    std::vector<tb::driver::MarketBar> barsSeen;

    std::string strategyName() const override {
        return "recording-strategy-v1";
    }

    std::vector<tb::driver::Order> decideOrders(std::int64_t time, std::span<const tb::driver::MarketBar> bars,
                                                const tb::driver::AccountSnapshot&) override {
        broker.calls.push_back("decideOrders");
        decisionTimes.push_back(time);
        barsSeen.insert(barsSeen.end(), bars.begin(), bars.end());
        std::vector<tb::driver::Order> orders;
        if (1 == decisionTimes.size()) {
            tb::driver::Order order;
            order.symbol = symbol;
            order.quantity = 1.0;
            orders.push_back(order);
        }
        return orders;
    }

private:
    RecordingBroker& broker;
    std::string symbol;
};

std::vector<tb::Bars> twoSymbols() {
    std::vector<tb::Bars> histories;
    histories.push_back(makeHistory("BBB", {100, 200}, {10.0, 11.0}));
    histories.push_back(makeHistory("AAA", {200, 300}, {20.0, 21.0}));
    return histories;
}

std::string readWholeFile(const std::filesystem::path& filePath) {
    std::ifstream input(filePath, std::ios::binary);
    return std::string{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

}  // namespace drivertest

TEST(the_timeline_orders_by_time_then_symbol) {
    const std::vector<tb::Bars> histories = drivertest::twoSymbols();
    const tb::driver::Timeline timeline = tb::driver::buildTimeline(histories);
    CHECK_EQ(timeline.bars.size(), size_t{4});
    CHECK_EQ(timeline.bars[0].symbol, std::string("BBB"));
    CHECK_EQ(timeline.bars[0].timestamp, std::int64_t{100});
    // Two bars at t=200: the tie is broken by symbol, never by load order.
    CHECK_EQ(timeline.bars[1].symbol, std::string("AAA"));
    CHECK_EQ(timeline.bars[2].symbol, std::string("BBB"));
    CHECK_EQ(timeline.bars[3].timestamp, std::int64_t{300});
}

TEST(each_step_hands_over_only_bars_that_have_closed) {
    const tb::driver::Timeline timeline = tb::driver::buildTimeline(drivertest::twoSymbols());
    drivertest::RecordingBroker broker;
    drivertest::RecordingStrategy strategy(broker, "BBB");
    const tb::driver::ReplayResult result = tb::driver::runReplay(timeline, strategy, broker);

    CHECK_EQ(result.stepCount, size_t{3});
    CHECK_EQ(strategy.decisionTimes.size(), size_t{3});
    // No look-ahead: every bar shown at a step is a bar of exactly that step.
    size_t barIndex = 0;
    for (const std::int64_t time : strategy.decisionTimes) {
        while (barIndex < strategy.barsSeen.size() && time == strategy.barsSeen[barIndex].timestamp) {
            ++barIndex;
        }
    }
    CHECK_EQ(barIndex, strategy.barsSeen.size());
    CHECK_EQ(strategy.barsSeen.size(), size_t{4});
}

TEST(the_broker_is_marked_before_the_strategy_decides) {
    const tb::driver::Timeline timeline = tb::driver::buildTimeline(drivertest::twoSymbols());
    drivertest::RecordingBroker broker;
    drivertest::RecordingStrategy strategy(broker, "BBB");
    tb::driver::runReplay(timeline, strategy, broker);

    const std::vector<std::string> oneStep = {"markPrices", "decideOrders", "submitOrders"};
    CHECK_EQ(broker.calls.size(), size_t{9});
    for (size_t callIndex = 0; callIndex < broker.calls.size(); ++callIndex) {
        CHECK_EQ(broker.calls[callIndex], oneStep[callIndex % oneStep.size()]);
    }
}

TEST(orders_reach_the_broker_and_its_fills_are_recorded) {
    const tb::driver::Timeline timeline = tb::driver::buildTimeline(drivertest::twoSymbols());
    drivertest::RecordingBroker broker;
    drivertest::RecordingStrategy strategy(broker, "BBB");
    const tb::driver::ReplayResult result = tb::driver::runReplay(timeline, strategy, broker);

    CHECK_EQ(result.fills.size(), size_t{1});
    CHECK_EQ(result.fills[0].symbol, std::string("BBB"));
    CHECK_EQ(result.fills[0].timestamp, std::int64_t{100});
    CHECK_NEAR(result.fills[0].price, 10.0, 1e-12);
    // One equity point per step, each the broker's own snapshot.
    CHECK_EQ(result.equityCurve.size(), size_t{3});
    CHECK_NEAR(result.equityCurve.back().account.cash, 990.0, 1e-12);
}

TEST(the_fingerprint_matches_published_fnv1a_values) {
    constexpr std::uint64_t fnvOffsetBasis = 14695981039346656037ULL;
    const std::string empty;
    const std::string letterA = "a";
    CHECK_EQ(tb::driver::fingerprintBytes(std::span<const char>(empty.data(), empty.size()), fnvOffsetBasis),
             std::uint64_t{0xcbf29ce484222325ULL});
    CHECK_EQ(tb::driver::fingerprintBytes(std::span<const char>(letterA.data(), letterA.size()), fnvOffsetBasis),
             std::uint64_t{0xaf63dc4c8601ec8cULL});
}

TEST(run_identity_ignores_input_order_but_not_input_content) {
    const tb::driver::Timeline timeline = tb::driver::buildTimeline(drivertest::twoSymbols());
    const tb::driver::ReplayResult emptyResult;
    const tb::driver::InputFile first{"AAA__1h.csv", 111};
    const tb::driver::InputFile second{"BBB__1h.csv", 222};

    const std::string forward =
        tb::driver::describeRun({first, second}, "s", "b", timeline, emptyResult).runIdentifier;
    const std::string reversed =
        tb::driver::describeRun({second, first}, "s", "b", timeline, emptyResult).runIdentifier;
    CHECK_EQ(forward, reversed);
    CHECK_EQ(forward.size(), size_t{16});

    const tb::driver::InputFile changed{"BBB__1h.csv", 223};
    CHECK(forward != tb::driver::describeRun({first, changed}, "s", "b", timeline, emptyResult).runIdentifier);
    CHECK(forward != tb::driver::describeRun({first, second}, "s2", "b", timeline, emptyResult).runIdentifier);
    CHECK(forward != tb::driver::describeRun({first, second}, "s", "b2", timeline, emptyResult).runIdentifier);
}

TEST(rerunning_the_same_inputs_writes_identical_files) {
    const std::filesystem::path outputRoot = std::filesystem::temp_directory_path() / "tb_driver_idempotency_test";
    std::filesystem::remove_all(outputRoot);

    std::vector<std::string> firstContents;
    std::string firstIdentifier;
    for (int runNumber = 0; runNumber < 2; ++runNumber) {
        const tb::driver::Timeline timeline = tb::driver::buildTimeline(drivertest::twoSymbols());
        drivertest::RecordingBroker broker;
        drivertest::RecordingStrategy strategy(broker, "BBB");
        const tb::driver::ReplayResult result = tb::driver::runReplay(timeline, strategy, broker);
        const tb::driver::RunManifest manifest = tb::driver::describeRun(
            {{"AAA__1h.csv", 1}, {"BBB__1h.csv", 2}}, strategy.strategyName(), broker.brokerName(), timeline,
            result);
        CHECK(tb::driver::writeRun(outputRoot.string(), manifest, result));

        std::vector<std::string> contents;
        for (const char* fileName : {"manifest.txt", "fills.csv", "equity.csv"}) {
            contents.push_back(drivertest::readWholeFile(outputRoot / manifest.runIdentifier / fileName));
        }
        if (0 == runNumber) {
            firstContents = contents;
            firstIdentifier = manifest.runIdentifier;
        } else {
            CHECK_EQ(manifest.runIdentifier, firstIdentifier);
            CHECK(contents == firstContents);
        }
    }
    CHECK(firstContents[1].find("100,BBB,1,10,0") != std::string::npos);
    std::filesystem::remove_all(outputRoot);
}

TEST(a_file_that_does_not_parse_is_skipped_and_reported) {
    const std::filesystem::path directory = std::filesystem::temp_directory_path() / "tb_driver_load_test";
    std::filesystem::remove_all(directory);
    std::filesystem::create_directories(directory);
    {
        std::ofstream good(directory / "GOOD__1h.csv", std::ios::binary);
        good << "Datetime,open,high,low,close,volume\n"
                "2026-06-10 18:10:00+00:00,1,1,1,1,100\n";
        std::ofstream bad(directory / "BAD__1h.csv", std::ios::binary);
        bad << "Datetime,open,high,low,volume\n2026-06-10,1,2,0.5,100\n";  // no close column
    }
    const std::vector<std::string> filePaths = {(directory / "GOOD__1h.csv").string(),
                                                (directory / "BAD__1h.csv").string()};
    std::vector<tb::Bars> histories;
    std::vector<tb::driver::InputFile> inputFiles;
    CHECK(!tb::driver::loadHistories(filePaths, histories, inputFiles));
    CHECK_EQ(histories.size(), size_t{1});
    CHECK_EQ(histories[0].symbol, std::string("GOOD"));
    CHECK_EQ(inputFiles.size(), size_t{1});
    std::filesystem::remove_all(directory);
}
