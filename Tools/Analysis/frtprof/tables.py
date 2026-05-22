"""Thesis tables — format any DataFrame to LaTeX (.tex) and Markdown (.md).

This module is pure formatting: it imports no sibling modules and accepts any
DataFrame, so the notebook controls what gets tabulated.
"""
from __future__ import annotations

from pathlib import Path

import pandas as pd


def to_latex(df: pd.DataFrame, caption: str = None, label: str = None) -> str:
    """LaTeX string for ``df``. Wrapped in a ``table`` env when caption given."""
    return df.to_latex(index=False, float_format="%.3f",
                       caption=caption, label=label)


def _fmt(value) -> str:
    """Format one cell: floats to 3 dp (whole floats collapse to int), NaN blank.

    Strings pass through untouched so callers can pre-format a cell precisely.
    """
    if isinstance(value, str):
        return value
    try:
        f = float(value)
    except (TypeError, ValueError):
        return str(value)
    if f != f:  # NaN
        return ""
    if f == int(f):
        return str(int(f))
    return f"{f:.3f}"


def to_markdown(df: pd.DataFrame) -> str:
    """Markdown table string for ``df``. Dependency-free (no ``tabulate``)."""
    cols = [str(c) for c in df.columns]
    rows = [[_fmt(v) for v in row] for row in df.itertuples(index=False, name=None)]
    widths = [
        max(len(cols[i]), *(len(r[i]) for r in rows)) if rows else len(cols[i])
        for i in range(len(cols))
    ]

    def line(cells) -> str:
        return "| " + " | ".join(c.ljust(widths[i]) for i, c in enumerate(cells)) + " |"

    sep = "| " + " | ".join("-" * w for w in widths) + " |"
    return "\n".join([line(cols), sep] + [line(r) for r in rows])


def write_table(df: pd.DataFrame, out_dir, name: str) -> list:
    """Write ``df`` as ``name.tex`` and ``name.md`` under ``out_dir``."""
    out_dir = Path(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    tex = out_dir / f"{name}.tex"
    md = out_dir / f"{name}.md"
    tex.write_text(to_latex(df), encoding="utf-8")
    md.write_text(to_markdown(df), encoding="utf-8")
    return [tex, md]
