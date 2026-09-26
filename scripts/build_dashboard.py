#!/usr/bin/env python3
"""Build the dividend-yield dashboard from the CSVs in data/.

Reads
  data/weekly_market_data.csv   date, T close (Yahoo split-adjusted), VZ close, DGS10
  data/dividends_T.csv          ex-date, cash dividend per share
  data/dividends_VZ.csv
  data/corporate_actions.csv    split-style adjustments to undo (AT&T's 2022 spin-off)
  data/sec_facts_T.csv          SEC XBRL company facts: cash dividends paid, share repurchases,
  data/sec_facts_VZ.csv         cover-page shares outstanding (concept,start,end,value,form,fy,fp,filed)

Writes
  docs/index.html               self-contained page (GitHub Pages / open locally)
  build/artifact.html           same page as a body fragment for publishing as an Artifact

Yield definitions
  indicated          = latest declared quarterly dividend x 4 / price            (per share)
  trailing dividend  = TTM cash dividends paid to common / market cap            (cash flow statement)
  buyback            = TTM cash paid to repurchase common stock / market cap     (cash flow statement)
  total shareholder  = trailing dividend + buyback
  market cap         = price x latest cover-page shares outstanding on or before the date
TTM sums the four most recent fiscal quarters whose period end is on or before the date.
Quarterly cash flows are derived from the year-to-date figures in 10-Qs and the full-year
figure in the 10-K. Spreads are yield minus the 10-year constant-maturity Treasury yield
(FRED DGS10), expressed in basis points.

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


QUARTER_ENDS = {(3, 31): 1, (6, 30): 2, (9, 30): 3, (12, 31): 4}


def load_sec_facts(path: Path) -> dict[str, list[dict]]:
    out: dict[str, list[dict]] = {}
    for r in read_csv(path):
        out.setdefault(r["concept"], []).append(r)
    return out


def quarterly_from_ytd(rows: list[dict], zero_fill_years: bool = False) -> dict[date, float]:
    """Turn year-to-date cash-flow facts into discrete fiscal quarters keyed by quarter end.

    A row counts only when it starts on 1 January and ends on a quarter end. Q1 is the
    Q1 YTD figure; later quarters are the difference between consecutive YTD figures.
    With zero_fill_years, a calendar year that has no rows at all, or only a full-year
    row of zero, is treated as four quarters of zero, and a quarter missing from a year
    that has earlier quarters is treated as zero activity (the YTD figure carries forward).
    That is right for a repurchase line that filers omit when there was nothing to report,
    and wrong for anything else.
    """
    ytd: dict[tuple[int, int], float] = {}
    for r in rows:
        if not r["start"]:
            continue
        start, end = parse_date(r["start"]), parse_date(r["end"])
        q = QUARTER_ENDS.get((end.month, end.day))
        if start != date(end.year, 1, 1) or q is None:
            continue
        ytd[(end.year, q)] = float(r["value"])
    years = sorted({y for y, _ in ytd})
    out: dict[date, float] = {}
    if not years:
        return out
    for y in range(years[0], years[-1] + 1):
        have = {q: ytd[(y, q)] for q in (1, 2, 3, 4) if (y, q) in ytd}
        if zero_fill_years and (not have or (set(have) == {4} and have[4] == 0)):
            have = {1: 0.0, 2: 0.0, 3: 0.0, 4: 0.0}
        if zero_fill_years and have and y < years[-1]:
            carry = 0.0
            for q in (1, 2, 3, 4):
                carry = have.setdefault(q, carry)
        prev = 0.0
        for q in (1, 2, 3, 4):
            if q not in have:
                prev = None
                continue
            qend = date(y, *[(3, 31), (6, 30), (9, 30), (12, 31)][q - 1])
            if prev is not None:
                out[qend] = have[q] - prev
            prev = have[q]
    return out


def ttm(quarters: dict[date, float], d: date) -> float | None:
    ends = sorted(k for k in quarters if k <= d)
    if len(ends) < 4:
        return None
    last4 = ends[-4:]
    if (d - last4[-1]).days > 200:  # data has gone stale, do not carry it forward
        return None
    return sum(quarters[k] for k in last4)


def shares_series(rows: list[dict]) -> list[tuple[date, float]]:
    return sorted((parse_date(r["end"]), float(r["value"])) for r in rows)


def shares_at(series: list[tuple[date, float]], d: date) -> float | None:
    val = None
    for dt, v in series:
        if dt <= d:
            val = v
        else:
            break
    return val if val is not None else (series[0][1] if series else None)


def r2(x: float | None, nd: int = 2) -> float | None:
    return None if x is None else round(x, nd)


def build_rows() -> tuple[list[dict], dict]:
    market = read_csv(DATA / "weekly_market_data.csv")
    div_t = load_dividends(DATA / "dividends_T.csv")
    div_vz = load_dividends(DATA / "dividends_VZ.csv")
    actions = load_adjustments(DATA / "corporate_actions.csv")

    sec = {}
    for tk in ("T", "VZ"):
        f = load_sec_facts(DATA / f"sec_facts_{tk}.csv")
        div_rows = f.get("PaymentsOfDividendsCommonStock") or f.get("PaymentsOfDividends") or []
        sec[tk] = {
            "div": quarterly_from_ytd(div_rows),
            "buy": quarterly_from_ytd(f.get("PaymentsForRepurchaseOfCommonStock", []), zero_fill_years=True),
            "shares": shares_series(f.get("EntityCommonStockSharesOutstanding", [])),
        }

    def cash_yields(tk: str, d: date, price: float | None) -> dict:
        sh = shares_at(sec[tk]["shares"], d)
        if price is None or sh is None:
            return {"mc": None, "divc": None, "bb": None, "tsy": None, "shr": None, "divttm": None, "bbttm": None}
        mc = price * sh
        dv, bb = ttm(sec[tk]["div"], d), ttm(sec[tk]["buy"], d)
        divc = None if dv is None else 100.0 * dv / mc
        bby = None if bb is None else 100.0 * bb / mc
        prev = shares_at(sec[tk]["shares"], d - timedelta(days=365))
        shr = None if not prev else 100.0 * (sh / prev - 1)
        return {"mc": mc / 1e9, "divc": divc, "bb": bby, "tsy": None if divc is None or bby is None else divc + bby,
                "shr": shr, "divttm": None if dv is None else dv / 1e9, "bbttm": None if bb is None else bb / 1e9}

    rows = []
    for r in market:
        d = parse_date(r["date"])
        if d < START:
            continue
        t_price = unadjust(to_float(r["T_close_split_adjusted"]), d, actions.get("T", []))
        vz_price = unadjust(to_float(r["VZ_close"]), d, actions.get("VZ", []))
        y10 = to_float(r["DGS10"])
        tc, vc = cash_yields("T", d, t_price), cash_yields("VZ", d, vz_price)
        rows.append({
            "d": d.isoformat(),
            "tP": r2(t_price),
            "vP": r2(vz_price),
            "y10": y10,
            "tInd": r2(indicated_yield(d, t_price, div_t)),
            "vInd": r2(indicated_yield(d, vz_price, div_vz)),
            "tTtm": r2(tc["divc"]), "tBb": r2(tc["bb"]), "tTsy": r2(tc["tsy"]), "tShr": r2(tc["shr"]), "tMc": r2(tc["mc"], 1),
            "vTtm": r2(vc["divc"]), "vBb": r2(vc["bb"]), "vTsy": r2(vc["tsy"]), "vShr": r2(vc["shr"]), "vMc": r2(vc["mc"], 1),
        })

    last_d = parse_date(rows[-1]["d"])
    latest = {}
    for tk in ("T", "VZ"):
        q_end = max(k for k in sec[tk]["buy"] if k <= last_d)
        c = cash_yields(tk, last_d, unadjust(to_float(market[-1]["T_close_split_adjusted" if tk == "T" else "VZ_close"]), last_d, actions.get(tk, [])))
        latest[tk] = {"quarterEnd": q_end.isoformat(), "ttmDividendsB": r2(c["divttm"], 2), "ttmBuybacksB": r2(c["bbttm"], 2),
                      "sharesB": r2(shares_at(sec[tk]["shares"], last_d) / 1e9, 3), "marketCapB": r2(c["mc"], 1)}

    meta = {
        "asOf": rows[-1]["d"],
        "latestCash": latest,
        "firstDate": rows[0]["d"],
        "lastDividend": {
            "T": {"exDate": div_t[-1][0].isoformat(), "amount": div_t[-1][1]},
            "VZ": {"exDate": div_vz[-1][0].isoformat(), "amount": div_vz[-1][1]},
        },
        "sources": {
            "prices": "Yahoo Finance daily closes via Wolfram FinancialData, weekly Friday observations",
            "dividends": "Yahoo Finance cash dividend history via Wolfram FinancialData",
            "treasury": "FRED series DGS10, 10-year Treasury constant maturity, daily, percent",
            "cashflows": "SEC XBRL company facts API: PaymentsOfDividendsCommonStock (AT&T) / PaymentsOfDividends (Verizon), PaymentsForRepurchaseOfCommonStock, dei:EntityCommonStockSharesOutstanding",
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
    print(f"T   indicated {last['tInd']}%  cash div {last['tTtm']}%  buyback {last['tBb']}%  total {last['tTsy']}%  shares yoy {last['tShr']}%  mcap ${last['tMc']}B")
    print(f"VZ  indicated {last['vInd']}%  cash div {last['vTtm']}%  buyback {last['vBb']}%  total {last['vTsy']}%  shares yoy {last['vShr']}%  mcap ${last['vMc']}B")
    print("latest cash:", meta["latestCash"])
    print(f"10Y {last['y10']}%")
    print(f"wrote {DOCS / 'index.html'} ({len(full)/1024:.0f} KB) and {BUILD / 'artifact.html'}")


if __name__ == "__main__":
    os.chdir(ROOT)
    main()
