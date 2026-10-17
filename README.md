# XIAO nRF52840 Sense — Automated ADC Sampling, Reconstruction & FFT

> **Branch notice:** this branch (`feature/adc-sampling-fft`) is dedicated
> **exclusively** to the ADC sampling / reconstruction / FFT laboratory
> project described below. It is unrelated to, and does not touch, the
> "Voice Controlled LED with Edge Impulse" project on `main`. Do not merge
> this branch into `main`; open it as its own PR / keep it as a standalone
> reference branch.

A constant-rate data-acquisition pipeline for the Seeed Studio **XIAO
nRF52840 Sense**: hardware-timer-driven ADC sampling at a strict 20 kHz,
SCPI-automated signal generator sweeps, CSV logging, time-domain
reconstruction, and FFT-based verification of the Nyquist–Shannon sampling
criterion across a 10 Hz – 1 kHz sweep.

```
┌─────────────────────────┐          USB-TMC / SCPI           ┌────────────────────────┐
│  Keysight EDU33212A      │ <───────────────────────────────> │  scripts/sweep_log.py   │
│  arbitrary waveform gen. │        (PyVISA, pyvisa-py)        │  (main thread: sweep)   │
└─────────────────────────┘                                    └───────────┬────────────┘
         │ SINE, 0–3.0 V swing                                             │ non-blocking
         ▼                                                                 │ segment submit
┌─────────────────────────┐          USB-CDC / serial          ┌───────────▼────────────┐
│  XIAO nRF52840 Sense      │ <───────────────────────────────> │  LivePlotWorker thread  │
│  AIN0 ← signal            │   "C <N>\n" / "idx,raw" / "#END"  │  (sweep_plot.py)        │
│                           │                                    └────────────────────────┘
│  NRF_TIMER3 (20 kHz) ──▶ │
│  NRF_SAADC (12-bit)  ──▶ │ RAM buffer (≤ 50 000 samples)
└─────────────────────────┘
                                                                  data/sweep_<ts>.csv
                                                                  data/sweep_plot.png (live)
                                                                  data/sweep_plot_grid.png
```

> **Note on the signal generator identification:** the capture scripts
> auto-discover the instrument by `*IDN?` and match on the substring
> `"33212"` (see `InstrumentController.discover` in `scripts/sweep_log.py`),
> which corresponds to a **Keysight EDU33212A** arbitrary waveform
> generator. If your bench instrument reports a different model string,
> update the match condition in `discover()` accordingly.

---

## 1. Repository layout

```
.
├── README.md
├── platformio.ini          # firmware build config (primary toolchain)
├── CMakeLists.txt          # CMake wrapper around PlatformIO, for IDE integration
├── Makefile                 # `make build/upload/monitor/sweep/plot/grid/...`
├── .gitignore
├── firmware/
│   ├── src/
│   │   └── sampling.cpp     # hardware-timed 20 kHz ADC acquisition firmware
│   └── include/
│       └── fmt.h            # tiny "{}"-style logging helper used by sampling.cpp
└── scripts/
    ├── requirements.txt
    ├── common.py             # shared wire-protocol / CSV-schema / logging helpers
    ├── sweep_log.py          # SCPI sweep + serial capture + CSV writer (main thread)
    └── sweep_plot.py         # reconstruction/grid plots + threaded live-preview worker
```

## 2. Hardware setup

### 2.1 Bill of materials
- Seeed Studio XIAO nRF52840 Sense
- Keysight EDU33212A arbitrary waveform generator (or compatible SCPI AWG)
- USB-C cable (XIAO ↔ host) and USB cable (generator ↔ host)
- Jumper wires

### 2.2 Pinout and connections

| XIAO nRF52840 Sense pin | Signal                         | Connects to |
|--------------------------|-------------------------------|-------------|
| `A0` / `D0` (P0.02, AIN0) | Analogue input, sampled by `NRF_SAADC` channel 0 | Generator output (via the voltage-limited scheme below) |
| `GND`                     | Common ground                  | Generator ground |
| `USB-C`                   | Power + serial (`Serial`, 115200 baud) | Host PC |

> Confirm `A0` against Seeed's official XIAO nRF52840 Sense pinout
> diagram before wiring, in case your board revision differs.

### 2.3 Circuit protection (no snubber required)
Rather than a dedicated snubber/clamping network, input protection is
achieved by **bounding the generator's output directly at the source**,
enforced in software by `sweep_log.py`'s defaults:

- Waveform: sine
- `Vpp = 3.0 V`, `Voffset = 1.5 V` → output swings strictly **0.0 V – 3.0 V**

This lies entirely inside the SAADC channel's configured input window
(0 – 3.6 V, internal reference, 1/6 gain — see `saadcInit()` in
`sampling.cpp`), so `AIN0` is never driven out of range. If you change
`--vpp`/`--voff`, keep the resulting swing within 0–3.6 V.

## 3. Software setup

### 3.1 Firmware toolchain (PlatformIO)
```bash
pip install platformio
pio boards | grep -i xiao      # confirm the board ID, see note in platformio.ini
```
If the board ID PlatformIO reports differs from `seeed_xiaoblesense`,
update it in `platformio.ini` under `[env:xiao_nrf52840_sense]`.

### 3.2 Python environment
```bash
make venv                       # creates .venv/ and installs scripts/requirements.txt
```
or manually:
```bash
python3 -m venv .venv
source .venv/bin/activate
pip install -r scripts/requirements.txt
```

### 3.3 Linux USB permissions (if needed)
`pyvisa-py` and the serial port both require user-level USB access:
```bash
sudo usermod -aG dialout,plugdev "$USER"   # then log out/in
```
If the generator isn't discovered, check `lsusb` and that a VISA/USBTMC
udev rule is installed for your distribution.

## 4. Build, flash and run

```bash
# 1. Create and switch to the dedicated branch (if not already on it)
git checkout -b feature/adc-sampling-fft

# 2. Build the firmware
make build
#   (equivalent: pio run -e xiao_nrf52840_sense)

# 3. Flash it to the XIAO (board in bootloader mode if required: double-tap RESET)
make upload PORT=/dev/ttyACM0

# 4. (optional) Watch the firmware boot banner / diagnostics
make monitor PORT=/dev/ttyACM0
# expect: "# BOOT fw=7.0 FS=20000 Hz MAX_N=50000 saadc_ok=1"

# 5. Run the automated 10 Hz -> 1 kHz sweep (close the monitor first - only
#    one process can own the serial port at a time)
make sweep PORT=/dev/ttyACM0
#   equivalent:
#   .venv/bin/python scripts/sweep_log.py --port /dev/ttyACM0

# 6. Generate the analysis plots from the CSV just produced
make grid     # data/sweep_plot.png -> one-panel-per-frequency overview
make split    # data/plots/f<freq>Hz.png -> one PNG per frequency
make plot     # live-watch mode (also runs automatically, in a background
              #  thread, during `make sweep` unless --no-live-plot is passed)
```

All outputs land under `data/` (CSV, PNGs) and `logs/` (per-run execution
logs from `scripts/common.py:setup_logging`), both git-ignored.

### 4.1 Useful `sweep_log.py` flags
| Flag | Default | Meaning |
|---|---|---|
| `--fstart` / `--fstop` / `--fstep` | `10` / `1000` / `10` | sweep range in Hz |
| `--fs` | `20000` | must match `FS` in `sampling.cpp` |
| `--periods` | `30` | target periods captured per frequency |
| `--maxn` | `50000` | must be ≤ `MAX_N` in `sampling.cpp` |
| `--vpp` / `--voff` | `3.0` / `1.5` | generator amplitude/offset (keep swing within 0–3.6 V) |
| `--no-live-plot` | off | disable the background live-preview thread |
| `--verbose` | off | DEBUG-level console logging |
| `--log-dir` | `logs` | per-run log file location (`''` disables file logging) |

## 5. Firmware serial protocol

115200 baud, 8N1. Backward compatible with the original "capture-then-dump"
design: no data is sent over USB while a capture is in progress, so USB
activity can never introduce sampling jitter.

| Host → device | Meaning |
|---|---|
| `C <N>\n` | capture `N` samples (1..`MAX_N`) at `FS` Hz, then dump them |
| `V\n` | report firmware version / build configuration |
| `S\n` | report current status (SAADC ok, capturing, capture sequence #) |

| Device → host | Meaning |
|---|---|
| `# BEGIN n=<N> fs=<FS> bad=<K> seq=<S>` | start of a dump |
| `<idx>,<raw>` | one sample row (`raw` is `int16_t`; `-32768` = failed conversion) |
| `# END` | dump complete |
| `# ERR <reason>` | command rejected or capture/dump aborted (see firmware comments) |
| `# WARN <reason>` | request was serviced but modified (e.g. `N` clamped to `MAX_N`) |

## 6. Architecture notes

- **Timing precision:** `NRF_TIMER3` free-runs at 1 MHz and advances its
  compare register by a *fixed* increment every interrupt
  (`CC[0] += TICK_US`), so the sample clock is anchored to absolute time
  and cannot accumulate drift from interrupt latency or ISR execution
  time — jitter from `delay()`-based loops is eliminated by construction.
- **Capture-then-transfer:** samples are written into a RAM buffer (up to
  `MAX_N = 50 000`, ~100 KB) during acquisition; USB/CDC transfer only
  starts once the timer has stopped, fully decoupling the time-critical
  sample loop from the slower, non-deterministic USB stack.
- **Bounded recovery, not infinite spins:** both the host-side capture
  wait and the firmware's own command handling use bounded timeouts
  (`handleCaptureCmd` in `sampling.cpp`; capture `deadline` in
  `AcquisitionClient.capture`) rather than unconditional blocking loops,
  so a stalled USB link or ISR fault cannot hang either side indefinitely.
- **UI never blocks acquisition:** `sweep_log.py`'s main thread only ever
  performs a non-blocking queue push (`LivePlotWorker.submit`) after each
  frequency step; matplotlib rendering happens entirely on a separate
  daemon thread, so a slow plot redraw can never delay the next SCPI
  command or serial read.

## 7. Troubleshooting

| Symptom | Likely cause / fix |
|---|---|
| `No Keysight/Agilent generator found via pyvisa-py` | Check `lsusb`, USB-TMC udev rule, that no other VISA session (e.g. Keysight Connection Expert) has the instrument open |
| `could not open serial port ...` | Wrong `--port`; check `ls /dev/ttyACM*` (Linux) or Device Manager (Windows); close any open serial monitor first |
| `# ERR capture timeout ...` in firmware output | USB enumeration hiccup or SAADC fault — power-cycle the XIAO, re-check `AIN0` wiring |
| `NO ROWS - check firmware/port` from `sweep_log.py` | Firmware not flashed / wrong baud rate / board in bootloader mode |
| Board ID in `platformio.ini` doesn't exist in your PlatformIO install | Run `pio boards \| grep -i xiao` and update `board=` accordingly |

## 8. License

No license file is included yet — add a `LICENSE` (e.g. MIT) before
treating this branch as reusable by others.
