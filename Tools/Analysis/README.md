# FRTEngine2 — Profiling Analysis

Python tooling that parses profiling sessions (`Local/Profiling/<session>/`)
and produces the thesis evaluation: speedup tables, plots, cost-model checks.

## Setup

From this directory (`Tools/Analysis/`):

```bat
python -m venv .venv
.venv\Scripts\activate
pip install -r requirements.txt
python -m ipykernel install --user --name frtprof --display-name "FRTEngine2 Analysis"
```

## Use

```bat
jupyter notebook analyze.ipynb
```

Set `SESSION_DIR` in the configuration cell to the session folder, then run
cells top to bottom. Figures and tables are written to `<session>/analysis/`.

`%autoreload` is enabled — edit any `frtprof/*.py` and re-run a cell; no
kernel restart needed.

## Layout

| File | Role |
|---|---|
| `analyze.ipynb` | Main interactive entry point |
| `frtprof/load.py` | Parse `<i>.txt` + `<i>.csv` into DataFrames |
| `frtprof/metrics.py` | Portal off/on pairing, speedup, sanity checks |
| `frtprof/plots.py` | Matplotlib figures |
| `frtprof/tables.py` | LaTeX / Markdown table emit |
| `frtprof/costmodel.py` | Cost-model fit + eq 2.8a validation (stub) |

## Notes

- The loader keys CSV columns by header name and tolerates schema changes:
  old 16-column exports (pre `portal_tests`) load fine; missing columns
  become `NaN`. No fixed profile count is assumed — any sweep size works.
- `costmodel.validate_eq_28a` is a stub. Paste thesis equation 2.8a into
  `frtprof/costmodel.py` to enable analytic cost validation.
