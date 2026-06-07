# M5StopWatch MQTT Counter Architecture

## Overview

The M5StopWatch MQTT Counter uses Node-RED as the authoritative source of counter state.

## Components

- M5Stack StopWatch devices
- Mosquitto MQTT broker
- Node-RED flow
- Web dashboard (counter.html)
- Additional MQTT display clients

## Architecture

```text
                    ┌───────────────┐
                    │   Node-RED    │
                    │ Authoritative │
                    │ Counter State │
                    └───────┬───────┘
                            │
                      MQTT Broker
                            │
         ┌──────────────────┼──────────────────┐
         │                  │                  │
         ▼                  ▼                  ▼
  StopWatch #1      StopWatch #2       Web Dashboard
         │
         ▼
   Additional MQTT Clients
```

## MQTT Topics

- counters/capacity/state
- counters/capacity/increment
- counters/capacity/decrement
- counters/capacity/reset
