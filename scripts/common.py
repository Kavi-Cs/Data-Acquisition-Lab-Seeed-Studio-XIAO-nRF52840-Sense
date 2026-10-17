# /// script
# requires-python = ">=3.11"
# ///
"""common.py - shared constants, CSV schema and helpers for sweep_log.py
and sweep_plot.py.

Keeping this logic in one place means the acquisition script and the
plotting script can never silently disagree about the CSV column order,
the device wire protocol, or how a log file is named.
"""
from __future__ import annotations

import logging
import re
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path

# --------------------------------------------------------------------------
# Wire protocol (must match firmware/src/sampling.cpp)
# --------------------------------------------------------------------------
# Data rows: "idx,raw"
ROW_RE = re.compile(r"^(\d+),(-?\d+)$")
# Firmware status/diagnostic lines are prefixed with '#', e.g.
#   "# BEGIN n=6000 fs=20000 bad=0 seq=3"
#   "# END"
#   "# ERR capture timeout after 1500 ms (got 4210/6000 samples)"
#   "# WARN requested 80000 > MAX_N, clamped to 50000"
COMMENT_RE = re.compile(r"^#\s*(BEGIN|END|ERR|WARN|VERSION|STATUS|BOOT)\b(.*)$")

# --------------------------------------------------------------------------
# CSV schema (must match what sweep_log.py writes and sweep_plot.py reads)
# --------------------------------------------------------------------------
CSV_HEADER = "freq_hz,time_ms,adc_v,gen_ref_v"
CSV_COLUMNS = ("freq_hz", "time_ms", "adc_v", "gen_ref_v")

ADC_VREF = 3.6
ADC_FULL_SCALE = 4095


def adc_counts_to_volts(raw: int) -> float:
    """Convert a raw 12-bit SAADC code to a measured voltage."""
    return raw * ADC_VREF / ADC_FULL_SCALE


@dataclass
class CaptureRow:
    """One decoded sample row from the device, before unit conversion."""

    idx: int
    raw: int


@dataclass
class CaptureResult:
    """Outcome of one 'C <N>' capture request."""

    rows: list[CaptureRow] = field(default_factory=list)
    got_end: bool = False
    bad: int = 0
    diagnostics: list[str] = field(default_factory=list)


def classify_line(line: str) -> tuple[str | None, str]:
    """Return (kind, payload) for a line from the device.

    kind is one of BEGIN/END/ERR/WARN/VERSION/STATUS/BOOT for firmware
    status lines, or None for an ordinary "idx,raw" data row (payload is
    the raw line in that case).
    """
    m = COMMENT_RE.match(line)
    if m:
        return m.group(1), m.group(2).strip()
    return None, line


def default_csv_path(outdir: Path, board: str) -> Path:
    outdir.mkdir(parents=True, exist_ok=True)
    stamp = time.strftime("%Y%m%d_%H%M%S")
    tag = f"{board}_" if board else ""
    return outdir / f"sweep_{tag}{stamp}.csv"


def setup_logging(log_dir: Path | None, verbose: bool, name: str) -> logging.Logger:
    """Configure console + optional rotating-by-run file logging.

    Console gets INFO (or DEBUG with --verbose); the log file, if
    requested, always gets DEBUG so a run can be diagnosed after the
    fact without needing to reproduce it with --verbose.
    """
    logger = logging.getLogger(name)
    logger.setLevel(logging.DEBUG)
    logger.handlers.clear()

    fmt = logging.Formatter(
        "%(asctime)s.%(msecs)03d [%(levelname)-7s] %(name)s: %(message)s",
        datefmt="%H:%M:%S",
    )

    console = logging.StreamHandler(sys.stdout)
    console.setLevel(logging.DEBUG if verbose else logging.INFO)
    console.setFormatter(fmt)
    logger.addHandler(console)

    if log_dir is not None:
        log_dir.mkdir(parents=True, exist_ok=True)
        stamp = time.strftime("%Y%m%d_%H%M%S")
        file_handler = logging.FileHandler(log_dir / f"{name}_{stamp}.log")
        file_handler.setLevel(logging.DEBUG)
        file_handler.setFormatter(fmt)
        logger.addHandler(file_handler)
        logger.debug("Logging to %s", file_handler.baseFilename)

    return logger
