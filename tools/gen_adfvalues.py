"""Generates cpp/src/adfvalues.hpp from statsmodels' own MacKinnon tables.

These are the response-surface coefficients behind every ADF / cointegration
p-value. They are NOT transcribed by hand: a single mistyped digit produces
plausible-but-wrong p-values with no visible failure, which would silently
call unrelated series cointegrated.

The output is committed, so a checkout builds and self-tests without Python or
statsmodels installed. Re-run only if statsmodels changes:

    python tools/gen_adfvalues.py
"""
import math
import os

import numpy as np
import statsmodels
import statsmodels.tsa.adfvalues as a

TRENDS = ("nc", "c", "ct", "ctt")


def fmt(x):
    if math.isinf(x):
        return ("" if x > 0 else "-") + "std::numeric_limits<double>::infinity()"
    return repr(float(x))


def arr1(name):
    return "{" + ", ".join(fmt(x) for x in np.asarray(getattr(a, name), dtype=float)) + "}"


def arr2(name):
    m = np.asarray(getattr(a, name), dtype=float)
    rows = ["    {" + ", ".join(fmt(x) for x in r) + "}" for r in m]
    return "{\n" + ",\n".join(rows) + "\n}", m.shape


lines = [
    f"// GENERATED from statsmodels {statsmodels.__version__} "
    "(statsmodels/tsa/adfvalues.py) by",
    "// cpp/tools/gen_adfvalues.py. Do not hand-edit: these are MacKinnon's",
    "// response-surface coefficients, and a single mistyped digit produces",
    "// plausible-but-wrong p-values rather than any visible failure.",
    "#pragma once",
    "#include <limits>",
    "",
    "namespace tb::adfvalues {",
    "",
]

for t in TRENDS:
    lines.append(f"inline constexpr double tau_star_{t}[] = {arr1('tau_star_' + t)};")
    lines.append(f"inline constexpr double tau_min_{t}[] = {arr1('tau_min_' + t)};")
    lines.append(f"inline constexpr double tau_max_{t}[] = {arr1('tau_max_' + t)};")

for t in TRENDS:
    for kind in ("smallp", "largep"):
        body, shape = arr2(f"tau_{t}_{kind}")
        lines.append(f"inline constexpr double tau_{t}_{kind}[{shape[0]}][{shape[1]}] = {body};")

lines += ["", "}  // namespace tb::adfvalues"]

path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "src", "adfvalues.hpp")
with open(path, "w", encoding="utf-8") as f:
    f.write("\n".join(lines) + "\n")
print("wrote", os.path.normpath(path))
