#pragma once
// The backtest driver. Design, call order and output format: docs/driver.md
#include "bars.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace tb::driver {

struct MarketBar {
    std::string symbol;
    // Signed on purpose: Unix seconds, and daily histories can start before
    // 1970, which is negative. It also matches Bars::timestamp, so no cast.
    std::int64_t timestamp = 0;
    double open = 0.0;
    double high = 0.0;
    double low = 0.0;
    double close = 0.0;
    double volume = 0.0;
};

struct Order {
    std::string symbol;
    double quantity = 0.0;
};

struct Fill {
    std::int64_t timestamp = 0;
    std::string symbol;
    double quantity = 0.0;
    double price = 0.0;
    double fee = 0.0;
};

struct AccountSnapshot {
    double cash = 0.0;
    double equity = 0.0;
};

class Broker {
public:
    virtual ~Broker() = default;
    virtual std::string brokerName() const = 0;
    virtual void markPrices(std::int64_t time, std::span<const MarketBar> bars) = 0;
    virtual std::vector<Fill> submitOrders(std::int64_t time, std::span<const Order> orders) = 0;
    virtual AccountSnapshot accountSnapshot() const = 0;
};

class Strategy {
public:
    virtual ~Strategy() = default;
    virtual std::string strategyName() const = 0;
    virtual std::vector<Order> decideOrders(std::int64_t time, std::span<const MarketBar> bars,
                                            const AccountSnapshot& account) = 0;
};

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

ReplayResult runReplay(const Timeline& timeline, Strategy& strategy, Broker& broker);

struct InputFile {
    std::string fileName;
    std::uint64_t contentFingerprint = 0;
};

struct RunManifest {
    std::string runIdentifier;
    std::string strategyName;
    std::string brokerName;
    std::vector<InputFile> inputFiles;
    size_t barCount = 0;
    size_t stepCount = 0;
    std::int64_t firstTime = 0;
    std::int64_t lastTime = 0;
};

std::uint64_t fingerprintBytes(std::span<const char> bytes, std::uint64_t seed);

RunManifest describeRun(std::vector<InputFile> inputFiles, const std::string& strategyName,
                        const std::string& brokerName, const Timeline& timeline,
                        const ReplayResult& result);

bool loadHistories(std::span<const std::string> filePaths, std::vector<Bars>& histories,
                   std::vector<InputFile>& inputFiles);

bool writeRun(const std::string& outputRoot, const RunManifest& manifest, const ReplayResult& result);

}  // namespace tb::driver
