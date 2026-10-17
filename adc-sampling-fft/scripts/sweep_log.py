# /// script
# requires-python = ">=3.11"
# dependencies = [
#     "pyserial>=3.5",
#     "pyvisa>=1.14",
#     "pyvisa-py>=0.7",
#     "pyusb>=1.2",
#     "matplotlib>=3.8",
# ]
# ///
"""sweep_log.py - automated 10 Hz -> 1 kHz frequency sweep.

Architecture
------------
  Main thread   : drives the SCPI sweep on the waveform generator, reads
                   sample rows off the XIAO over serial, and writes the
                   merged CSV. This is the only thread that owns the
                   serial port and VISA session.
  Worker thread : LivePlotWorker (see sweep_plot.py) renders the live
                   preview PNG. The main thread only ever pushes the
                   latest completed segment onto a 1-slot queue
                   (non-blocking); matplotlib rendering - which can take
                   tens to hundreds of milliseconds for a busy figure -
                   never delays the next SCPI/serial step.

Output: one self-describing CSV per run, "freq_hz,time_ms,adc_v,gen_ref_v"
plus '#'-prefixed metadata header lines, written incrementally so a run
interrupted partway through still leaves a usable partial file.
"""
from __future__ import annotations

import argparse
import logging
import math
import signal
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Optional

import pyvisa
import serial

from common import (
    CaptureResult,
    CaptureRow,
    CSV_HEADER,
    adc_counts_to_volts,
    classify_line,
    default_csv_path,
    setup_logging,
)
from sweep_plot import LivePlotWorker, Segment

log = logging.getLogger("sweep_log")


class InstrumentError(RuntimeError):
    """Raised for any unrecoverable waveform-generator communication fault."""


@dataclass
class SweepConfig:
    port: str
    baud: int
    fs: int
    fstart: float
    fstop: float
    fstep: float
    periods: float
    margin: float
    settle: float
    vpp: float
    voff: float
    maxn: int
    outdir: Path
    board: str
    live_plot: bool
    plot_out: Path


# --------------------------------------------------------------------------
# Waveform generator control
# --------------------------------------------------------------------------
class InstrumentController:
    """Thin, defensive wrapper around a PyVISA SCPI waveform generator."""

    def __init__(self, resource, idn: str):
        self._inst = resource
        self.idn = idn

    @classmethod
    def discover(cls) -> "InstrumentController":
        """Find a Keysight/Agilent arbitrary waveform generator over USB-TMC.

        Matches on IDN substrings rather than a single exact model string,
        since different firmware/serial numbers report slightly different
        identification strings for the same EDU332xx family of
        instruments.
        """
        rm = pyvisa.ResourceManager("@py")
        names: list[str] = []
        try:
            names = list(rm.list_resources("USB?*::INSTR"))
        except Exception as exc:  # noqa: BLE001
            log.debug("list_resources failed: %s", exc)

        for name in names:
            try:
                inst = rm.open_resource(name)
                inst.timeout = 3000
                idn = inst.query("*IDN?").strip()
            except Exception as exc:  # noqa: BLE001
                log.debug("skipping %s: %s", name, exc)
                continue
            up = idn.upper()
            if "33212" in idn or "KEYSIGHT" in up or "AGILENT" in up:
                log.info("generator found: %s (%s)", idn, name)
                return cls(inst, idn)
            inst.close()

        raise InstrumentError(
            "No Keysight/Agilent generator found via pyvisa-py.\n"
            f"  resources seen: {names}\n"
            "  check: USB cable, `lsusb`, udev rule, `sudo apt install libusb-1.0-0`"
        )

    def _write(self, cmd: str) -> None:
        self._inst.write(cmd)

    def _query(self, cmd: str, retries: int = 3, backoff_s: float = 0.2) -> str:
        last_exc: Optional[Exception] = None
        for attempt in range(1, retries + 1):
            try:
                return self._inst.query(cmd).strip()
            except Exception as exc:  # noqa: BLE001
                last_exc = exc
                log.warning("SCPI query %r failed (attempt %d/%d): %s", cmd, attempt, retries, exc)
                time.sleep(backoff_s * attempt)
        raise InstrumentError(f"SCPI query {cmd!r} failed after {retries} attempts") from last_exc

    def configure_sine(self, vpp: float, voff: float) -> None:
        self._write("FUNC SIN")
        self._write(f"VOLT {vpp}")
        self._write(f"VOFF {voff}")
        self._write("PHAS 0")

    def set_output(self, on: bool) -> None:
        self._write(f"OUTP {'ON' if on else 'OFF'}")

    def set_frequency(self, hz: float) -> float:
        """Program a frequency and return the value the instrument confirms."""
        self._write(f"FREQ {hz}")
        return float(self._query("FREQ?"))

    def close(self) -> None:
        try:
            self.set_output(False)  # safety: never leave the output live
        except Exception:  # noqa: BLE001
            log.warning("could not confirm output OFF on close; verify manually")
        finally:
            self._inst.close()


# --------------------------------------------------------------------------
# XIAO acquisition client
# --------------------------------------------------------------------------
class AcquisitionClient:
    """Serial client for the "C <N>" capture-then-dump firmware protocol."""

    def __init__(self, port: str, baud: int):
        self._ser = serial.Serial(port, baud, timeout=0.5)
        time.sleep(1.0)  # let the USB-CDC enumeration / firmware boot settle
        self._ser.reset_input_buffer()

    def capture(self, n: int, fs: int) -> CaptureResult:
        result = CaptureResult()
        self._ser.reset_input_buffer()
        self._ser.write(f"C {n}\n".encode())

        # capture time + generous transfer-time allowance + slack
        deadline = time.monotonic() + (n / fs) + (n * 12 / 150_000) + 5.0
        while time.monotonic() < deadline:
            try:
                raw_line = self._ser.readline()
            except serial.SerialException as exc:
                raise InstrumentError(f"serial read failed: {exc}") from exc
            line = raw_line.decode("utf-8", "replace").strip()
            if not line:
                continue
            kind, payload = classify_line(line)
            if kind == "END":
                result.got_end = True
                return result
            if kind == "ERR":
                result.diagnostics.append(payload)
                log.error("firmware: %s", payload)
                return result
            if kind == "WARN":
                result.diagnostics.append(payload)
                log.warning("firmware: %s", payload)
                continue
            if kind is not None:  # BEGIN/VERSION/STATUS/BOOT - informational
                log.debug("firmware: %s %s", kind, payload)
                continue
            from common import ROW_RE

            m = ROW_RE.match(line)
            if m:
                result.rows.append(CaptureRow(int(m.group(1)), int(m.group(2))))
        return result

    def close(self) -> None:
        self._ser.close()


# --------------------------------------------------------------------------
# Sweep orchestration
# --------------------------------------------------------------------------
class SweepRunner:
    def __init__(self, cfg: SweepConfig, gen: InstrumentController, acq: AcquisitionClient):
        self.cfg = cfg
        self.gen = gen
        self.acq = acq
        self.plot_worker: Optional[LivePlotWorker] = None
        self._abort = False

    def install_signal_handlers(self) -> None:
        def _handler(signum, _frame):
            log.warning("received signal %s - finishing current step then stopping", signum)
            self._abort = True

        signal.signal(signal.SIGINT, _handler)
        signal.signal(signal.SIGTERM, _handler)

    def run(self) -> Path:
        cfg = self.cfg
        csv_path = default_csv_path(cfg.outdir, cfg.board)
        n_steps = int((cfg.fstop - cfg.fstart) // cfg.fstep) + 1

        if cfg.live_plot:
            self.plot_worker = LivePlotWorker(cfg.plot_out).start()
            log.info("live preview: %s (updated on a background thread)", cfg.plot_out)

        self.gen.configure_sine(cfg.vpp, cfg.voff)
        self.gen.set_output(True)

        try:
            with open(csv_path, "w") as f:
                self._write_header(f, csv_path)
                step = 0
                fr = cfg.fstart
                while fr <= cfg.fstop + 1e-9 and not self._abort:
                    step += 1
                    self._run_step(f, step, n_steps, fr)
                    fr += cfg.fstep
            log.info("done. CSV: %s", csv_path)
        finally:
            self.gen.set_output(False)
            if self.plot_worker is not None:
                self.plot_worker.stop()
        return csv_path

    def _write_header(self, f, csv_path: Path) -> None:
        cfg = self.cfg
        f.write(f"# board={cfg.board or 'unknown'}\n")
        f.write(f"# generator={self.gen.idn}\n")
        f.write(
            f"# sweep sine {cfg.fstart}..{cfg.fstop} Hz step {cfg.fstep}, "
            f"vpp={cfg.vpp}, voff={cfg.voff}, phase_anchor=window_start\n"
        )
        f.write(f"# fs={cfg.fs}, periods={cfg.periods}, margin={cfg.margin}, settle={cfg.settle}\n")
        f.write(f"# started={time.strftime('%Y%m%d_%H%M%S')}, adc_v=raw*3.6/4095\n")
        f.write(CSV_HEADER + "\n")

    def _run_step(self, f, step: int, n_steps: int, fr: float) -> None:
        cfg = self.cfg
        try:
            got = self.gen.set_frequency(fr)
        except InstrumentError as exc:
            log.error("[%3d/%d] could not set frequency %.1f Hz: %s", step, n_steps, fr, exc)
            return
        time.sleep(cfg.settle)

        n_want = int(cfg.periods * cfg.fs / got)
        n = min(n_want, cfg.maxn)

        result = self.acq.capture(n, cfg.fs)

        if not result.rows:
            log.warning("[%3d/%d] f=%7.1f Hz  NO ROWS - check firmware/port", step, n_steps, got)
            return

        t_ms_list, adc_v_list, ref_v_list = [], [], []
        for row in result.rows:
            t_ms = row.idx * 1000.0 / cfg.fs
            ref = cfg.voff + (cfg.vpp / 2.0) * math.sin(2.0 * math.pi * got * (t_ms / 1000.0))
            adc_v = adc_counts_to_volts(row.raw)
            f.write(f"{got:.1f},{t_ms:.2f},{adc_v:.4f},{ref:.4f}\n")
            t_ms_list.append(t_ms)
            adc_v_list.append(adc_v)
            ref_v_list.append(ref)
        f.flush()

        if self.plot_worker is not None:
            t0 = t_ms_list[0]
            self.plot_worker.submit(
                Segment(got, [t - t0 for t in t_ms_list], adc_v_list, ref_v_list)
            )

        flag = ""
        if not result.got_end:
            flag += "  <-- no END (transfer cut off)"
        if len(result.rows) != n:
            flag += "  <-- rows != requested"
        if n < n_want:
            flag += f"  (capped: {n * got / cfg.fs:.1f} periods)"
        log.info(
            "[%3d/%d] f=%7.1f Hz  rows=%6d (asked %d)%s",
            step, n_steps, got, len(result.rows), n, flag,
        )


# --------------------------------------------------------------------------
# CLI entry point
# --------------------------------------------------------------------------
def parse_args() -> argparse.Namespace:
    ap = argparse.ArgumentParser(description="SCPI frequency sweep + XIAO ADC logging")
    ap.add_argument("--board", default="", help="board name, added to the CSV filename")
    ap.add_argument("--port", required=True, help="XIAO serial port, e.g. /dev/ttyACM0")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--fs", type=int, default=20000, help="must match FS in sampling.cpp")
    ap.add_argument("--fstart", type=float, default=10)
    ap.add_argument("--fstop", type=float, default=1000)
    ap.add_argument("--fstep", type=float, default=10)
    ap.add_argument("--periods", type=float, default=30)
    ap.add_argument("--margin", type=float, default=0.2)
    ap.add_argument("--settle", type=float, default=0.1)
    ap.add_argument("--vpp", type=float, default=3.0)
    ap.add_argument("--voff", type=float, default=1.5)
    ap.add_argument("--maxn", type=int, default=50000, help="must be <= MAX_N in sampling.cpp")
    ap.add_argument("--outdir", type=Path, default=Path("data"))
    ap.add_argument("--yes", action="store_true", help="skip the safety prompt")
    ap.add_argument("--no-live-plot", action="store_true", help="disable the background live preview")
    ap.add_argument("--plot-out", type=Path, default=Path("data/sweep_plot.png"))
    ap.add_argument("--log-dir", type=Path, default=Path("logs"), help="run log directory (set to '' to disable)")
    ap.add_argument("--verbose", action="store_true")
    return ap.parse_args()


def main() -> int:
    args = parse_args()
    log_dir = args.log_dir if str(args.log_dir) else None
    setup_logging(log_dir, args.verbose, "sweep_log")
    # sweep_plot logs through the same root config; give it its own logger too
    setup_logging(log_dir, args.verbose, "sweep_plot")

    try:
        gen = InstrumentController.discover()
    except InstrumentError as exc:
        log.error(str(exc))
        return 1

    print(f"Generator: {gen.idn}")
    print(
        f"Output will be SINE {args.vpp} Vpp, {args.voff} V offset "
        f"(swing 0.0..{args.vpp} V) into the XIAO ADC."
    )
    if not args.yes:
        ans = input("Enable output and start the sweep? [yes/N] ").strip().lower()
        if ans not in ("y", "yes"):
            gen.set_output(False)
            print("Aborted.")
            return 1

    try:
        acq = AcquisitionClient(args.port, args.baud)
    except serial.SerialException as exc:
        log.error("could not open serial port %s: %s", args.port, exc)
        gen.set_output(False)
        return 1

    cfg = SweepConfig(
        port=args.port,
        baud=args.baud,
        fs=args.fs,
        fstart=args.fstart,
        fstop=args.fstop,
        fstep=args.fstep,
        periods=args.periods,
        margin=args.margin,
        settle=args.settle,
        vpp=args.vpp,
        voff=args.voff,
        maxn=args.maxn,
        outdir=args.outdir,
        board=args.board,
        live_plot=not args.no_live_plot,
        plot_out=args.plot_out,
    )

    runner = SweepRunner(cfg, gen, acq)
    runner.install_signal_handlers()
    try:
        runner.run()
    except InstrumentError as exc:
        log.error("unrecoverable instrument error: %s", exc)
        return 1
    finally:
        acq.close()
        gen.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
