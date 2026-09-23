# price-tracker

Dashboards built from public market data. First one: **AT&T and Verizon dividend yields versus the US 10-year Treasury yield**, weekly, from 2016.

## Dividend yield dashboard

`docs/index.html` is a single self-contained page. Open it locally or serve it with GitHub Pages (Settings → Pages → branch, folder `/docs`).

What it shows

- Dividend yield for T and VZ on the price an investor paid at the time, against the 10-year constant-maturity Treasury yield, on one percent axis.
- The spread of each stock's yield over the 10-year, in basis points, with a zero line.
- Latest readings, and where today's spread sits in the distribution since 2016.
- Range presets (1Y, 3Y, 5Y, Max), a choice of yield basis, a crosshair tooltip, and a table view of every weekly row.

Yield bases

- **Indicated**: latest declared quarterly dividend × 4 ÷ price. Reacts immediately to a change in the payout, so AT&T's April 2022 cut shows on the day.
- **Trailing 12M**: cash dividends with ex-dates in the prior 365 days ÷ price. Smoother, and lags a cut by up to a year.

## Data

| File | Contents | Source |
|---|---|---|
| `data/weekly_market_data.csv` | Friday closes for T and VZ, and the 10-year yield | Yahoo Finance daily closes (via Wolfram `FinancialData`); FRED `DGS10` |
| `data/dividends_T.csv`, `data/dividends_VZ.csv` | Cash dividends by ex-date | Yahoo Finance dividend events |
| `data/corporate_actions.csv` | Split-style adjustments the build undoes | Yahoo Finance split events |

Two things worth knowing about the price series.

1. Yahoo's close is split-adjusted, and it books AT&T's April 2022 WarnerMedia spin-off as a 1324:1000 split. Every AT&T close before 11 April 2022 in the feed is therefore divided by 1.324. The build multiplies those closes back by 1.324 so that pre-spin yields use the price actually paid. Leaving the adjustment in place would overstate AT&T's historical yield by roughly a third.
2. The dividend feed is missing the January 2026 ex-date for both companies. Indicated yields are unaffected. Trailing 12M yields for the windows that cover that quarter understate by one payment.

## Rebuilding

```
python3 scripts/fetch_data.py      # refresh data/ from Yahoo Finance and FRED (needs internet)
python3 scripts/build_dashboard.py # regenerate docs/index.html from data/
```

Both scripts use only the Python standard library.
