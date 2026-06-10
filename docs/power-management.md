# Power Management

This is the current known-good StopWatch counter power-management baseline.

Confirmed on hardware:

- Counter app idles after 30 seconds of inactivity.
- Display brightness is set to zero during idle.
- ESP32-S3 light sleep is used in short cycles.
- M5PM1 rails are kept powered.
- MQTT and battery publishing remain active.
- Device resumes from KEYA, KEYB, touch, and BMI270 motion.
- Lanyard-to-handheld motion works as expected.

## Hardware path

The working motion path is:

BMI270 any-motion -> BMI270 INT1 -> IMU_INT -> Q7 -> PMG0_RTC_IMU_INT -> M5PM1 GPIO0.

## Orientation fallback

The fallback orientation check is still present:

- Y less than 0.30
- Z greater than 0.35
- 3 consecutive samples
- 100 ms sample interval

Reference positions:

- Hanging from lanyard: Y about +1.0, Z about 0.0, X about 0.0
- Handheld: Y about -0.4, Z about +0.9, X about 0.0

## M5PM1 setup

M5PM1 GPIO0 is configured as the shared RTC and IMU wake input:

- PMG0_RTC_IMU_INT equals M5PM1_GPIO_NUM_0
- input mode
- pull-up enabled
- falling edge
- wake enabled
- wake function selected
- external wake flag cleared at boot

## BMI270 setup

BMI270 is configured for any-motion interrupt output on INT1:

- direct BMI270 I2C handle created after sensor init
- INT1 configured active-low
- non-latched interrupt for this baseline
- any-motion enabled on all axes
- duration filter: 3 samples
- threshold: about 250 mg
- any-motion mapped to INT1

## Keep this baseline

The 100 ms timer fallback is intentionally still present. Keep this as the rollback point before removing polling or changing sleep depth.
