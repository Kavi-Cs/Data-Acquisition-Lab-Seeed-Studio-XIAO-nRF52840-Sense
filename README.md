# Data Acquisition Lab - Voice Controlled LED With# Data Acquisition Lab - Voice Controlled LED 🎙️💡

**Author:** Kaveesha Induwara (Kavi-Cs)

## 📌 Project Overview
This repository contains the source code for a highly responsive voice-controlled LED project using Machine Learning (Keyword Spotting). Developed for a Data Acquisition lab experiment, the system leverages an onboard microphone to filter out environmental noise and trigger hardware actions based on specific voice commands ("on" and "off").

The underlying AI model was trained using **Edge Impulse Studio** to classify audio streams in real-time, utilizing continuous inference on an edge device.

## 🛠️ Hardware & Software
* **Microcontroller:** Seeed Studio XIAO nRF52840 Sense
* **Sensor:** Onboard PDM Microphone
* **Output:** External LED connected to Pin **D6**
* **Framework:** Arduino CLI / C++
* **Machine Learning:** Edge Impulse SDK (C++ Deployment)

## 🧠 Advanced Features (Inference Logic)
Unlike basic keyword spotting scripts, this project implements robust state-machine logic to handle real-world acoustic challenges:
* **Asymmetrical Debouncing:** Applies dynamic frame-confirmation thresholds. The short, sharp "off" command requires only 1 frame to trigger instantly, while the "on" command requires 4 consecutive frames to completely eliminate false positives from ambient room noise.
* **Continuous State Reset:** Explicitly flushes the static sliding-window feature matrix (`run_classifier_init()`) immediately after a cooldown expires. This prevents stale audio history from causing delayed ghost triggers.
* **Non-blocking Cooldown:** A 1.8-second lockout period post-trigger prevents echo or breath tails from re-triggering the system, while keeping the audio buffer actively draining.

## 📂 Directory Structure
* `data_forwarder_audio/` - Contains the firmware for raw PCM audio data acquisition and UART forwarding.
* `voice_led_inference/` - Contains the main C++ inference application, Edge Impulse SDK integration, and LED control logic.

## 🚀 Getting Started (Arduino CLI)

### 1. Compilation
Navigate to the root directory and compile the inference code for the mbed-enabled XIAO nRF52840 Sense:
```bash
arduino-cli compile --fqbn Seeeduino:mbed:xiaonRF52840Sense voice_led_inference 🎙️💡

**Author:** Kaveesha Induwara (Kavi-Cs)

## 📌 Project Overview
This repository contains the source code for a highly responsive voice-controlled LED project using Machine Learning (Keyword Spotting). Developed for a Data Acquisition lab experiment, the system leverages an onboard microphone to filter out environmental noise and trigger hardware actions based on specific voice commands ("on" and "off").

The underlying AI model was trained using **Edge Impulse Studio** to classify audio streams in real-time, utilizing continuous inference on an edge device.

## 🛠️ Hardware & Software
* **Microcontroller:** Seeed Studio XIAO nRF52840 Sense
* **Sensor:** Onboard PDM Microphone
* **Output:** External LED connected to Pin **D6**
* **Framework:** Arduino CLI / C++
* **Machine Learning:** Edge Impulse SDK (C++ Deployment)

## 🧠 Advanced Features (Inference Logic)
Unlike basic keyword spotting scripts, this project implements robust state-machine logic to handle real-world acoustic challenges:
* **Asymmetrical Debouncing:** Applies dynamic frame-confirmation thresholds. The short, sharp "off" command requires only 1 frame to trigger instantly, while the "on" command requires 4 consecutive frames to completely eliminate false positives from ambient room noise.
* **Continuous State Reset:** Explicitly flushes the static sliding-window feature matrix (`run_classifier_init()`) immediately after a cooldown expires. This prevents stale audio history from causing delayed ghost triggers.
* **Non-blocking Cooldown:** A 1.8-second lockout period post-trigger prevents echo or breath tails from re-triggering the system, while keeping the audio buffer actively draining.

## 📂 Directory Structure
* `data_forwarder_audio/` - Contains the firmware for raw PCM audio data acquisition and UART forwarding.
* `voice_led_inference/` - Contains the main C++ inference application, Edge Impulse SDK integration, and LED control logic.

## 🚀 Getting Started (Arduino CLI)

### 1. Compilation
Navigate to the root directory and compile the inference code for the mbed-enabled XIAO nRF52840 Sense:
```bash
arduino-cli compile --fqbn Seeeduino:mbed:xiaonRF52840Sense voice_led_inference
