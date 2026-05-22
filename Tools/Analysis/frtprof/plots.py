"""Matplotlib figures for the thesis evaluation section.

Every function takes a DataFrame and returns a ``matplotlib.figure.Figure``;
none call ``plt.show()``, so they display inline in the notebook and can also
be saved. Use :func:`save_fig` to write PNG + PDF.

Line plots average over ``rr_depth`` unless a single ``rr_depth`` is pinned.
"""
from __future__ import annotations

from pathlib import Path

import matplotlib.pyplot as plt
import numpy as np
import pandas as pd
from matplotlib.lines import Line2D


def _filter_rr(df: pd.DataFrame, rr_depth):
    if rr_depth is None:
        return df
    return df[df["rr_depth"] == rr_depth]


def _by_bounce_spp(df: pd.DataFrame, metric: str) -> pd.DataFrame:
    """Collapse to one (max_bounces, spp) row by averaging the metric."""
    return df.groupby(["max_bounces", "spp"])[metric].mean().reset_index()


def plot_rt_vs_spp(profiles: pd.DataFrame, rr_depth=None):
    """RT dispatch ms vs spp.

    Colour = ``max_bounces``, dashed = portal off, solid = portal on — so each
    off/on pair shares a colour and the saving reads as the gap between the two
    same-coloured lines.
    """
    df = _filter_rr(profiles, rr_depth)
    bounce_vals = sorted(df["max_bounces"].dropna().unique())
    cmap = plt.get_cmap("tab10")
    colour = {b: cmap(i % 10) for i, b in enumerate(bounce_vals)}

    fig, ax = plt.subplots(figsize=(7, 4.5))
    for prefilter, style in [(0, "--"), (1, "-")]:
        agg = _by_bounce_spp(df[df["portal_prefilter"] == prefilter], "rt_ms_mean")
        for bounces, sub in agg.groupby("max_bounces"):
            sub = sub.sort_values("spp")
            ax.plot(sub["spp"], sub["rt_ms_mean"], style, marker="o",
                    color=colour[bounces])
    ax.set_xlabel("samples per pixel")
    ax.set_ylabel("RT dispatch (ms)")
    ax.set_title("Ray-trace dispatch cost vs spp")
    ax.grid(alpha=0.3)

    colour_legend = [Line2D([], [], color=colour[b], marker="o",
                            label=f"bounces={b}") for b in bounce_vals]
    style_legend = [Line2D([], [], color="0.3", ls="--", label="portal off"),
                    Line2D([], [], color="0.3", ls="-", label="portal on")]
    ax.add_artist(ax.legend(handles=colour_legend, loc="upper left", fontsize=8))
    ax.legend(handles=style_legend, loc="lower right", fontsize=8)
    fig.tight_layout()
    return fig


def plot_ek_vs_portal_count(sessions):
    """E[K] vs portal count — one line per max_bounces.

    ``sessions`` is a list of dicts from :func:`frtprof.load.load_session`, one
    per scene. E[K] (mean portal-quad tests per sky shadow ray) is a per-ray
    geometric quantity — independent of spp and rr_depth, so those are averaged
    out. It is driven by portal count, with mild bounce-depth dependence. A
    single session plots one point per bounce; add sessions for scenes with
    other portal counts to see the trend.
    """
    rows = []
    for sess in sessions:
        profiles = sess["profiles"]
        on = profiles[profiles["portal_prefilter"] == 1]
        portal_count = int(sess["configs"].iloc[0]["portal_count"])
        for bounces, sub in on.groupby("max_bounces"):
            rows.append({"portal_count": portal_count, "max_bounces": bounces,
                         "ek": sub["mean_portal_tests_mean"].mean()})
    data = pd.DataFrame(rows)

    fig, ax = plt.subplots(figsize=(7, 4.5))
    if not data.empty:
        for bounces, sub in data.groupby("max_bounces"):
            sub = sub.sort_values("portal_count")
            ax.plot(sub["portal_count"], sub["ek"], marker="o",
                    label=f"bounces={bounces}")
        ax.legend(fontsize=8)
    ax.set_xlabel("portal count")
    ax.set_ylabel("E[K] - mean portal tests / sky ray")
    ax.set_title("Portal-test count E[K] vs portal count")
    ax.grid(alpha=0.3)
    fig.tight_layout()
    return fig


def plot_pct_filtered(profiles: pd.DataFrame, rr_depth=None):
    """Fraction of sky shadow rays rejected by the pre-filter, vs spp.

    Y axis autoscales: the rejection rate clusters very close to 1, so a fixed
    0..1 range would flatten every visible difference.
    """
    df = _filter_rr(profiles[profiles["portal_prefilter"] == 1], rr_depth)
    agg = _by_bounce_spp(df, "pct_filtered_mean")
    fig, ax = plt.subplots(figsize=(7, 4.5))
    for bounces, sub in agg.groupby("max_bounces"):
        sub = sub.sort_values("spp")
        ax.plot(sub["spp"], sub["pct_filtered_mean"], marker="o",
                label=f"bounces={bounces}")
    ax.set_xlabel("samples per pixel")
    ax.set_ylabel("fraction filtered")
    ax.margins(y=0.2)
    ax.set_title("Pre-filter rejection rate")
    ax.grid(alpha=0.3)
    ax.legend(fontsize=8)
    fig.tight_layout()
    return fig


def plot_speedup_heatmap(paired: pd.DataFrame, value: str = "rt_faster_pct", rr_depth=None):
    """Heatmap of ``value`` (default RT percent-faster) over the spp x max_bounces grid."""
    df = _filter_rr(paired, rr_depth)
    grid = df.pivot_table(index="max_bounces", columns="spp", values=value)
    fig, ax = plt.subplots(figsize=(7, 4.5))
    im = ax.imshow(grid.values, origin="lower", aspect="auto", cmap="viridis")
    ax.set_xticks(range(len(grid.columns)), grid.columns)
    ax.set_yticks(range(len(grid.index)), grid.index)
    ax.set_xlabel("samples per pixel")
    ax.set_ylabel("max bounces")
    ax.set_title(f"{value} (portal off vs on)")
    for (i, j), v in np.ndenumerate(grid.values):
        if not np.isnan(v):
            ax.text(j, i, f"{v:.1f}", ha="center", va="center",
                    color="white", fontsize=8)
    fig.colorbar(im, ax=ax)
    fig.tight_layout()
    return fig


def save_fig(fig, out_dir, name: str, formats=("png", "pdf")) -> list:
    """Write ``fig`` to ``out_dir`` as ``name.<fmt>`` for each format."""
    out_dir = Path(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    written = []
    for fmt in formats:
        path = out_dir / f"{name}.{fmt}"
        fig.savefig(path, dpi=150, bbox_inches="tight")
        written.append(path)
    return written
