"""Cost-model fitting and thesis equation (2.8a) validation.

Two models:

* :func:`validate_eq_28a` — the thesis portal pre-filter cost equation. Fits
  its two physical constants across the sweep and reports fit quality.
* :func:`fit_linear_cost` — a coarse empirical fit (RT ms vs TLAS dispatches),
  kept as a quick cross-check.
"""
from __future__ import annotations

import numpy as np
import pandas as pd


def fit_linear_cost(profiles: pd.DataFrame, x_col: str = "tlas_dispatches_mean",
                    y_col: str = "rt_ms_mean") -> dict:
    """Least-squares fit ``y = c0 + c1 * x`` across profiles.

    A coarse empirical model: RT dispatch time vs number of TLAS dispatches.
    Returns coefficients, R^2 and the fitted arrays for plotting.
    """
    data = profiles.dropna(subset=[x_col, y_col])
    x = data[x_col].to_numpy(dtype=float)
    y = data[y_col].to_numpy(dtype=float)
    if len(x) < 2:
        raise ValueError("need at least 2 profiles to fit a cost model")

    design = np.vstack([np.ones_like(x), x]).T
    coef, *_ = np.linalg.lstsq(design, y, rcond=None)
    pred = design @ coef

    ss_res = float(((y - pred) ** 2).sum())
    ss_tot = float(((y - y.mean()) ** 2).sum())
    r2 = 1.0 - ss_res / ss_tot if ss_tot else float("nan")

    return {"c0": float(coef[0]), "c1": float(coef[1]), "r2": r2,
            "x": x, "y": y, "pred": pred, "x_col": x_col, "y_col": y_col}


def validate_eq_28a(paired: pd.DataFrame) -> dict:
    """Validate thesis cost equation (2.8a) by fitting its two constants.

    Per eligible sky shadow ray, the net saving of the portal pre-filter is

        dC = r * E[C_TLAS|P=0]  -  E[K] * C_quad                      (2.8a)

    Per frame the saving is N_elig * dC, so across the sweep

        rt_off - rt_on  ~  (N*r) * c_tlas  -  (N*E[K]) * c_quad

    which is linear in the two unknown constants

        c_tlas = E[C_TLAS|P=0]  -- cost of the TLAS traversal a rejected ray
                                   would otherwise have paid
        c_quad = C_quad         -- cost of one analytic ray-portal test

    (2.8a) is a *decomposition to fit*, not a single-row predictor: neither
    constant is in the CSV. This least-squares fits both across every paired
    config and reports fit quality. Inputs (from ``metrics.pair_portals``):

        r      = pct_filtered_mean_on
        E[K]   = mean_portal_tests_mean_on
        N_elig = sky_attempted_mean_on
        saving = rt_saved_ms   (rt_ms_mean_off - rt_ms_mean_on)

    Caveat: when r and E[K] barely vary across the sweep the two design
    columns are collinear and c_tlas / c_quad are not separately identifiable
    (the combined fit and the per-ray dC still are). The design condition
    number is reported; a scene with varying r / E[K] is needed to pin the
    two constants apart.
    """
    needed = ["pct_filtered_mean_on", "mean_portal_tests_mean_on",
              "sky_attempted_mean_on", "rt_saved_ms"]
    data = paired.dropna(subset=needed)
    if len(data) < 2:
        raise ValueError("need >= 2 paired configs to fit eq 2.8a")

    r = data["pct_filtered_mean_on"].to_numpy(dtype=float)
    ek = data["mean_portal_tests_mean_on"].to_numpy(dtype=float)
    n_elig = data["sky_attempted_mean_on"].to_numpy(dtype=float)
    saving = data["rt_saved_ms"].to_numpy(dtype=float)

    # saving = A*c_tlas - B*c_quad   (no intercept: dC = 0 when r = E[K] = 0)
    col_a = n_elig * r
    col_b = n_elig * ek
    design = np.column_stack([col_a, -col_b])
    coef, *_ = np.linalg.lstsq(design, saving, rcond=None)
    c_tlas, c_quad = float(coef[0]), float(coef[1])

    predicted = design @ coef
    residual = saving - predicted
    ss_res = float((residual ** 2).sum())
    ss_tot = float(((saving - saving.mean()) ** 2).sum())
    r2 = 1.0 - ss_res / ss_tot if ss_tot else float("nan")
    cond = float(np.linalg.cond(design))
    dc = saving / n_elig  # per-ray saving — identifiable even when c_* are not

    result = {
        "c_tlas_ms": c_tlas, "c_quad_ms": c_quad,
        "r2": r2, "rmse_ms": float(np.sqrt(ss_res / len(data))),
        "cond": cond, "n_configs": int(len(data)),
        "dc_mean_ms": float(dc.mean()), "dc_std_ms": float(dc.std()),
        "predicted": predicted, "measured": saving, "residual": residual,
    }

    print(f"[eq 2.8a] fit over {len(data)} configs:  "
          f"R^2 = {r2:.4f}   RMSE = {result['rmse_ms']:.3f} ms")
    print(f"          E[C_TLAS|P=0] = {c_tlas:.3e} ms/ray    "
          f"C_quad = {c_quad:.3e} ms/test")
    print(f"          mean dC = {result['dc_mean_ms']:.3e} "
          f"+/- {result['dc_std_ms']:.3e} ms/ray  (identifiable)")
    if cond > 1.0e3:
        print(f"          WARNING: design condition number = {cond:.1e}. "
              f"r and E[K] are near-constant\n"
              f"          across this sweep, so C_quad and E[C_TLAS|P=0] are "
              f"NOT separately identifiable.\n"
              f"          The combined fit and mean dC stay valid; profile a "
              f"scene with varying\n"
              f"          r / E[K] (multi-portal, varied geometry) to pin the "
              f"two constants apart.")
    if c_tlas < 0.0 or c_quad < 0.0:
        print("          WARNING: a fitted constant is negative - model / data "
              "mismatch.")
    return result
