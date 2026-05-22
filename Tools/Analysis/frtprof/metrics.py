"""Derived metrics: portal off/on pairing, speedup, sanity checks."""
from __future__ import annotations

import pandas as pd


# Config axes that identify a comparable run (everything except the portal flag).
PAIR_KEYS = ["spp", "max_bounces", "rr_depth"]


def pair_portals(profiles: pd.DataFrame) -> pd.DataFrame:
    """Join portal-off vs portal-on profiles sharing the same (spp, bounces, rr).

    Pairing is by axis *values*, not config index — robust to sweep size and
    axis nesting order. Adds speedup and saving columns.
    """
    off = profiles[profiles["portal_prefilter"] == 0]
    on = profiles[profiles["portal_prefilter"] == 1]
    paired = off.merge(on, on=PAIR_KEYS, suffixes=("_off", "_on"))

    # "percent faster" = (off / on - 1) * 100
    paired["rt_faster_pct"] = (paired["rt_ms_mean_off"] / paired["rt_ms_mean_on"] - 1.0) * 100.0
    paired["rt_saved_ms"] = paired["rt_ms_mean_off"] - paired["rt_ms_mean_on"]
    paired["gpu_faster_pct"] = (paired["gpu_ms_mean_off"] / paired["gpu_ms_mean_on"] - 1.0) * 100.0
    paired["frame_faster_pct"] = (paired["frame_ms_mean_off"] / paired["frame_ms_mean_on"] - 1.0) * 100.0
    paired["tlas_reduction"] = (
        1.0 - paired["tlas_dispatches_mean_on"] / paired["tlas_dispatches_mean_off"]
    )
    return paired.sort_values(PAIR_KEYS).reset_index(drop=True)


def paired_summary(paired: pd.DataFrame) -> pd.DataFrame:
    """Tidy subset of :func:`pair_portals` output for tables and quick reading."""
    cols = {
        "spp": "spp",
        "max_bounces": "bounces",
        "rr_depth": "rr",
        "rt_ms_mean_off": "rt_off_ms",
        "rt_ms_mean_on": "rt_on_ms",
        "rt_faster_pct": "rt_faster_%",
        "rt_saved_ms": "saved_ms",
        "pct_filtered_mean_on": "pct_filtered",
        "mean_portal_tests_mean_on": "E[K]",
        "tlas_reduction": "tlas_reduction",
    }
    available = {k: v for k, v in cols.items() if k in paired.columns}
    return paired[list(available)].rename(columns=available)


def results_table(paired: pd.DataFrame) -> pd.DataFrame:
    """T2 results: one row per (spp, bounces), averaged over rr_depth.

    Speedups are recomputed from the averaged ms so the table is internally
    consistent (displayed speedup == displayed off / on).
    """
    grouped = paired.groupby(["spp", "max_bounces"], as_index=False).agg(
        rt_off_ms=("rt_ms_mean_off", "mean"),
        rt_on_ms=("rt_ms_mean_on", "mean"),
        frame_off_ms=("frame_ms_mean_off", "mean"),
        frame_on_ms=("frame_ms_mean_on", "mean"),
        pct_filtered=("pct_filtered_mean_on", "mean"),
    )
    # "percent faster" = (off / on - 1) * 100
    grouped["rt_faster_pct"] = (grouped["rt_off_ms"] / grouped["rt_on_ms"] - 1.0) * 100.0
    grouped["frame_faster_pct"] = (grouped["frame_off_ms"] / grouped["frame_on_ms"] - 1.0) * 100.0
    # to percent: the rejection rate clusters near 1, a 3-dp table would print
    # every row as 0.996 and lose the variation.
    grouped["pct_filtered"] *= 100.0
    grouped = grouped.rename(columns={"max_bounces": "bounces",
                                      "pct_filtered": "pct_filtered_%"})
    cols = ["spp", "bounces", "rt_off_ms", "rt_on_ms", "rt_faster_pct",
            "frame_off_ms", "frame_on_ms", "frame_faster_pct", "pct_filtered_%"]
    return grouped[cols].sort_values(["spp", "bounces"]).reset_index(drop=True)


def results_extremes(results: pd.DataFrame, n: int = 3,
                     by: str = "rt_faster_pct") -> pd.DataFrame:
    """Top-``n`` and bottom-``n`` rows of :func:`results_table` by ``by``.

    Same columns as the input — the in-text table is the appendix table cut to
    its best and worst cases. If there are <= 2n rows the whole table is
    returned unchanged.
    """
    ordered = results.sort_values(by, ascending=False).reset_index(drop=True)
    if len(ordered) <= 2 * n:
        return ordered
    return pd.concat([ordered.head(n), ordered.tail(n)]).reset_index(drop=True)


def scene_table(configs: pd.DataFrame, portals: pd.DataFrame) -> pd.DataFrame:
    """T1 scene descriptor as a two-column (field, value) table.

    A profiling session uses one fixed scene, so static fields are read from
    the first config. Values are stringified for uniform table output.
    """
    c = configs.iloc[0]
    rows = [
        ("render resolution", f"{int(c['render_width'])} x {int(c['render_height'])}"),
        ("entities", int(c["entity_count"])),
        ("triangles", int(c["triangle_count"])),
        ("point lights", int(c["point_lights"])),
        ("directional lights", int(c["directional_lights"])),
        ("area-quad lights", int(c["areaquad_lights"])),
        ("portals", int(c["portal_count"])),
        ("portal coverage (total)", f"{c['portal_coverage_total']:.5f}"),
        ("camera position",
         f"({c['camera_position_x']}, {c['camera_position_y']}, {c['camera_position_z']})"),
        ("camera rotation",
         f"({c['camera_rotation_x']}, {c['camera_rotation_y']}, {c['camera_rotation_z']})"),
        ("sun direction",
         f"({c['sun_direction_x']}, {c['sun_direction_y']}, {c['sun_direction_z']})"),
        ("sun intensity", c["sun_intensity"]),
        ("sky intensity", c["sky_intensity"]),
    ]
    scene_portals = portals[portals["config_index"] == c["config_index"]]
    for _, p in scene_portals.iterrows():
        rows.append((
            f"portal {int(p['portal_index'])}",
            f"{p['shape']}, area {p['world_area']}, coverage {p['screen_coverage']:.5f}",
        ))
    return pd.DataFrame([(f, str(v)) for f, v in rows], columns=["field", "value"])


def expected_k(frames: pd.DataFrame) -> pd.DataFrame:
    """E[K] per config, computed two ways for cross-check.

    * ``ek_frame_mean`` — mean of the per-frame ``mean_portal_tests``.
    * ``ek_ratio``      — sum(portal_tests) / sum(sky_attempted).
    """
    grouped = frames.groupby("config_index")
    return pd.DataFrame({
        "ek_frame_mean": grouped["mean_portal_tests"].mean(),
        "ek_ratio": grouped["portal_tests"].sum() / grouped["sky_attempted"].sum(),
    }).reset_index()


def sanity_check(frames: pd.DataFrame) -> list[str]:
    """Return a list of human-readable invariant violations (empty == clean)."""
    issues: list[str] = []
    if frames.empty:
        return ["frames table is empty"]

    base = frames[frames["portal_prefilter"] == 0]
    if "portal_rej" in base and (base["portal_rej"] != 0).any():
        issues.append("portal-off rows with portal_rej != 0")
    if "portal_tests" in base:
        portal_tests = base["portal_tests"].dropna()
        if not portal_tests.empty and (portal_tests != 0).any():
            issues.append("portal-off rows with portal_tests != 0")

    # sky_attempted must split exactly into pass + rej for every row.
    need = {"portal_pass", "portal_rej", "sky_attempted"}
    if need <= set(frames.columns):
        bad = frames["portal_pass"] + frames["portal_rej"] != frames["sky_attempted"]
        if bad.any():
            issues.append(
                f"{int(bad.sum())} rows where portal_pass + portal_rej != sky_attempted"
            )

    return issues
