#!/usr/bin/env python3
"""Refresh data/ from public sources. Run on a machine with internet access.

Sources
  Yahoo Finance chart API  daily closes, cash dividends and split-style events for T and VZ
  FRED                     DGS10, 10-year Treasury constant maturity yield

Writes the same files scripts/build_dashboard.py reads:
  data/weekly_market_data.csv   Friday observations (last available value on or before each Friday)
                                plus the latest trading day if it is not a Friday
  data/dividends_T.csv, data/dividends_VZ.csv
  data/corporate_actions.csv    every split-style event Yahoo reports, so the build can undo it

Yahoo's "close" field is split-adjusted, and Yahoo books AT&T's 2022 WarnerMedia spin-off
as a 1324:1000 split. The build script multiplies pre-event closes by numerator/denominator
to recover the unadjusted price. That is correct for a spin-off booked this way. It would
also undo a genuine share split, which is what you want when pairing prices with the
unadjusted cash dividends Yahoo reports for these two names. Neither company has split
in the window, so the only event in practice is the AT&T spin-off.

Standard library only. Usage:  python3 scripts/fetch_data.py [--start 2015-01-01]
"""
from __future__ import annotations

import argparse
import csv
import json
import sys
import time
import urllib.request
from datetime import date, datetime, timedelta, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DATA = ROOT / "data"
UA = {"User-Agent": "Mozilla/5.0 (price-tracker; +https://github.com/sammchugh-gif/price-tracker)"}


def get(url: str) -> bytes:
    req = urllib.request.Request(url, headers=UA)
    with urllib.request.urlopen(req, timeout=60) as resp:
        return resp.read()


def yahoo(ticker: str, start: date, end: date) -> dict:
    p1 = int(datetime(start.year, start.month, start.day, tzinfo=timezone.utc).timestamp())
    p2 = int(datetime(end.year, end.month, end.day, tzinfo=timezone.utc).timestamp()) + 86400
    url = (f"https://query2.finance.yahoo.com/v8/finance/chart/{ticker}"
           f"?period1={p1}&period2={p2}&interval=1d&events=div%2Csplits")
    payload = json.loads(get(url))
    res = payload["chart"]["result"][0]
    closes = {}
    for ts, c in zip(res["timestamp"], res["indicators"]["quote"][0]["close"]):
        if c is not None:
            closes[datetime.fromtimestamp(ts, tz=timezone.utc).date()] = round(c, 4)
    events = res.get("events", {})
    dividends = sorted((datetime.fromtimestamp(v["date"], tz=timezone.utc).date(), v["amount"])
                       for v in events.get("dividends", {}).values())
    splits = sorted((datetime.fromtimestamp(v["date"], tz=timezone.utc).date(), v["numerator"], v["denominator"])
                    for v in events.get("splits", {}).values())
    return {"closes": closes, "dividends": dividends, "splits": splits}


def fred_dgs10() -> dict[date, float]:
    text = get("https://fred.stlouisfed.org/graph/fredgraph.csv?id=DGS10").decode()
    out = {}
    for row in csv.DictReader(text.splitlines()):
        v = row.get("DGS10", "").strip()
        if v and v != ".":
            out[date.fromisoformat(row["observation_date"])] = float(v)
    return out


def last_on_or_before(series: dict[date, float], d: date, lookback: int = 6) -> float | None:
    for k in range(lookback):
        v = series.get(d - timedelta(days=k))
        if v is not None:
            return v
    return None


def fridays(start: date, end: date) -> list[date]:
    d = start
    while d.weekday() != 4:  # Friday
        d += timedelta(days=1)
    out = []
    while d <= end:
        out.append(d)
        d += timedelta(days=7)
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--start", default="2015-01-01")
    args = ap.parse_args()
    start = date.fromisoformat(args.start)
    today = date.today()

    print("fetching Yahoo T ...", file=sys.stderr)
    t = yahoo("T", start, today)
    time.sleep(1)
    print("fetching Yahoo VZ ...", file=sys.stderr)
    vz = yahoo("VZ", start, today)
    print("fetching FRED DGS10 ...", file=sys.stderr)
    dgs10 = fred_dgs10()

    last_trading = max(max(t["closes"]), max(vz["closes"]))
    grid = fridays(start, last_trading)
    if last_trading not in grid:
        grid.append(last_trading)

    DATA.mkdir(exist_ok=True)
    with (DATA / "weekly_market_data.csv").open("w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["date", "T_close_split_adjusted", "VZ_close", "DGS10"])
        for d in grid:
            tv = last_on_or_before(t["closes"], d)
            vv = last_on_or_before(vz["closes"], d)
            yv = last_on_or_before(dgs10, d)
            w.writerow([d.isoformat(), "" if tv is None else tv, "" if vv is None else vv, "" if yv is None else yv])

    for name, src in (("T", t), ("VZ", vz)):
        with (DATA / f"dividends_{name}.csv").open("w", newline="") as f:
            w = csv.writer(f)
            w.writerow(["ex_date", "dividend_per_share"])
            for d, amt in src["dividends"]:
                w.writerow([d.isoformat(), amt])

    with (DATA / "corporate_actions.csv").open("w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["ticker", "effective_date", "type", "numerator", "denominator", "note"])
        for name, src in (("T", t), ("VZ", vz)):
            for d, num, den in src["splits"]:
                w.writerow([name, d.isoformat(), "split_adjustment", num, den,
                            "Reported by Yahoo Finance as a split; the build multiplies earlier closes by numerator/denominator."])

    print(f"wrote {len(grid)} weekly rows through {last_trading}, "
          f"{len(t['dividends'])} T dividends, {len(vz['dividends'])} VZ dividends", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
