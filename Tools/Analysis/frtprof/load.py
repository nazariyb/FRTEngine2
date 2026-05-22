"""Load FRTEngine2 profiling sessions into pandas DataFrames.

A session directory holds pairs of files ``<i>.txt`` (config + static scene
descriptor) and ``<i>.csv`` (one row per measured frame). See the project
profiling context for the full schema.

The loader is schema-tolerant: CSV columns are keyed by header name and no
fixed column count is assumed, so old 16-column exports (pre ``portal_tests``)
and future schema changes both load without edits. Missing columns become NaN.
"""
from __future__ import annotations

import re
import warnings
from pathlib import Path

import pandas as pd


_PORTAL_RE = re.compile(r"portal\[(\d+)\]\.(\w+)")

# Columns that identify a row rather than measure something.
_ID_COLS = {"frame", "config_index", "spp", "max_bounces", "rr_depth", "portal_prefilter"}


def _coerce(value: str):
    """Convert a raw txt value string to int / float / 3-tuple / str."""
    value = value.strip()
    parts = value.split()
    if len(parts) > 1:
        try:
            return tuple(float(p) for p in parts)
        except ValueError:
            return value
    try:
        return int(value)
    except ValueError:
        pass
    try:
        return float(value)
    except ValueError:
        return value


def parse_config_txt(path) -> dict:
    """Parse a ``<i>.txt`` config file into a dict.

    Repeated ``portal[i].*`` keys are collected into a ``portals`` list of
    dicts, ordered by index. Every other key maps to a coerced scalar or, for
    space-separated vectors (camera / sun), a 3-tuple of floats.
    """
    cfg: dict = {}
    portals: dict[int, dict] = {}
    for line in Path(path).read_text().splitlines():
        if ":" not in line:
            continue
        key, _, raw = line.partition(":")
        key = key.strip()
        match = _PORTAL_RE.fullmatch(key)
        if match:
            idx, field = int(match.group(1)), match.group(2)
            portals.setdefault(idx, {})[field] = _coerce(raw)
        else:
            cfg[key] = _coerce(raw)
    cfg["portals"] = [portals[i] for i in sorted(portals)]
    return cfg


def read_frames_csv(path) -> pd.DataFrame:
    """Read a ``<i>.csv`` frame table. Columns are keyed by header, not index."""
    return pd.read_csv(path)


def _p95(series: pd.Series) -> float:
    return series.quantile(0.95)


_p95.__name__ = "p95"


def _summarize(frames: pd.DataFrame, configs: pd.DataFrame) -> pd.DataFrame:
    """Per-config aggregate: static config fields + mean/std/p50/p95 per metric."""
    if frames.empty:
        return configs.copy()
    metric_cols = [
        c for c in frames.columns
        if c not in _ID_COLS and pd.api.types.is_numeric_dtype(frames[c])
    ]
    grouped = frames.groupby("config_index")[metric_cols]
    agg = grouped.agg(["mean", "std", "median", _p95])
    agg.columns = [
        f"{metric}_{'p50' if stat == 'median' else stat}"
        for metric, stat in agg.columns
    ]
    agg = agg.reset_index()
    return configs.merge(agg, on="config_index", how="left")


def load_session(session_dir) -> dict:
    """Load every ``<i>.txt`` / ``<i>.csv`` pair under ``session_dir``.

    Returns a plain dict of DataFrames (plain dict — autoreload-safe):

    * ``frames``   — long table, one row per measured frame, all profiles.
    * ``profiles`` — one row per config: static fields + aggregated stats.
    * ``portals``  — long table, one row per portal per config.
    * ``configs``  — one row per config: static fields only (no aggregates).
    """
    session_dir = Path(session_dir)
    if not session_dir.is_dir():
        raise FileNotFoundError(f"session dir not found: {session_dir}")

    txts = sorted(
        (p for p in session_dir.glob("*.txt") if p.stem.isdigit()),
        key=lambda p: int(p.stem),
    )
    if not txts:
        raise FileNotFoundError(f"no <index>.txt files in {session_dir}")

    frame_dfs: list[pd.DataFrame] = []
    config_rows: list[dict] = []
    portal_rows: list[dict] = []

    for txt in txts:
        idx = int(txt.stem)
        cfg = parse_config_txt(txt)

        row: dict = {"config_index": idx}
        for key, val in cfg.items():
            if key == "portals":
                continue
            if isinstance(val, tuple) and len(val) == 3:
                row[f"{key}_x"], row[f"{key}_y"], row[f"{key}_z"] = val
            else:
                row[key] = val

        for portal_index, portal in enumerate(cfg["portals"]):
            portal_rows.append(
                {"config_index": idx, "portal_index": portal_index, **portal}
            )

        csv = txt.with_suffix(".csv")
        if csv.exists():
            fdf = read_frames_csv(csv)
            fdf.insert(0, "config_index", idx)
            for key in ("spp", "max_bounces", "rr_depth", "portal_prefilter"):
                fdf[key] = cfg.get(key)
            row["n_frames"] = len(fdf)
            frame_dfs.append(fdf)
        else:
            warnings.warn(f"{txt.name}: no matching .csv, frames skipped")
            row["n_frames"] = 0

        config_rows.append(row)

    # concat aligns columns by name across schema versions; missing -> NaN.
    frames = pd.concat(frame_dfs, ignore_index=True) if frame_dfs else pd.DataFrame()
    configs = pd.DataFrame(config_rows)
    portals = pd.DataFrame(portal_rows)
    profiles = _summarize(frames, configs)

    return {"frames": frames, "profiles": profiles, "portals": portals, "configs": configs}
