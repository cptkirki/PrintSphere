#include "printsphere/pmu.hpp"

#define XPOWERS_CHIP_AXP2101
#include "XPowersLib.h"

#include "bsp/esp32_s3_touch_amoled_1_75.h"

#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"

namespace printsphere {

namespace {

constexpr char kTag[] = "printsphere.pmu";
constexpr uint32_t kI2cTimeoutMs = 1000;

XPowersPMU s_pmu;
i2c_master_dev_handle_t s_pmu_device = nullptr;

int pmu_register_read(uint8_t, uint8_t reg_addr, uint8_t* data, uint8_t len) {
  if (s_pmu_device == nullptr) {
    return -1;
  }

  const esp_err_t err =
      i2c_master_transmit_receive(s_pmu_device, &reg_addr, 1, data, len, kI2cTimeoutMs);
  return err == ESP_OK ? 0 : -1;
}

int pmu_register_write_byte(uint8_t, uint8_t reg_addr, uint8_t* data, uint8_t len) {
  if (s_pmu_device == nullptr) {
    return -1;
  }

  uint8_t buffer[17] = {};
  if (len + 1 > sizeof(buffer)) {
    return -1;
  }

  buffer[0] = reg_addr;
  for (uint8_t index = 0; index < len; ++index) {
    buffer[index + 1] = data[index];
  }

  const esp_err_t err = i2c_master_transmit(s_pmu_device, buffer, len + 1, kI2cTimeoutMs);
  return err == ESP_OK ? 0 : -1;
}

}  // namespace

esp_err_t PmuManager::initialize() {
  if (initialized_) {
    return ESP_OK;
  }

  ESP_RETURN_ON_ERROR(bsp_i2c_init(), kTag, "bsp_i2c_init failed");

  i2c_master_bus_handle_t bus_handle = bsp_i2c_get_handle();
  if (bus_handle == nullptr) {
    return ESP_FAIL;
  }

  if (s_pmu_device == nullptr) {
    i2c_device_config_t device_config = {};
    device_config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    device_config.device_address = AXP2101_SLAVE_ADDRESS;
    device_config.scl_speed_hz = 400000;
    device_config.scl_wait_us = 0;
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus_handle, &device_config, &s_pmu_device), kTag,
                        "i2c_master_bus_add_device failed");
  }

  if (!s_pmu.begin(AXP2101_SLAVE_ADDRESS, pmu_register_read, pmu_register_write_byte)) {
    ESP_LOGE(kTag, "PMU begin failed");
    return ESP_FAIL;
  }

  s_pmu.enableVbusVoltageMeasure();
  s_pmu.enableBattVoltageMeasure();
  s_pmu.enableSystemVoltageMeasure();
  s_pmu.enableTemperatureMeasure();
  s_pmu.disableTSPinMeasure();

  s_pmu.disableIRQ(XPOWERS_AXP2101_ALL_IRQ);
  s_pmu.clearIrqStatus();
  s_pmu.setPrechargeCurr(XPOWERS_AXP2101_PRECHARGE_50MA);
  s_pmu.setChargerConstantCurr(XPOWERS_AXP2101_CHG_CUR_400MA);
  s_pmu.setChargerTerminationCurr(XPOWERS_AXP2101_CHG_ITERM_25MA);
  s_pmu.setChargeTargetVoltage(XPOWERS_AXP2101_CHG_VOL_4V2);

  initialized_ = true;
  ESP_LOGI(kTag, "PMU ready");
  return ESP_OK;
}

PowerSnapshot PmuManager::sample() const {
  PowerSnapshot snapshot;
  if (!initialized_) {
    return snapshot;
  }

  snapshot.available = true;
  snapshot.battery_present = s_pmu.isBatteryConnect();
  snapshot.usb_present = s_pmu.isVbusIn() || s_pmu.isVbusGood();
  snapshot.charging = s_pmu.isCharging();
  snapshot.temperature_c = s_pmu.getTemperature();

  if (snapshot.battery_present) {
    snapshot.battery_percent = static_cast<uint8_t>(s_pmu.getBatteryPercent());
  }

  return snapshot;
}

}  // namespace printsphere
