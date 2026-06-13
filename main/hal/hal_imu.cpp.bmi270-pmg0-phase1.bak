/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "hal.h"
#include <mooncake_log.h>
#include <bmi270_bmm150.h>

#ifndef BMI2_I2C_PRIM_ADDR
#define BMI2_I2C_PRIM_ADDR 0x68
#endif

static const std::string_view _tag        = "HAL-IMU";
static bmi270_bmm150_handle_t _imu_sensor = NULL;
static i2c_bus_device_handle_t _imu_i2c_dev = NULL;

namespace {

constexpr uint8_t BMI270_REG_FEAT_PAGE     = 0x2F;
constexpr uint8_t BMI270_REG_FEATURES_12   = 0x3C;
constexpr uint8_t BMI270_REG_FEATURES_14   = 0x3E;
constexpr uint8_t BMI270_REG_INT1_IO_CTRL  = 0x53;
constexpr uint8_t BMI270_REG_INT_LATCH     = 0x55;
constexpr uint8_t BMI270_REG_INT1_MAP_FEAT = 0x56;
constexpr uint8_t BMI270_REG_INT_STATUS_0  = 0x1C;

constexpr uint8_t BMI270_FEAT_PAGE_ANYMO = 1;
constexpr uint8_t BMI270_INT1_ACTIVE_LOW_PUSHPULL_OUTPUT = 0x08;
constexpr uint8_t BMI270_INT1_MAP_ANY_MOTION = 1 << 6;

// ANYMO_1: duration is bits 12..0, select_x/y/z are bits 13/14/15.
// 3 samples at 50 Hz = about 60 ms. All axes are enabled because the device
// can be grabbed from several lanyard angles.
constexpr uint16_t BMI270_ANYMO_1_ALL_AXES_DURATION_3 = 0xE000 | 3;

// ANYMO_2: threshold is bits 10..0, out_conf is bits 14..11, enable is bit 15.
// 0x0200 is about 250 mg using the BMI270 feature threshold scale where 0x0800
// represents roughly 1 g. out_conf=7 keeps the feature output assigned to an
// interrupt/status bit and bit 15 enables the any-motion feature.
constexpr uint16_t BMI270_ANYMO_2_ENABLE_OUTCONF7_THRESHOLD_250MG = 0x8000 | (0x07 << 11) | 0x0200;

esp_err_t bmi270_write_u8(uint8_t reg, uint8_t value)
{
    if (_imu_i2c_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return i2c_bus_write_byte(_imu_i2c_dev, reg, value);
}

esp_err_t bmi270_write_u16(uint8_t reg, uint16_t value)
{
    const uint8_t data[2] = {
        static_cast<uint8_t>(value & 0xFF),
        static_cast<uint8_t>((value >> 8) & 0xFF),
    };
    if (_imu_i2c_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return i2c_bus_write_bytes(_imu_i2c_dev, reg, sizeof(data), data);
}

void log_imu_config_result(const char* action, esp_err_t result)
{
    if (result == ESP_OK) {
        mclog::tagInfo(_tag, "{} ok", action);
    } else {
        mclog::tagError(_tag, "{} failed: {}", action, static_cast<int>(result));
    }
}

void configure_bmi270_any_motion_int1()
{
    if (_imu_i2c_dev == NULL) {
        mclog::tagError(_tag, "any-motion config skipped: no i2c device");
        return;
    }

    // Configure BMI270 INT1 as active-low output so the shared IMU/RTC wake path
    // can create the falling edge expected by M5PM1 GPIO0.
    log_imu_config_result("BMI270 INT1 electrical config",
                          bmi270_write_u8(BMI270_REG_INT1_IO_CTRL, BMI270_INT1_ACTIVE_LOW_PUSHPULL_OUTPUT));

    // Keep any-motion non-latched for this first hardware test. The existing
    // Phase 2 orientation polling remains the safe fallback if this is too brief.
    log_imu_config_result("BMI270 interrupt latch config", bmi270_write_u8(BMI270_REG_INT_LATCH, 0x00));

    // Select feature page 1, then write ANYMO_1 and ANYMO_2 as 16-bit words.
    log_imu_config_result("BMI270 feature page any-motion", bmi270_write_u8(BMI270_REG_FEAT_PAGE, BMI270_FEAT_PAGE_ANYMO));
    log_imu_config_result("BMI270 ANYMO_1 config",
                          bmi270_write_u16(BMI270_REG_FEATURES_12, BMI270_ANYMO_1_ALL_AXES_DURATION_3));
    log_imu_config_result("BMI270 ANYMO_2 config",
                          bmi270_write_u16(BMI270_REG_FEATURES_14, BMI270_ANYMO_2_ENABLE_OUTCONF7_THRESHOLD_250MG));

    // Route the any-motion feature output to INT1.
    log_imu_config_result("BMI270 INT1 any-motion map",
                          bmi270_write_u8(BMI270_REG_INT1_MAP_FEAT, BMI270_INT1_MAP_ANY_MOTION));

    // Clear/read pending feature interrupt state after configuration.
    uint8_t status = 0;
    if (i2c_bus_read_byte(_imu_i2c_dev, BMI270_REG_INT_STATUS_0, &status) == ESP_OK) {
        mclog::tagInfo(_tag, "BMI270 INT_STATUS_0 after config: 0x{:02X}", status);
    }
}

}  // namespace

void Hal::imu_init()
{
    mclog::tagInfo(_tag, "init");

    bmi270_bmm150_config_t sensor_conf = {
        .i2c_addr        = BMI2_I2C_PRIM_ADDR,
        .config_file_ptr = NULL,
        // .mode            = BOSCH_ACCEL_AND_MAGN,
        .mode = BOSCH_ACCELEROMETER_ONLY,
    };

    if (bmi270_bmm150_sensor_create(_i2c_bus, &_imu_sensor, &sensor_conf) != ESP_OK) {
        mclog::tagError(_tag, "init failed");
        _imu_sensor = NULL;
    }

    if (_imu_i2c_dev == NULL) {
        _imu_i2c_dev = i2c_bus_device_create(_i2c_bus, BMI2_I2C_PRIM_ADDR, 0);
        if (_imu_i2c_dev == NULL) {
            mclog::tagError(_tag, "failed to create bmi270 direct i2c handle");
        }
    }

    configure_bmi270_any_motion_int1();
}

bool Hal::imuGetInterruptStatus0(uint8_t& status)
{
    if (_imu_i2c_dev == NULL) {
        return false;
    }

    uint8_t value = 0;
    if (i2c_bus_read_byte(_imu_i2c_dev, BMI270_REG_INT_STATUS_0, &value) != ESP_OK) {
        return false;
    }

    status = value;
    return true;
}

void Hal::updateImuData()
{
    if (_imu_sensor != NULL) {
        int available = 0;
        if (bmi270_bmm150_sensor_acceleration_available(_imu_sensor, &available) == ESP_OK && available > 0) {
            bmi270_bmm150_sensor_read_acceleration(_imu_sensor, &_imu_data.accelY, &_imu_data.accelX,
                                                   &_imu_data.accelZ);
        }
        if (bmi270_bmm150_sensor_gyroscope_available(_imu_sensor, &available) == ESP_OK && available > 0) {
            bmi270_bmm150_sensor_read_gyroscope(_imu_sensor, &_imu_data.gyroY, &_imu_data.gyroX, &_imu_data.gyroZ);
        }
    } else {
        mclog::tagError(_tag, "imu invalid");
    }
}
