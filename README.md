# M5StopWatch MQTT Counter

MQTT synchronized capacity counter firmware for the M5Stack StopWatch.

## Overview

M5StopWatch MQTT Counter transforms the M5Stack StopWatch into a battery-powered MQTT counter device that can synchronize with other counters, web dashboards, and display devices through a central MQTT broker.

The project is based on the original M5Stack StopWatch User Demo firmware and has been modified to support MQTT-based counter synchronization using Node-RED as the authoritative source of truth.

## Features

- MQTT synchronized counter
- Multiple StopWatch devices stay in sync
- Increment counter
- Decrement counter
- Reset counter
- Real-time updates
- Touchscreen interface
- Battery powered operation
- Node-RED integration
- Web dashboard integration
- Compatible with additional MQTT display clients

## System Architecture

text                     ┌───────────────┐                     │   Node-RED    │                     │ Authoritative │                     │ Counter State │                     └───────┬───────┘                             │                       MQTT Broker                             │          ┌──────────────────┼──────────────────┐          │                  │                  │          ▼                  ▼                  ▼   StopWatch #1      StopWatch #2       Web Dashboard          │          ▼    Additional MQTT Clients 

Node-RED maintains the authoritative counter value.

All clients subscribe to the counter state topic and update their displays whenever a new value is published.

## Hardware

### Target Device

- M5Stack StopWatch
- ESP32-S3
- Round touchscreen display
- Integrated battery
- IMU motion sensor

## Button Mapping

| Button | Function |
|----------|----------|
| KEYB (Blue) | Increment |
| KEYA (Yellow) | Decrement |
| KEYA + KEYB | Reset |

## MQTT Topics

### Counter State

Topic:

text counters/capacity/state 

Payload Example:

text 30 

### Increment Command

Topic:

text counters/capacity/increment 

Payload:

text 1 

### Decrement Command

Topic:

text counters/capacity/decrement 

Payload:

text 1 

### Reset Command

Topic:

text counters/capacity/reset 

Payload:

text 1 

## Software Components

### MQTT Broker

Tested with:

- Mosquitto MQTT

### Automation Platform

- Node-RED

Node-RED maintains the authoritative counter value and republishes state changes to all connected devices.

### Additional Clients

Examples:

- Web Dashboard (counter.html)
- M5StickS3 MQTT Counter
- GeekMagic SmallTV Pro MQTT Display
- Future mobile applications

## Development Environment

This project is developed using Espressif ESP-IDF.

### Requirements

- ESP-IDF v5.x
- Python 3.x
- Git
- USB-C connection to the StopWatch

## Clone Repository

bash git clone https://github.com/slochewie/M5StopWatch-MQTT-Counter.git cd M5StopWatch-MQTT-Counter 

## Configure Target

bash idf.py set-target esp32s3 

Optional:

bash idf.py menuconfig 

## Build

bash idf.py build 

## Flash

bash idf.py flash 

## Monitor

bash idf.py monitor 

Flash and monitor:

bash idf.py flash monitor 

## Recommended Repository Structure

text M5StopWatch-MQTT-Counter/ ├── main/ ├── components/ ├── docs/ │   ├── architecture.md │   └── screenshots/ ├── node-red/ │   └── flows.json ├── web/ │   └── counter.html ├── CMakeLists.txt ├── sdkconfig.defaults └── README.md 

## Planned Power Management

Planned functionality:

- Screen timeout after 30 seconds inactivity
- Wake on motion
- Wake on touch
- Wake on button press
- Extended battery runtime optimization

## Related Projects

### Original Firmware

M5Stack StopWatch User Demo firmware.

### Companion Projects

- M5StickS3 MQTT Counter
- Node-RED Capacity Counter
- Counter Web Dashboard
- GeekMagic SmallTV Pro MQTT Display

## Screenshots

Add screenshots of:

- Main counter screen
- Increment action
- Decrement action
- Reset action
- Sleep mode
- Multi-device synchronization

## Status

Active development.

Current focus:

- MQTT synchronization
- Counter application
- Multi-device support
- Power optimization
- User interface refinement

## License

See LICENSE file for details.
