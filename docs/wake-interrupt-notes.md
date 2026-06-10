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
- The overview marks `BMI270;Wake-up Support`.
- The overview marks `PMG0_RTC&IMU_INT` as the shared RTC/IMU wake path.

This means the hardware supports an IMU-originated wake path, but the firmware still needs BMI270 interrupt configuration before the IMU can assert this line.

## Current firmware state

The current HAL initializes BMI270/BMM150 in accelerometer-only mode and reads acceleration by polling over I2C.

At the time of this note, the firmware does not yet configure:

- BMI270 any-motion feature
- BMI270 interrupt output mapping
- BMI270 INT electrical behavior
- clearing BMI270 latched interrupt status

## BMI270 datasheet findings

The BMI270 datasheet defines any-motion configuration registers/features, including:

- duration expressed in 50 Hz samples
- threshold range from 0 to 1 g
- output mapping to interrupt/status bits
- any-motion output mapping to `INT1_MAP_FEAT` / `INT2_MAP_FEAT`

## Safe implementation path

Recommended next steps:

1. Keep the current Phase 2 timer-based light sleep implementation as the fallback.
2. Add ESP32-S3 light-sleep GPIO wake on the shared wake net once the ESP32 GPIO mapping is confirmed.
3. Add BMI270 any-motion interrupt configuration.
4. Confirm whether the shared `PMG0_RTC&IMU_INT` net is active-low after the Q7 MOSFET stage.
5. Keep M5PM1 rails powered during this phase so `3V3_L1`, BMI270, RTC, and the wake path remain alive.

## Important caution

Do not remove timer polling until the BMI270 interrupt configuration is proven on hardware. The shared RTC/IMU wake net exists, but the IMU will not wake the ESP32 unless BMI270 firmware configuration actively drives its interrupt output.
