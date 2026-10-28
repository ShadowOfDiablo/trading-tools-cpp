#include "driver.hpp"

#include <algorithm>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string_view>
#include <system_error>
#include <utility>

namespace tb::driver {

namespace formatting {

// FNV-1a, 64-bit (http://www.isthe.com/chongo/tech/comp/fnv/).
constexpr std::uint64_t fnvOffsetBasis = 14695981039346656037ULL;
constexpr std::uint64_t fnvPrime = 1099511628211ULL;

constexpr std::string_view hexDigits = "0123456789abcdef";
constexpr std::uint64_t hexDigitMask = 0xF;
constexpr int bitsPerHexDigit = 4;
constexpr int hexDigitsPerFingerprint = 16;

// Large enough for any double or int64 that std::to_chars can produce.
constexpr size_t numberBufferSize = 64;

// The separator hashed between fields, so that ("ab", "c") and ("a", "bc")
// never produce the same run identity.
constexpr std::string_view fieldSeparator = "\x1f";

std::string toHex(std::uint64_t value) {
    std::string text(hexDigitsPerFingerprint, '0');
    for (int digitIndex = hexDigitsPerFingerprint - 1; 0 <= digitIndex; --digitIndex) {
        text[digitIndex] = hexDigits[value & hexDigitMask];
        value >>= bitsPerHexDigit;
    }
    return text;
}

// Shortest round-trip representation: the same double always prints the same
// characters, which is what makes the output files byte-identical across runs.
void appendNumber(std::string& output, double value) {
    char buffer[numberBufferSize];
    const std::to_chars_result written = std::to_chars(buffer, buffer + numberBufferSize, value);
    output.append(buffer, written.ptr);
}

void appendInteger(std::string& output, std::int64_t value) {
    char buffer[numberBufferSize];
    const std::to_chars_result written = std::to_chars(buffer, buffer + numberBufferSize, value);
    output.append(buffer, written.ptr);
}

void appendCount(std::string& output, size_t value) {
    char buffer[numberBufferSize];
    const std::to_chars_result written = std::to_chars(buffer, buffer + numberBufferSize, value);
    output.append(buffer, written.ptr);
}

std::uint64_t fingerprintText(std::string_view text, std::uint64_t seed) {
    return fingerprintBytes(std::span<const char>(text.data(), text.size()), seed);
}

// "AAPL__1h.csv" -> "AAPL"; a name without "__" is used whole, minus extension.
std::string symbolFromFileName(const std::filesystem::path& filePath) {
    const std::string stem = filePath.stem().string();
    const size_t separatorPosition = stem.find("__");
    if (std::string::npos == separatorPosition) {
        return stem;
    }
    return stem.substr(0, separatorPosition);
}

bool writeWholeFile(const std::filesystem::path& filePath, const std::string& content) {
    std::ofstream output(filePath, std::ios::binary | std::ios::trunc);
    if (!output) {
        std::cerr << "cannot write " << filePath.string() << "\n";
        return false;
    }
    output << content;
    if (!output) {
        std::cerr << "write failed for " << filePath.string() << "\n";
        return false;
    }
    return true;
}

}  // namespace formatting

std::uint64_t fingerprintBytes(std::span<const char> bytes, std::uint64_t seed) {
    std::uint64_t hash = seed;
    for (const char character : bytes) {
        const unsigned char byteValue = character;
        hash ^= byteValue;
        hash *= formatting::fnvPrime;
    }
    return hash;
}

Timeline buildTimeline(std::span<const Bars> histories) {
    Timeline timeline;
    size_t totalBars = 0;
    for (const Bars& history : histories) {
        totalBars += history.size();
    }
    timeline.bars.reserve(totalBars);

    for (const Bars& history : histories) {
        for (size_t rowIndex = 0; rowIndex < history.size(); ++rowIndex) {
            MarketBar bar;
            bar.symbol = history.symbol;
            bar.timestamp = history.timestamp[rowIndex];
            bar.open = history.open[rowIndex];
            bar.high = history.high[rowIndex];
            bar.low = history.low[rowIndex];
            bar.close = history.close[rowIndex];
            bar.volume = history.volume[rowIndex];
            timeline.bars.push_back(std::move(bar));
        }
    }

    std::sort(timeline.bars.begin(), timeline.bars.end(),
              [](const MarketBar& left, const MarketBar& right) {
                  if (left.timestamp != right.timestamp) {
                      return left.timestamp < right.timestamp;
                  }
                  return left.symbol < right.symbol;
              });
    return timeline;
}

ReplayResult runReplay(const Timeline& timeline, Strategy& strategy, Broker& broker) {
    ReplayResult result;
    const std::span<const MarketBar> allBars(timeline.bars);
    size_t stepStart = 0;

    while (stepStart < allBars.size()) {
        const std::int64_t time = allBars[stepStart].timestamp;
        size_t stepEnd = stepStart;
        while (stepEnd < allBars.size() && time == allBars[stepEnd].timestamp) {
            ++stepEnd;
        }
        const std::span<const MarketBar> barsAtTime = allBars.subspan(stepStart, stepEnd - stepStart);

        broker.markPrices(time, barsAtTime);
        const std::vector<Order> orders = strategy.decideOrders(time, barsAtTime, broker.accountSnapshot());
        const std::vector<Fill> fills = broker.submitOrders(time, orders);
        result.fills.insert(result.fills.end(), fills.begin(), fills.end());

        EquityPoint point;
        point.time = time;
        point.account = broker.accountSnapshot();
        result.equityCurve.push_back(point);

        ++result.stepCount;
        stepStart = stepEnd;
    }
    return result;
}

RunManifest describeRun(std::vector<InputFile> inputFiles, const std::string& strategyName,
                        const std::string& brokerName, const Timeline& timeline,
                        const ReplayResult& result) {
    std::sort(inputFiles.begin(), inputFiles.end(),
              [](const InputFile& left, const InputFile& right) {
                  return left.fileName < right.fileName;
              });

    std::uint64_t identity = formatting::fnvOffsetBasis;
    for (const InputFile& inputFile : inputFiles) {
        identity = formatting::fingerprintText(inputFile.fileName, identity);
        identity = formatting::fingerprintText(formatting::fieldSeparator, identity);
        identity = formatting::fingerprintText(formatting::toHex(inputFile.contentFingerprint), identity);
        identity = formatting::fingerprintText(formatting::fieldSeparator, identity);
    }
    identity = formatting::fingerprintText(strategyName, identity);
    identity = formatting::fingerprintText(formatting::fieldSeparator, identity);
    identity = formatting::fingerprintText(brokerName, identity);

    RunManifest manifest;
    manifest.runIdentifier = formatting::toHex(identity);
    manifest.strategyName = strategyName;
    manifest.brokerName = brokerName;
    manifest.inputFiles = std::move(inputFiles);
    manifest.barCount = timeline.bars.size();
    manifest.stepCount = result.stepCount;
    if (!timeline.bars.empty()) {
        manifest.firstTime = timeline.bars.front().timestamp;
        manifest.lastTime = timeline.bars.back().timestamp;
    }
    return manifest;
}

bool loadHistories(std::span<const std::string> filePaths, std::vector<Bars>& histories,
                   std::vector<InputFile>& inputFiles) {
    bool everyFileLoaded = true;
    for (const std::string& filePath : filePaths) {
        std::ifstream input(filePath, std::ios::binary);
        if (!input) {
            std::cerr << "cannot open " << filePath << " - skipped\n";
            everyFileLoaded = false;
            continue;
        }
        const std::string content{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};

        // load_csv reports a malformed file by throwing. Caught here once per
        // FILE, at load time, never inside the replay loop or per value.
        Bars history;
        try {
            history = load_csv(filePath, formatting::symbolFromFileName(filePath));
        } catch (const BarParseError& parseError) {
            std::cerr << parseError.what() << " - skipped\n";
            everyFileLoaded = false;
            continue;
        }

        InputFile inputFile;
        inputFile.fileName = std::filesystem::path(filePath).filename().string();
        inputFile.contentFingerprint =
            fingerprintBytes(std::span<const char>(content.data(), content.size()), formatting::fnvOffsetBasis);
        inputFiles.push_back(std::move(inputFile));
        histories.push_back(std::move(history));
    }
    return everyFileLoaded;
}

bool writeRun(const std::string& outputRoot, const RunManifest& manifest, const ReplayResult& result) {
    const std::filesystem::path runDirectory = std::filesystem::path(outputRoot) / manifest.runIdentifier;
    std::error_code directoryError;
    std::filesystem::create_directories(runDirectory, directoryError);
    if (directoryError) {
        std::cerr << "cannot create " << runDirectory.string() << ": " << directoryError.message() << "\n";
        return false;
    }

    std::string manifestText;
    manifestText += "run_identifier=" + manifest.runIdentifier + "\n";
    manifestText += "strategy=" + manifest.strategyName + "\n";
    manifestText += "broker=" + manifest.brokerName + "\n";
    manifestText += "bar_count=";
    formatting::appendCount(manifestText, manifest.barCount);
    manifestText += "\nstep_count=";
    formatting::appendCount(manifestText, manifest.stepCount);
    manifestText += "\nfirst_time=";
    formatting::appendInteger(manifestText, manifest.firstTime);
    manifestText += "\nlast_time=";
    formatting::appendInteger(manifestText, manifest.lastTime);
    manifestText += "\n";
    for (const InputFile& inputFile : manifest.inputFiles) {
        manifestText += "input=" + inputFile.fileName + " " + formatting::toHex(inputFile.contentFingerprint) + "\n";
    }

    std::string fillsText = "timestamp,symbol,quantity,price,fee\n";
    for (const Fill& fill : result.fills) {
        formatting::appendInteger(fillsText, fill.timestamp);
        fillsText += "," + fill.symbol + ",";
        formatting::appendNumber(fillsText, fill.quantity);
        fillsText += ",";
        formatting::appendNumber(fillsText, fill.price);
        fillsText += ",";
        formatting::appendNumber(fillsText, fill.fee);
        fillsText += "\n";
    }

    std::string equityText = "timestamp,cash,equity\n";
    for (const EquityPoint& point : result.equityCurve) {
        formatting::appendInteger(equityText, point.time);
        equityText += ",";
        formatting::appendNumber(equityText, point.account.cash);
        equityText += ",";
        formatting::appendNumber(equityText, point.account.equity);
        equityText += "\n";
    }

    if (!formatting::writeWholeFile(runDirectory / "manifest.txt", manifestText)) {
        return false;
    }
    if (!formatting::writeWholeFile(runDirectory / "fills.csv", fillsText)) {
        return false;
    }
    return formatting::writeWholeFile(runDirectory / "equity.csv", equityText);
}

}  // namespace tb::driver
