# Command Line Reference Guide
**Project:** Voice Controlled LED with Seeed Studio XIAO nRF52840 Sense

This document contains all the necessary Command Line (CMD/PowerShell) instructions used throughout the lifecycle of this project, from data collection to GitHub version control.

---

## 1. Edge Impulse Data Collection
These commands are used to connect the board to Edge Impulse Studio for audio sampling and model training.

* **Connect the board directly** (requires Edge Impulse firmware):
  ```cmd
  edge-impulse-daemon
  ```
* **Clear cache and connect a new project/board:**
  ```cmd
  edge-impulse-daemon --clean
  ```
* **Forward raw sensor data over serial** (used with custom `data_forwarder_audio` code):
  ```cmd
  edge-impulse-data-forwarder
  ```

---

## 2. Arduino CLI Workflow
These commands handle the compilation and deployment of the C++ code to the microcontroller.

* **Check connected boards and find the COM port:**
  ```cmd
  arduino-cli board list
  ```
* **Install the exported Edge Impulse library (Zip file):**
  *(Replace the path with the actual path to your downloaded zip file)*
  ```cmd
  arduino-cli lib install --zip-path "C:\Downloads\ei-project-name-arduino.zip"
  ```
* **Compile the inference code:**
  *(Run this from your root project directory)*
  ```cmd
  arduino-cli compile --fqbn Seeeduino:mbed:xiaonRF52840Sense voice_led_inference
  ```
* **Upload the code to the board:**
  *(Put the board in bootloader mode first. Replace COM13 with your actual port)*
  ```cmd
  arduino-cli upload -p COM13 --fqbn Seeeduino:mbed:xiaonRF52840Sense voice_led_inference
  ```
* **Open the Serial Monitor to view results:**
  ```cmd
  arduino-cli monitor -p COM13 --config baudrate=115200
  ```

---

## 3. Initial GitHub Setup (Pushing the Project for the First Time)
Use these commands when initializing the repository and uploading your project directory to GitHub for the very first time.

* **Navigate to your project folder:**
  ```cmd
  cd C:\Speeed_Studio\sensor_test
  ```
* **Initialize Git:**
  ```cmd
  git init
  ```
* **Configure Git identity (Only needed once per PC):**
  ```cmd
  git config --global user.email "your_email@example.com"
  git config --global user.name "Your Name"
  ```
* **Add all files and make the initial commit:**
  ```cmd
  git add .
  git commit -m "Initial commit: Voice LED project setup"
  ```
* **Set the main branch and connect to your GitHub repository:**
  *(Replace the URL with your actual repository link)*
  ```cmd
  git branch -M main
  git remote add origin https://github.com/Kavi-Cs/Data-Acquisition-Lab.git
  ```
* **Push the files to GitHub:**
  *(If the repo already has a default README, use `git push -f origin main` to overwrite it)*
  ```cmd
  git push -u origin main
  ```

---

## 4. Version Control (Updating GitHub)
Whenever you make changes to your code, update the README, or add new Edge Impulse zip models, use these three simple commands to update your GitHub repository.

* **Stage all modified/new files:**
  ```cmd
  git add .
  ```
* **Commit the changes with a descriptive message:**
  ```cmd
  git commit -m "Updated inference code and modified debounce logic"
  ```
* **Push the updates to GitHub:**
  ```cmd
  git push
  ```