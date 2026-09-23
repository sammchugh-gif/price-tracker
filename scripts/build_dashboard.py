#!/usr/bin/env python3
"""Build the dividend-yield dashboard from the CSVs in data/.

Reads
  data/weekly_market_data.csv   date, T close (Yahoo split-adjusted), VZ close, DGS10
  data/dividends_T.csv          ex-date, cash dividend per share
  data/dividends_VZ.csv
  data/corporate_actions.csv    split-style adjustments to undo (AT&T's 2022 spin-off)

Writes
  docs/index.html               self-contained page (GitHub Pages / open locally)
  build/artifact.html           same page as a body fragment for publishing as an Artifact

Yield definitions
  indicated  = latest declared quarterly dividend x 4 / price
  trailing   = sum of dividends with ex-dates in the trailing 365 days / price
Spreads are yield minus the 10-year constant-maturity Treasury yield (FRED DGS10),
expressed in basis points.

Standard library only.
"""
from __future__ import annotations

import csv
import json
import os
from datetime import date, timedelta
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DATA = ROOT / "data"
DOCS = ROOT / "docs"
BUILD = ROOT / "build"
TEMPLATE = ROOT / "scripts" / "template.html"

START = date(2016, 1, 1)  # a full year of dividend history exists from here


def read_csv(path: Path) -> list[dict]:
    with path.open(newline="") as f:
        return list(csv.DictReader(f))


def parse_date(s: str) -> date:
    return date.fromisoformat(s)


def to_float(s: str) -> float | None:
    s = (s or "").strip()
    return float(s) if s else None


def load_dividends(path: Path) -> list[tuple[date, float]]:
    rows = [(parse_date(r["ex_date"]), float(r["dividend_per_share"])) for r in read_csv(path)]
    rows.sort()
    return rows


def load_adjustments(path: Path) -> dict[str, list[tuple[date, float]]]:
    """ticker -> [(effective_date, factor)] where factor multiplies closes before effective_date."""
    out: dict[str, list[tuple[date, float]]] = {}
    if not path.exists():
        return out
    for r in read_csv(path):
        factor = float(r["numerator"]) / float(r["denominator"])
        out.setdefault(r["ticker"], []).append((parse_date(r["effective_date"]), factor))
    return out


def unadjust(price: float | None, d: date, actions: list[tuple[date, float]]) -> float | None:
    if price is None:
        return None
    for eff, factor in actions:
        if d < eff:
            price *= factor
    return price


def indicated_yield(d: date, price: float | None, divs: list[tuple[date, float]]) -> float | None:
    if price is None or price <= 0:
        return None
    last = None
    for ex, amt in divs:
        if ex <= d:
            last = amt
        else:
            break
    if last is None:
        return None
    return 100.0 * last * 4 / price


def trailing_yield(d: date, price: float | None, divs: list[tuple[date, float]]) -> float | None:
    if price is None or price <= 0:
        return None
    lo = d - timedelta(days=365)
    total = sum(amt for ex, amt in divs if lo < ex <= d)
    if total == 0:
        return None
    return 100.0 * total / price


def r2(x: float | None, nd: int = 2) -> float | None:
    return None if x is None else round(x, nd)


def build_rows() -> tuple[list[dict], dict]:
    market = read_csv(DATA / "weekly_market_data.csv")
    div_t = load_dividends(DATA / "dividends_T.csv")
    div_vz = load_dividends(DATA / "dividends_VZ.csv")
    actions = load_adjustments(DATA / "corporate_actions.csv")

    rows = []
    for r in market:
        d = parse_date(r["date"])
        if d < START:
            continue
        t_price = unadjust(to_float(r["T_close_split_adjusted"]), d, actions.get("T", []))
        vz_price = unadjust(to_float(r["VZ_close"]), d, actions.get("VZ", []))
        y10 = to_float(r["DGS10"])
        rows.append({
            "d": d.isoformat(),
            "tP": r2(t_price),
            "vP": r2(vz_price),
            "y10": y10,
            "tInd": r2(indicated_yield(d, t_price, div_t)),
            "tTtm": r2(trailing_yield(d, t_price, div_t)),
            "vInd": r2(indicated_yield(d, vz_price, div_vz)),
            "vTtm": r2(trailing_yield(d, vz_price, div_vz)),
        })

    meta = {
        "asOf": rows[-1]["d"],
        "firstDate": rows[0]["d"],
        "lastDividend": {
            "T": {"exDate": div_t[-1][0].isoformat(), "amount": div_t[-1][1]},
            "VZ": {"exDate": div_vz[-1][0].isoformat(), "amount": div_vz[-1][1]},
        },
        "sources": {
            "prices": "Yahoo Finance daily closes via Wolfram FinancialData, weekly Friday observations",
            "dividends": "Yahoo Finance cash dividend history via Wolfram FinancialData",
            "treasury": "FRED series DGS10, 10-year Treasury constant maturity, daily, percent",
        },
    }
    return rows, meta


def main() -> None:
    rows, meta = build_rows()
    payload = json.dumps({"meta": meta, "rows": rows}, separators=(",", ":"))
    fragment = TEMPLATE.read_text().replace("__DATA_JSON__", payload)

    BUILD.mkdir(exist_ok=True)
    (BUILD / "artifact.html").write_text(fragment)

    full = (
        "<!doctype html>\n<html lang=\"en\">\n<head>\n"
        "<meta charset=\"utf-8\">\n"
        "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1, viewport-fit=cover\">\n"
        "</head>\n<body>\n" + fragment + "\n</body>\n</html>\n"
    )
    DOCS.mkdir(exist_ok=True)
    (DOCS / "index.html").write_text(full)

    last = rows[-1]
    print(f"rows: {len(rows)}  first: {rows[0]['d']}  as of: {last['d']}")
    print(f"T   indicated {last['tInd']}%  trailing {last['tTtm']}%  price {last['tP']}")
    print(f"VZ  indicated {last['vInd']}%  trailing {last['vTtm']}%  price {last['vP']}")
    print(f"10Y {last['y10']}%")
    print(f"wrote {DOCS / 'index.html'} ({len(full)/1024:.0f} KB) and {BUILD / 'artifact.html'}")


if __name__ == "__main__":
    os.chdir(ROOT)
    main()
