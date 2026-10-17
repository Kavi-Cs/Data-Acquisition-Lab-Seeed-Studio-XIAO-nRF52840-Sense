"""
fft_analysis.py
----------------
Reads a block of raw 12-bit ADC samples streamed over Serial from the
XIAO nRF52840 Sense sampling sketch, reconstructs the time-domain
waveform, and plots its FFT spectrum.
"""

import struct
import sys
import time
import os

import numpy as np
import serial
from scipy.fft import rfft, rfftfreq
import matplotlib.pyplot as plt

# =====================================================================
# CONFIGURATION
# =====================================================================
COM_PORT         = "COM13"
BAUD_RATE        = 115200

NUM_SAMPLES      = 2048
SAMPLING_RATE_HZ = 10000

TEST_FREQUENCY_LABEL = "10Hz"

ADC_RESOLUTION_BITS = 12
ADC_MAX_COUNT       = (1 << ADC_RESOLUTION_BITS) - 1
ADC_REF_VOLTAGE     = 3.3

DC_OFFSET_VOLTAGE   = 1.5

APPLY_WINDOW     = True
SYNC_WORD        = 0xAA55
# =====================================================================

if len(sys.argv) > 1:
    TEST_FREQUENCY_LABEL = sys.argv[1]


def read_one_block(ser: serial.Serial, num_samples: int) -> np.ndarray:
    sync_low  = SYNC_WORD & 0xFF
    sync_high = (SYNC_WORD >> 8) & 0xFF

    last_status = time.time()

    while True:
        b = ser.read(1)
        if len(b) == 0:
            if time.time() - last_status > 3:
                print("Still waiting for data - check COM_PORT, that the "
                      "sketch is running, and that nothing else has the port open.",
                      file=sys.stderr)
                last_status = time.time()
            continue

        if b[0] != sync_low:
            continue

        b2 = ser.read(1)
        if len(b2) == 0 or b2[0] != sync_high:
            continue

        last_status = time.time()

        count_bytes = ser.read(2)
        if len(count_bytes) < 2:
            continue
        (count,) = struct.unpack("<H", count_bytes)

        if count != num_samples:
            print(f"WARNING: device block size ({count}) != NUM_SAMPLES "
                  f"({num_samples}) - skipping this block.", file=sys.stderr)
            ser.read(count * 2)
            continue

        payload = ser.read(count * 2)
        if len(payload) < count * 2:
            print("WARNING: short read, retrying block.", file=sys.stderr)
            continue

        raw = np.frombuffer(payload, dtype="<u2")
        return raw


def raw_to_voltage(raw_counts: np.ndarray) -> np.ndarray:
    return (raw_counts.astype(np.float64) / ADC_MAX_COUNT) * ADC_REF_VOLTAGE


def main():
    print(f"Test frequency label: {TEST_FREQUENCY_LABEL}")

    data_path = f"data_{TEST_FREQUENCY_LABEL}.csv"
    graph_path = f"graph_{TEST_FREQUENCY_LABEL}.png"
    if os.path.exists(data_path) or os.path.exists(graph_path):
        print(f"WARNING: {data_path} and/or {graph_path} already exist and will be overwritten.")

    print(f"Opening {COM_PORT} @ {BAUD_RATE} baud ...")
    
    try:
        with serial.Serial(COM_PORT, BAUD_RATE, timeout=2) as ser:
            # Wake up the XIAO's Native USB CDC connection
            ser.dtr = True
            ser.rts = True
            
            # Give the board 2 seconds to exit while(!Serial) and start streaming
            time.sleep(2.0) 
            
            ser.reset_input_buffer()
            print(f"Waiting for a {NUM_SAMPLES}-sample block ...")
            raw_counts = read_one_block(ser, NUM_SAMPLES)
    except serial.SerialException as e:
        print(f"\n[ERROR] Failed to open {COM_PORT}: {e}")
        print("-> FIX: Ensure the Arduino IDE Serial Monitor, PuTTY, or other scripts are closed.")
        print("-> FIX: Try physically replugging the XIAO board into the USB port.")
        sys.exit(1)

    voltages = raw_to_voltage(raw_counts)

    # ---- Time domain reconstruction ----
    sample_period_s = 1.0 / SAMPLING_RATE_HZ
    t = np.arange(NUM_SAMPLES) * sample_period_s

    # ---- Auto-save data ----
    data_filename = f"data_{TEST_FREQUENCY_LABEL}.csv"
    np.savetxt(
        data_filename,
        np.column_stack((t, voltages)),
        delimiter=",",
        header="time_s,voltage_v",
        comments="",
        fmt="%.8f",
    )
    print(f"Saved data: {data_filename}")

    # ---- Frequency domain (FFT) ----
    ac_signal = voltages - np.mean(voltages)

    if APPLY_WINDOW:
        window = np.hanning(NUM_SAMPLES)
        windowed = ac_signal * window
        coherent_gain = np.sum(window) / NUM_SAMPLES
    else:
        windowed = ac_signal
        coherent_gain = 1.0

    spectrum = rfft(windowed)
    freqs = rfftfreq(NUM_SAMPLES, d=sample_period_s)

    magnitude = (np.abs(spectrum) / NUM_SAMPLES) / coherent_gain
    magnitude[1:] *= 2 

    peak_idx = np.argmax(magnitude[1:]) + 1 
    peak_freq = freqs[peak_idx]
    peak_mag = magnitude[peak_idx]

    print(f"Detected dominant frequency: {peak_freq:.2f} Hz (amplitude {peak_mag:.4f} V)")

    # ---- Plotting ----
    fig, (ax_time, ax_freq) = plt.subplots(2, 1, figsize=(10, 8))

    ax_time.plot(t * 1000, voltages, linewidth=1)
    ax_time.axhline(DC_OFFSET_VOLTAGE, color="gray", linestyle="--", linewidth=0.8,
                     label=f"Expected DC offset ({DC_OFFSET_VOLTAGE} V)")
    ax_time.set_title(f"Time Domain - Reconstructed Signal ({NUM_SAMPLES} samples @ {SAMPLING_RATE_HZ} Hz)")
    ax_time.set_xlabel("Time (ms)")
    ax_time.set_ylabel("Voltage (V)")
    ax_time.legend(loc="upper right")
    ax_time.grid(True, alpha=0.3)

    ax_freq.plot(freqs, magnitude, linewidth=1)
    ax_freq.plot(peak_freq, peak_mag, "ro", label=f"Peak: {peak_freq:.1f} Hz")
    ax_freq.set_title("Frequency Domain - FFT (DC component removed)")
    ax_freq.set_xlabel("Frequency (Hz)")
    ax_freq.set_ylabel("Amplitude (V)")
    ax_freq.set_xlim(0, SAMPLING_RATE_HZ / 2) 
    ax_freq.legend(loc="upper right")
    ax_freq.grid(True, alpha=0.3)

    freq_resolution = SAMPLING_RATE_HZ / NUM_SAMPLES
    fig.suptitle(f"FFT bin resolution: {freq_resolution:.2f} Hz "
                 f"(increase NUM_SAMPLES for finer resolution at low test frequencies)")

    plt.tight_layout()

    # ---- Auto-save graph ----
    graph_filename = f"graph_{TEST_FREQUENCY_LABEL}.png"
    plt.savefig(graph_filename, dpi=150)
    print(f"Saved graph: {graph_filename}")

    plt.show()


if __name__ == "__main__":
    main()