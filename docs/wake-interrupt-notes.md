# StopWatch Wake Interrupt Notes

## Source documents

- `C152-SCH_Stopwatch_PRJ_Main_VA_20251201_2026_04_24_17_46_22.pdf`
- `BMI270.PDF`

## Confirmed schematic findings

The StopWatch schematic shows the BMI270 IMU has interrupt-capable wiring:

- BMI270 `INT1` is connected to the `IMU_INT` net.
- `IMU_INT` passes through Q7, a `2N7002KT` MOSFET stage.
- The output net is `PMG0_RTC&IMU_INT`.
- The RTC interrupt also shares this wake net.
- `PMG0_RTC&IMU_INT` connects to M5PM1 pin `G0_WAKEin(INT0/2)_IRQout_NEOPIXEL`.
- The overview marks `BMI270;Wake-up Support`.
- The overview marks `PMG0_RTC&IMU_INT` as the shared RTC/IMU wake path.

This means the hardware supports an IMU-originated wake path through M5PM1 G0, not a direct ESP32-S3 GPIO wake pin.

## Important correction

Earlier notes assumed the shared RTC/IMU wake net might land directly on an ESP32-S3 GPIO. The schematic does not show that. It shows the net going into M5PM1 `G0_WAKEin(INT0/2)_IRQout_NEOPIXEL`.

So true Phase 3 should be implemented as an M5PM1 wake-source flow, not by guessing an ESP32-S3 GPIO number.

## Current firmware state

The current HAL initializes BMI270/BMM150 in accelerometer-only mode and reads acceleration by polling over I2C.

At the time of this note, the firmware does not yet configure:

- BMI270 any-motion feature
- BMI270 interrupt output mapping
- BMI270 INT electrical behavior
- clearing BMI270 latched interrupt status
- M5PM1 G0 wake-source behavior for `PMG0_RTC&IMU_INT`

## BMI270 datasheet findings

The BMI270 datasheet defines any-motion configuration registers/features, including:

- duration expressed in 50 Hz samples
- threshold based on acceleration slope
- output mapping to interrupt/status bits
- any-motion output mapping to `INT1_MAP_FEAT` / `INT2_MAP_FEAT`
- interrupt pin electrical behavior through `INT1_IO_CTRL`
- latch behavior through `INT_LATCH`

## Safe implementation path

Recommended next steps:

1. Keep the current Phase 2 timer-based light sleep implementation as the fallback.
2. Add direct GPIO wake for KEY1/KEY2 only, because those are confirmed as ESP32-S3 GPIO1/GPIO2.
3. Add BMI270 any-motion interrupt configuration and route it to INT1.
4. Configure BMI270 INT1 electrical behavior to match the Q7/M5PM1 G0 path.
5. Add M5PM1 G0 wake-source handling once the M5PM1 library/API path is confirmed.
6. Only then remove or relax the 100 ms IMU polling loop.

## Important caution

Do not remove timer polling until both of these are proven on hardware:

1. BMI270 INT1 asserts the shared `PMG0_RTC&IMU_INT` path when the lanyard-to-handheld motion occurs.
2. M5PM1 G0 wakes or notifies the ESP32-S3 as expected.

The schematic confirms the hardware wake path exists, but the firmware still needs both BMI270 interrupt setup and M5PM1 wake-source handling.
