# The backtest driver

`src/driver.hpp`, `src/driver.cpp`, tested by `tests/test_driver.cpp`.

The driver replays historical bars through a strategy and a simulated broker,
and records what they did. It is a clock, a data feed and a recorder. It makes
no trading decision and does no accounting of its own.

## Why the driver holds no trading rules

A backtest should run the strategy itself through history, not a second copy
of its rules. A backtester that re-implements a strategy holds two versions of
every rule, and two versions drift: the replay then reports results for a
strategy nobody runs. So the work is split three ways, and only one of the
three lives in the driver:

| piece | job | where |
|---|---|---|
| **driver** | the simulated clock, the historical data feed, the recorder | `src/driver.hpp` |
| **broker** | fills, fees, cash, positions: a simulated account | implements `tb::driver::Broker` |
| **strategy** | every trading decision | implements `tb::driver::Strategy` |

## A run, end to end

```cpp
std::vector<tb::Bars> histories;
std::vector<tb::driver::InputFile> inputFiles;
const bool everyFileLoaded = tb::driver::loadHistories(filePaths, histories, inputFiles);

const tb::driver::Timeline timeline = tb::driver::buildTimeline(histories);
const tb::driver::ReplayResult result = tb::driver::runReplay(timeline, strategy, broker);
const tb::driver::RunManifest manifest = tb::driver::describeRun(
    inputFiles, strategy.strategyName(), broker.brokerName(), timeline, result);
tb::driver::writeRun(outputRoot, manifest, result);
```

### `loadHistories`

Reads each file's bytes, fingerprints them, and parses them as bars. The
symbol is taken from the file name: `AAPL__1h.csv` gives `AAPL`, and a name
with no `__` is used whole, without its extension.

A file that cannot be opened or parsed is reported on stderr and skipped. The
function then returns `false`, so the caller knows the run is incomplete. A
skipped file is not part of the run's identity: the run describes exactly the
data it replayed.

`load_csv` reports a malformed file by throwing `BarParseError`. That is the
only exception in the driver, and it is caught once per file at load time, never
inside the replay loop.

### `buildTimeline`

Puts every bar of every symbol on one clock, sorted by timestamp and then by
symbol. Ties are broken by symbol, never by the order the files were loaded, so
the order of the input files cannot change a result.

### `runReplay`: the loop

A *step* is one distinct timestamp. For each step, in order:

1. `broker.markPrices(time, barsAtTime)`: the broker sees the bars that closed
   at this time;
2. `orders = strategy.decideOrders(time, barsAtTime, broker.accountSnapshot())`;
3. `fills = broker.submitOrders(time, orders)`;
4. the driver records the fills and `broker.accountSnapshot()` as one point of
   the equity curve.

The strategy is handed only the bars of the current step, so it can never
see a bar from the future. A strategy that needs history keeps it itself, from
the bars it has already been shown.

The broker is marked before the strategy decides, so the snapshot the strategy
receives is valued at this step's prices, not the previous step's.

### `describeRun` and `writeRun`

`describeRun` builds the run's identity and manifest (see below). `writeRun`
writes three files into `<outputRoot>/<runIdentifier>/`, replacing any earlier
copy:

| file | contents |
|---|---|
| `manifest.txt` | run identifier, strategy and broker names, bar and step counts, first and last time, then one `input=<file name> <fingerprint>` line per file |
| `fills.csv` | `timestamp,symbol,quantity,price,fee`, one row per fill, in the order they happened |
| `equity.csv` | `timestamp,cash,equity`, one row per step |

It returns `false` and reports on stderr if the directory or a file cannot be
written.

## The two interfaces

### `Broker`

| method | called | does |
|---|---|---|
| `brokerName()` | when the run is described | a stable name, part of the run's identity |
| `markPrices(time, bars)` | first, at every step | updates prices with the bars that closed at `time` |
| `submitOrders(time, orders)` | after the strategy decides | executes what it can, returns the fills |
| `accountSnapshot()` | before the decision and after the fills | cash and equity |

### `Strategy`

| method | called | does |
|---|---|---|
| `strategyName()` | when the run is described | a stable name, part of the run's identity |
| `decideOrders(time, bars, account)` | once per step | returns the orders for this step |

An `Order` is a symbol and a quantity: a positive quantity buys, a negative one
sells. What a broker does with an order it cannot fill (no price yet, not
enough cash) is the broker's decision, not the driver's.

## Runs are idempotent

The same inputs always give the same run identity and byte-identical output.
Three things make that hold:

- **No wall clock and no randomness in the loop.** Time is the timestamp of
  the bars being replayed. Bars are ordered by timestamp and then symbol, never
  by load order.
- **A run is identified by its inputs.** The identity combines every input
  file's name and content fingerprint (sorted by file name, so the order the
  files were passed in does not matter), then the strategy's name, then the
  broker's name. A separator byte (`0x1F`) is hashed between fields, so
  `("ab", "c")` and `("a", "bc")` cannot give the same identity. Output goes to a
  directory named after that identity, so a different input can never overwrite
  another input's result.
- **Numbers are written in shortest round-trip form** (`std::to_chars`), so the
  same double always prints the same characters.

**A strategy or broker whose behaviour changes must change its name.** The
name is the only part of the identity that describes the code, so a changed
strategy under an unchanged name would overwrite the old result with a different
one in the same directory.

### The fingerprint

The fingerprint is 64-bit FNV-1a
([reference](http://www.isthe.com/chongo/tech/comp/fnv/)): offset basis
`14695981039346656037`, prime `1099511628211`. The run identifier is the final
value as 16 lowercase hex digits.

It is not cryptographic and does not need to be: it only has to make two
different inputs land in different run directories. The test suite pins two
published reference values (`""` gives `cbf29ce484222325` and `"a"` gives
`af63dc4c8601ec8c`), so a change to the hash cannot pass unnoticed.

## Types

- **Timestamps are `std::int64_t`, Unix seconds, UTC.** The type is signed on
  purpose: `parse_timestamp` accepts any year from 0000 to 9999, and daily
  histories of long-listed stocks start before 1970, which is negative Unix
  time. It is also the type `Bars::timestamp` already uses, so the driver copies
  timestamps without a conversion.
- **Quantities, prices, fees, cash and equity are `double`.** Fractional
  quantities are allowed; rounding to whole shares, if a broker needs it, is the
  broker's job.

## What the tests prove

`tests/test_driver.cpp` uses a recording broker and strategy that log every
call:

| test | proves |
|---|---|
| timeline ordering | bars are sorted by timestamp, ties broken by symbol |
| no look-ahead | every bar shown at a step belongs to that step |
| call order | mark, decide, submit, snapshot, at every step |
| fills recorded | fills and one equity point per step reach the result |
| FNV reference values | the hash matches the published values |
| run identity | the order of input files does not matter; their content and the two names do |
| rerun | the same inputs write byte-identical files to the same directory |
| a bad file is skipped | it is left out of the run, and `loadHistories` returns `false` |

The call-order and rerun tests were checked by breaking the driver on purpose
(swapping mark and decide; writing a counter into the manifest) and watching
them fail.
