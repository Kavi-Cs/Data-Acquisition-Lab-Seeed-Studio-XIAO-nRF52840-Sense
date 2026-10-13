# Data Acquisition Lab - Voice Controlled LED With Seeed Studio XIAO nRF52840 Sense

**Author:** Kaveesha Induwara (Kavi-Cs)

## Project Overview
This repository contains the source code for a voice-controlled LED project using Machine Learning (Keyword Spotting). 
It was developed for a Data Acquisition lab experiment to filter out environmental noise and trigger actions based on specific voice commands. The AI model was trained using Edge Impulse Studio.

## Hardware & Software
- **Board:** Seeed Studio XIAO nRF52840 Sense
- **Microphone:** Onboard PDM Microphone
- **Framework:** Arduino CLI / C++
- **Machine Learning:** Edge Impulse SDK

## Directory Structure
- `data_forwarder_audio/` - Contains code for raw audio data acquisition.
- `voice_led_inference/` - Contains the main C++ inference code with advanced debounce and cooldown logic for LED control.