# /// script
# requires-python = ">=3.11"
# dependencies = ["matplotlib>=3.8"]
# ///
"""sweep_plot.py - reconstruction & review plots for a sweep CSV.

Three modes, selected by CLI flag:
  (default) live : redraw <out> every --interval s with only the newest
                    frequency segment - point this at a CSV that
                    sweep_log.py is still writing to, to watch it update.
  --grid          : one PNG containing one small subplot per frequency
                     (all of them) - the final "overview" figure.
  --split         : one PNG per frequency, written to --splitdir.

This module also exposes `LivePlotWorker`, a small background-thread
renderer that `sweep_log.py` imports directly so the live preview can be
updated *without* ever blocking the time-critical SCPI/serial sweep loop:
the sweep thread only has to call `worker.submit(segment)`, a non-blocking
queue push; actual matplotlib rendering happens on a separate thread.
"""
from __future__ import annotations

import argparse
import glob
import logging
import math
import os
import queue
import sys
import threading
import time
from pathlib import Path
from typing import NamedTuple, Optional, Sequence

import matplotlib

matplotlib.use("Agg")  # headless: never opens a GUI window, safe in a thread
import matplotlib.pyplot as plt

from common import CSV_COLUMNS, setup_logging

log = logging.getLogger("sweep_plot")


class Segment(NamedTuple):
    """One frequency's worth of samples, time re-zeroed to its own start."""

    freq_hz: float
    t_ms: list[float]
    adc_v: list[float]
    ref_v: list[float]


# --------------------------------------------------------------------------
# Data loading
# --------------------------------------------------------------------------
def load_rows(path: Path) -> list[tuple[float, float, float, float]]:
    """Parse a sweep CSV into (freq_hz, time_ms, adc_v, gen_ref_v) tuples.

    Malformed lines are skipped with a debug-level log line rather than
    aborting the whole load, since this is routinely called on a file
    that sweep_log.py may still be appending to (a partially written
    final line is expected, not an error).
    """
    rows: list[tuple[float, float, float, float]] = []
    try:
        with open(path, newline="") as f:
            for lineno, line in enumerate(f, 1):
                if line.startswith("#") or line.startswith(CSV_COLUMNS[0]):
                    continue
                parts = line.strip().split(",")
                if len(parts) != 4:
                    continue
                try:
                    rows.append(tuple(float(x) for x in parts))  # type: ignore[arg-type]
                except ValueError:
                    log.debug("skipping malformed row %d in %s: %r", lineno, path, line)
    except FileNotFoundError:
        log.warning("CSV not found (yet?): %s", path)
    except OSError as exc:
        log.error("failed to read %s: %s", path, exc)
    return rows


def segments(rows: Sequence[tuple[float, float, float, float]]) -> list[Segment]:
    """Group rows by (contiguous) frequency, zeroing time to segment start."""
    raw_segs: list[list] = []
    for f, t, a, r in rows:
        if not raw_segs or raw_segs[-1][0] != f:
            raw_segs.append([f, [], [], []])
        s = raw_segs[-1]
        s[1].append(t)
        s[2].append(a)
        s[3].append(r)
    out: list[Segment] = []
    for f, ts, ad, rf in raw_segs:
        t0 = ts[0]
        out.append(Segment(f, [x - t0 for x in ts], ad, rf))
    return out


# --------------------------------------------------------------------------
# Rendering primitives
# --------------------------------------------------------------------------
def style_ax(ax, seg: Segment, big: bool) -> None:
    ax.plot(seg.t_ms, seg.ref_v, "-", lw=1.0, color="tab:gray", label="ref" if big else None)
    ax.plot(
        seg.t_ms,
        seg.adc_v,
        ".-",
        ms=2 if big else 1.2,
        lw=0.6,
        color="tab:blue",
        label="adc" if big else None,
    )
    ax.set_title(f"{seg.freq_hz:.0f} Hz", fontsize=11 if big else 8)
    ax.grid(alpha=0.3)
    if big:
        ax.set_xlabel("ms in segment")
        ax.set_ylabel("V")


def draw_live(seg: Optional[Segment], out: Path) -> None:
    fig, ax = plt.subplots(figsize=(9, 4.5))
    try:
        if seg is None:
            ax.set_title("waiting for data...")
        else:
            style_ax(ax, seg, big=True)
            ax.legend(loc="upper right")
        fig.tight_layout()
        fig.savefig(out, dpi=110)
    finally:
        plt.close(fig)


def draw_grid(segs: Sequence[Segment], out: Path, cols: int) -> None:
    n = len(segs)
    nrows = math.ceil(n / cols)
    fig, axes = plt.subplots(nrows, cols, squeeze=False, figsize=(cols * 2.3, nrows * 1.7))
    try:
        for i, row in enumerate(axes):
            for j, ax in enumerate(row):
                k = i * cols + j
                if k < n:
                    style_ax(ax, segs[k], big=False)
                    ax.tick_params(labelsize=6)
                else:
                    ax.axis("off")
        fig.suptitle("measured (blue) vs ideal (grey) - one panel per frequency")
        fig.tight_layout(rect=(0, 0, 1, 0.98))
        fig.savefig(out, dpi=110)
    finally:
        plt.close(fig)


def draw_split(segs: Sequence[Segment], outdir: Path) -> None:
    outdir.mkdir(parents=True, exist_ok=True)
    for seg in segs:
        fig, ax = plt.subplots(figsize=(6, 3))
        try:
            style_ax(ax, seg, big=True)
            ax.legend(loc="upper right", fontsize=8)
            fig.tight_layout()
            fig.savefig(outdir / f"f{seg.freq_hz:07.1f}Hz.png", dpi=110)
        finally:
            plt.close(fig)
    log.info("wrote %d figures to %s/", len(segs), outdir)


# --------------------------------------------------------------------------
# Threaded live-preview worker (used by sweep_log.py)
# --------------------------------------------------------------------------
class LivePlotWorker:
    """Renders the live preview PNG on a dedicated background thread.

    The acquisition loop calls `submit(segment)`, a non-blocking queue
    push, after each frequency step; this thread picks up only the
    *latest* submitted segment (older ones are dropped if rendering can't
    keep up) and redraws the preview image. This keeps slow matplotlib
    rendering from ever delaying the SCPI/serial sweep loop.
    """

    def __init__(self, out_path: Path, logger: Optional[logging.Logger] = None):
        self.out_path = out_path
        self.log = logger or log
        self._queue: "queue.Queue[Optional[Segment]]" = queue.Queue(maxsize=1)
        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._run, name="plot-worker", daemon=True)

    def start(self) -> "LivePlotWorker":
        self._thread.start()
        return self

    def submit(self, seg: Segment) -> None:
        """Non-blocking: replaces any not-yet-rendered pending segment."""
        try:
            self._queue.get_nowait()
        except queue.Empty:
            pass
        try:
            self._queue.put_nowait(seg)
        except queue.Full:
            pass  # extremely unlikely given the get_nowait() above

    def stop(self, timeout: float = 5.0) -> None:
        self._stop.set()
        try:
            self._queue.put_nowait(None)  # wake the worker if it's idle-blocked
        except queue.Full:
            pass
        self._thread.join(timeout=timeout)

    def _run(self) -> None:
        while not self._stop.is_set():
            try:
                seg = self._queue.get(timeout=1.0)
            except queue.Empty:
                continue
            if seg is None:
                continue
            try:
                draw_live(seg, self.out_path)
            except Exception:  # noqa: BLE001 - never let a plot error kill the thread
                self.log.exception("live plot render failed")


# --------------------------------------------------------------------------
# CLI entry point
# --------------------------------------------------------------------------
def _latest_csv() -> Path:
    cands = sorted(glob.glob("data/sweep_*.csv"), key=os.path.getmtime)
    if not cands:
        sys.exit("No data/sweep_*.csv yet. Run sweep_log.py first.")
    return Path(cands[-1])


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--file", type=Path, default=None, help="CSV to read (default: newest in data/)")
    ap.add_argument("--out", type=Path, default=Path("data/sweep_plot.png"))
    ap.add_argument("--grid", action="store_true", help="render the all-frequencies overview grid")
    ap.add_argument("--cols", type=int, default=10)
    ap.add_argument("--split", action="store_true", help="render one PNG per frequency")
    ap.add_argument("--splitdir", type=Path, default=Path("data/plots"))
    ap.add_argument("--interval", type=float, default=2.0, help="live mode redraw interval (s)")
    ap.add_argument("--once", action="store_true", help="live mode: render once and exit")
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()

    setup_logging(None, args.verbose, "sweep_plot")

    path = args.file or _latest_csv()

    if args.grid or args.split:
        segs = segments(load_rows(path))
        if not segs:
            sys.exit("CSV has no data rows yet.")
        if args.grid:
            draw_grid(segs, args.out, args.cols)
            log.info("grid plot (%d panels): %s", len(segs), args.out)
        if args.split:
            draw_split(segs, args.splitdir)
        return

    log.info("plotting %s -> %s every %.1fs (Ctrl+C to stop)", path, args.out, args.interval)
    try:
        while True:
            segs = segments(load_rows(path))
            draw_live(segs[-1] if segs else None, args.out)
            if args.once:
                return
            time.sleep(args.interval)
    except KeyboardInterrupt:
        segs = segments(load_rows(path))
        draw_live(segs[-1] if segs else None, args.out)
        log.info("final plot: %s", args.out)


if __name__ == "__main__":
    main()
