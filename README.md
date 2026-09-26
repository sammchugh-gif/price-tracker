# price-tracker

Dashboards built from public market data. First one: **AT&T and Verizon dividend and total shareholder yields versus the US 10-year Treasury yield**, weekly, from 2016.

## Dividend yield dashboard

`docs/index.html` is a single self-contained page. Open it locally or serve it with GitHub Pages (Settings → Pages → branch, folder `/docs`).

What it shows

- Yield for T and VZ on the price an investor paid at the time, against the 10-year constant-maturity Treasury yield, on one percent axis.
- The spread of each stock's yield over the 10-year, in basis points, with a zero line.
- Total shareholder yield decomposed into cash dividends and buybacks, one panel per company, with the 10-year overlaid.
- Latest readings, where today's spread sits in the distribution since 2016, and the year-on-year change in the share count.
- Range presets (1Y, 3Y, 5Y, Max), a choice of yield basis that scopes every chart and tile, a crosshair tooltip, and a table view of every weekly row.

Yield bases

- **Indicated**: latest declared quarterly dividend × 4 ÷ price. Per share. Reacts immediately to a change in the payout, so AT&T's April 2022 cut shows on the day.
- **Trailing dividend**: trailing-twelve-month cash dividends paid to common shareholders (cash flow statement) ÷ market capitalisation. Lags a cut by up to a year.
- **Total shareholder**: trailing dividend yield plus buyback yield, where buyback yield is trailing-twelve-month cash paid to repurchase common stock ÷ market capitalisation.

Market capitalisation is the weekly price × the most recent cover-page share count from the SEC filing on or before that date. TTM figures sum the four most recent fiscal quarters whose period end falls on or before the date, so a quarter enters the series at its period end rather than its filing date. The buyback measure is gross cash repurchases. Neither company reports a common-stock issuance line, and dilution from employee equity plans is visible instead in the year-on-year share-count change shown on the tiles and in the table.

## Data

| File | Contents | Source |
|---|---|---|
| `data/weekly_market_data.csv` | Friday closes for T and VZ, and the 10-year yield | Yahoo Finance daily closes (via Wolfram `FinancialData`); FRED `DGS10` |
| `data/dividends_T.csv`, `data/dividends_VZ.csv` | Cash dividends by ex-date | Yahoo Finance dividend events |
| `data/corporate_actions.csv` | Split-style adjustments the build undoes | Yahoo Finance split events |
| `data/sec_facts_T.csv`, `data/sec_facts_VZ.csv` | Cash dividends paid, share repurchases, cover-page shares outstanding, by reporting period | SEC XBRL company facts API (CIK 732717, CIK 732712) |

Two things worth knowing about the price series.

1. Yahoo's close is split-adjusted, and it books AT&T's April 2022 WarnerMedia spin-off as a 1324:1000 split. Every AT&T close before 11 April 2022 in the feed is therefore divided by 1.324. The build multiplies those closes back by 1.324 so that pre-spin yields use the price actually paid. Leaving the adjustment in place would overstate AT&T's historical yield by roughly a third.
2. The Yahoo dividend feed is missing the January 2026 ex-date for both companies. Only the indicated yield uses that feed, and it is unaffected because it reads the latest payment. The trailing and total bases use cash dividends from the SEC filings instead.
3. Verizon reports its repurchase line only in years with activity (2015, and 2026 onward) plus explicit zeros for 2016, 2017 and 2025. The build treats calendar years, and later quarters within a year, with no repurchase row as zero for that line only. AT&T reports it every quarter.

## Rebuilding

```
python3 scripts/fetch_data.py      # refresh data/ from Yahoo Finance and FRED (needs internet)
python3 scripts/build_dashboard.py # regenerate docs/index.html from data/
```

Both scripts use only the Python standard library.
