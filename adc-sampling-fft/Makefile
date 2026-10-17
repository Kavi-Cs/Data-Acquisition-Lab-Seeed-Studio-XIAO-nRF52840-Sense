# Makefile - convenience wrapper around PlatformIO (firmware) and the
# Python venv + scripts (host-side automation). See CMakeLists.txt's
# header comment for why the firmware build itself is delegated to
# PlatformIO rather than reimplemented here or in CMake.

PIO_ENV   ?= xiao_nrf52840_sense
PORT      ?= /dev/ttyACM0
BAUD      ?= 115200
PYTHON    ?= python3
VENV      ?= .venv
VENV_PY   := $(VENV)/bin/python

FSTART ?= 10
FSTOP  ?= 1000
FSTEP  ?= 10

.PHONY: help build upload monitor clean venv sweep plot grid split distclean

help:
	@echo "Firmware (PlatformIO, env=$(PIO_ENV)):"
	@echo "  make build              compile firmware"
	@echo "  make upload PORT=...    flash firmware over USB (default $(PORT))"
	@echo "  make monitor PORT=...   open serial monitor at $(BAUD) baud"
	@echo "  make clean              remove PlatformIO build artifacts"
	@echo ""
	@echo "Host-side automation (Python venv in $(VENV)/):"
	@echo "  make venv               create venv + install scripts/requirements.txt"
	@echo "  make sweep PORT=...     run the automated frequency sweep"
	@echo "                          (FSTART=$(FSTART) FSTOP=$(FSTOP) FSTEP=$(FSTEP))"
	@echo "  make plot               live-watch the newest CSV in data/"
	@echo "  make grid               render the all-frequencies overview grid PNG"
	@echo "  make split              render one PNG per frequency"
	@echo ""
	@echo "  make distclean          remove build artifacts, venv, logs and data"

build:
	pio run -e $(PIO_ENV)

upload:
	pio run -e $(PIO_ENV) -t upload --upload-port $(PORT)

monitor:
	pio device monitor -p $(PORT) -b $(BAUD)

clean:
	pio run -e $(PIO_ENV) -t clean

venv:
	$(PYTHON) -m venv $(VENV)
	$(VENV_PY) -m pip install --upgrade pip
	$(VENV_PY) -m pip install -r scripts/requirements.txt
	@echo "Virtualenv ready: $(VENV)"

sweep: venv
	$(VENV_PY) scripts/sweep_log.py --port $(PORT) --baud $(BAUD) \
		--fstart $(FSTART) --fstop $(FSTOP) --fstep $(FSTEP)

plot: venv
	$(VENV_PY) scripts/sweep_plot.py

grid: venv
	$(VENV_PY) scripts/sweep_plot.py --grid

split: venv
	$(VENV_PY) scripts/sweep_plot.py --split

distclean: clean
	rm -rf $(VENV) data logs .pio
