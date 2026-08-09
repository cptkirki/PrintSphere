#include "printsphere/serial_provisioner.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <utility>

#include "driver/uart.h"
#include "driver/usb_serial_jtag.h"
#include "esp_log.h"
#include "freertos/task.h"
#include "printsphere/config_store.hpp"
#include "printsphere/wifi_manager.hpp"
#include "sdkconfig.h"
#include "soc/soc_caps.h"

namespace printsphere {

namespace {

constexpr char kTag[] = "printsphere.improv";
constexpr char kHeader[] = "IMPROV";
constexpr uint8_t kProtocolVersion = 1;
constexpr uint8_t kTypeCurrentState = 0x01;
constexpr uint8_t kTypeErrorState = 0x02;
constexpr uint8_t kTypeRpc = 0x03;
constexpr uint8_t kTypeRpcResult = 0x04;
constexpr uint8_t kStateReady = 0x02;
constexpr uint8_t kStateProvisioning = 0x03;
constexpr uint8_t kStateProvisioned = 0x04;
constexpr uint8_t kErrorNone = 0x00;
constexpr uint8_t kErrorInvalidRpc = 0x01;
constexpr uint8_t kErrorUnknownRpc = 0x02;
constexpr uint8_t kErrorUnableToConnect = 0x03;
constexpr uint8_t kCommandWifiSettings = 0x01;
constexpr uint8_t kCommandCurrentState = 0x02;
constexpr uint8_t kCommandDeviceInfo = 0x03;
constexpr uint8_t kCommandWifiNetworks = 0x04;
constexpr TickType_t kConnectTimeout = pdMS_TO_TICKS(45000);
constexpr uint32_t kTaskStackBytes = 6144;

const char* hardware_variant() {
#if defined(PRINTSPHERE_HW_VARIANT_AMOLED_1_75)
  return "ESP32-S3 / AMOLED 1.75";
#elif defined(PRINTSPHERE_HW_VARIANT_LCD_2_8C)
  return "ESP32-S3 / LCD 2.8C";
#elif defined(PRINTSPHERE_HW_VARIANT_KNOMI_V2)
  return "ESP32-S3 / Knomi v2";
#else
  return "ESP32-S3";
#endif
}

uint8_t checksum(const uint8_t* data, size_t length) {
  uint8_t result = 0;
  for (size_t index = 0; index < length; ++index) {
    result = static_cast<uint8_t>(result + data[index]);
  }
  return result;
}

bool elapsed(TickType_t started_at, TickType_t duration) {
  return static_cast<int32_t>(xTaskGetTickCount() - started_at) >=
         static_cast<int32_t>(duration);
}

}  // namespace

SerialProvisioner::SerialProvisioner(ConfigStore& config_store, WifiManager& wifi_manager)
    : config_store_(config_store), wifi_manager_(wifi_manager) {}

esp_err_t SerialProvisioner::start() {
  const esp_err_t uart_err = uart_driver_install(UART_NUM_0, 1024, 1024, 0, nullptr, 0);
  if (uart_err == ESP_OK || uart_err == ESP_ERR_INVALID_STATE) {
    uart_available_ = true;
  } else {
    ESP_LOGW(kTag, "UART Improv transport unavailable: %s", esp_err_to_name(uart_err));
  }

#if SOC_USB_SERIAL_JTAG_SUPPORTED && CONFIG_USJ_ENABLE_USB_SERIAL_JTAG
  if (usb_serial_jtag_is_driver_installed()) {
    usb_available_ = true;
  } else {
    usb_serial_jtag_driver_config_t config = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    const esp_err_t usb_err = usb_serial_jtag_driver_install(&config);
    if (usb_err == ESP_OK) {
      usb_available_ = true;
    } else {
      ESP_LOGW(kTag, "USB Serial/JTAG Improv transport unavailable: %s", esp_err_to_name(usb_err));
    }
  }
#endif

  if (!uart_available_ && !usb_available_) {
    return ESP_FAIL;
  }

  const BaseType_t created =
      xTaskCreate(task_entry, "printsphere_improv", kTaskStackBytes / sizeof(StackType_t), this, 4, nullptr);
  if (created != pdPASS) {
    return ESP_ERR_NO_MEM;
  }
  ESP_LOGI(kTag, "Improv Serial ready (UART=%d USB=%d)", uart_available_ ? 1 : 0,
           usb_available_ ? 1 : 0);
  return ESP_OK;
}

void SerialProvisioner::task_entry(void* context) {
  static_cast<SerialProvisioner*>(context)->task();
}

void SerialProvisioner::task() {
  while (true) {
    uint8_t byte = 0;
    if (uart_available_ && uart_read_bytes(UART_NUM_0, &byte, 1, pdMS_TO_TICKS(10)) == 1) {
      process_byte(uart_parser_, Transport::kUart, byte);
    }
#if SOC_USB_SERIAL_JTAG_SUPPORTED && CONFIG_USJ_ENABLE_USB_SERIAL_JTAG
    if (usb_available_ && usb_serial_jtag_read_bytes(&byte, 1, 0) == 1) {
      process_byte(usb_parser_, Transport::kUsbSerialJtag, byte);
    }
#endif

    if (provisioning_pending_) {
      if (wifi_manager_.is_station_connected()) {
        on_wifi_connected();
      } else if (elapsed(provisioning_started_at_, kConnectTimeout)) {
        on_wifi_connection_timeout();
      }
    }
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

void SerialProvisioner::process_byte(Parser& parser, Transport transport, uint8_t byte) {
  if (parser.length == 0 && byte != static_cast<uint8_t>(kHeader[0])) {
    return;
  }
  if (parser.length < sizeof(kHeader) - 1 && byte != static_cast<uint8_t>(kHeader[parser.length])) {
    parser.length = byte == static_cast<uint8_t>(kHeader[0]) ? 1 : 0;
    return;
  }
  if (parser.length >= parser.bytes.size()) {
    parser.length = 0;
    return;
  }

  parser.bytes[parser.length++] = byte;
  if (parser.length == sizeof(kHeader)) {
    return;
  }
  if (parser.length == sizeof(kHeader) + 1 && byte != kProtocolVersion) {
    parser.length = 0;
    return;
  }
  if (parser.length < 9) {
    return;
  }

  const size_t packet_length = 10U + parser.bytes[8];
  if (packet_length > parser.bytes.size()) {
    parser.length = 0;
    return;
  }
  if (parser.length == packet_length) {
    process_packet(transport, parser.bytes.data(), packet_length);
    parser.length = 0;
  }
}

void SerialProvisioner::process_packet(Transport transport, const uint8_t* packet, size_t packet_length) {
  if (packet_length < 10 || checksum(packet, packet_length - 1) != packet[packet_length - 1]) {
    send_error(transport, kErrorInvalidRpc);
    return;
  }
  if (packet[7] != kTypeRpc) {
    return;
  }
  process_rpc(transport, packet + 9, packet[8]);
}

void SerialProvisioner::process_rpc(Transport transport, const uint8_t* data, size_t data_length) {
  if (data_length < 2 || data[1] != data_length - 2) {
    send_error(transport, kErrorInvalidRpc);
    return;
  }
  send_error(transport, kErrorNone);

  switch (data[0]) {
    case kCommandWifiSettings: {
      if (data_length < 4) {
        send_error(transport, kErrorInvalidRpc);
        return;
      }
      const size_t ssid_length = data[2];
      const size_t password_length_index = 3 + ssid_length;
      if (password_length_index >= data_length) {
        send_error(transport, kErrorInvalidRpc);
        return;
      }
      const size_t password_length = data[password_length_index];
      if (password_length_index + 1 + password_length != data_length) {
        send_error(transport, kErrorInvalidRpc);
        return;
      }

      WifiCredentials credentials;
      credentials.ssid.assign(reinterpret_cast<const char*>(data + 3), ssid_length);
      credentials.password.assign(reinterpret_cast<const char*>(data + password_length_index + 1),
                                  password_length);
      if (!credentials.is_configured() ||
          config_store_.save_wifi_credentials(credentials) != ESP_OK ||
          wifi_manager_.connect_station(credentials) != ESP_OK) {
        send_error(transport, kErrorUnableToConnect);
        return;
      }
      provisioning_pending_ = true;
      provisioning_started_at_ = xTaskGetTickCount();
      broadcast_state();
      return;
    }

    case kCommandCurrentState:
      send_state(transport);
      if (wifi_manager_.is_station_connected()) {
        send_rpc_result(transport, kCommandCurrentState, {device_url()});
      }
      return;

    case kCommandDeviceInfo:
      send_rpc_result(transport, kCommandDeviceInfo,
                      {"PrintSphere", PRINTSPHERE_RELEASE_VERSION, hardware_variant(),
                       config_store_.load_device_name()});
      return;

    case kCommandWifiNetworks: {
      const std::vector<VisibleWifiNetwork> networks = wifi_manager_.scan_visible_network_details();
      for (const VisibleWifiNetwork& network : networks) {
        send_rpc_result(transport, kCommandWifiNetworks,
                        {network.ssid, std::to_string(network.rssi),
                         network.auth_required ? "YES" : "NO"});
      }
      send_rpc_result(transport, kCommandWifiNetworks, {});
      return;
    }

    default:
      send_error(transport, kErrorUnknownRpc);
      return;
  }
}

void SerialProvisioner::send_state(Transport transport) {
  const uint8_t state = provisioning_pending_
                            ? kStateProvisioning
                            : (wifi_manager_.is_station_connected() ? kStateProvisioned : kStateReady);
  send_packet(transport, kTypeCurrentState, &state, 1);
}

void SerialProvisioner::broadcast_state() {
  if (uart_available_) {
    send_state(Transport::kUart);
  }
  if (usb_available_) {
    send_state(Transport::kUsbSerialJtag);
  }
}

void SerialProvisioner::send_error(Transport transport, uint8_t error) {
  send_packet(transport, kTypeErrorState, &error, 1);
}

void SerialProvisioner::send_rpc_result(Transport transport, uint8_t command,
                                        const std::vector<std::string>& values) {
  std::vector<uint8_t> result;
  result.reserve(3 + values.size() * 16);
  result.push_back(command);
  result.push_back(0);
  for (const std::string& value : values) {
    const size_t value_length = std::min<size_t>(value.size(), 255);
    result.push_back(static_cast<uint8_t>(value_length));
    result.insert(result.end(), value.begin(), value.begin() + static_cast<std::ptrdiff_t>(value_length));
  }
  result[1] = static_cast<uint8_t>(result.size() - 2);
  send_packet(transport, kTypeRpcResult, result.data(), result.size());
}

void SerialProvisioner::send_packet(Transport transport, uint8_t type, const uint8_t* data,
                                    size_t data_length) {
  if (data_length > 255) {
    return;
  }
  std::array<uint8_t, 265> packet{};
  std::memcpy(packet.data(), kHeader, sizeof(kHeader) - 1);
  packet[6] = kProtocolVersion;
  packet[7] = type;
  packet[8] = static_cast<uint8_t>(data_length);
  if (data_length > 0) {
    std::memcpy(packet.data() + 9, data, data_length);
  }
  const size_t packet_length = 10 + data_length;
  packet[packet_length - 1] = checksum(packet.data(), packet_length - 1);

  if (transport == Transport::kUart) {
    uart_write_bytes(UART_NUM_0, packet.data(), packet_length);
    return;
  }
#if SOC_USB_SERIAL_JTAG_SUPPORTED && CONFIG_USJ_ENABLE_USB_SERIAL_JTAG
  usb_serial_jtag_write_bytes(packet.data(), packet_length, pdMS_TO_TICKS(20));
#else
  (void)packet;
#endif
}

void SerialProvisioner::on_wifi_connected() {
  provisioning_pending_ = false;
  ESP_LOGI(kTag, "Wi-Fi credentials provisioned over Improv Serial");
  broadcast_state();
  if (uart_available_) {
    send_rpc_result(Transport::kUart, kCommandWifiSettings, {device_url()});
  }
  if (usb_available_) {
    send_rpc_result(Transport::kUsbSerialJtag, kCommandWifiSettings, {device_url()});
  }
}

void SerialProvisioner::on_wifi_connection_timeout() {
  provisioning_pending_ = false;
  ESP_LOGW(kTag, "Wi-Fi provisioning timed out");
  if (uart_available_) {
    send_error(Transport::kUart, kErrorUnableToConnect);
    send_state(Transport::kUart);
  }
  if (usb_available_) {
    send_error(Transport::kUsbSerialJtag, kErrorUnableToConnect);
    send_state(Transport::kUsbSerialJtag);
  }
}

std::string SerialProvisioner::device_url() const {
  const std::string ip = wifi_manager_.station_ip();
  return ip.empty() ? std::string{} : "http://" + ip + "/";
}

}  // namespace printsphere
