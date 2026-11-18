#include <cstdio>
#include <cstdlib>
#include <string>
#include "printsphere/cloud_lan.hpp"

#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "Cloud LAN check failed at %d: %s\n", __LINE__, #c); std::exit(2); } } while (0)

int main() {
  using namespace printsphere;
  auto host = [](const char* json) {
    auto* value = cJSON_Parse(json);
    auto result = cloud_lan_host(value);
    cJSON_Delete(value);
    return result;
  };
  CHECK(host(R"({"net":{"info":[{"ip":800893120}]}})") == "192.168.188.47");
  CHECK(host(R"({"net":{"info":[{"ip":0},{"ip":800893120}]}})") == "192.168.188.47");
  for (const char* json : {"null", "{}", R"({"net":null})", R"({"net":{"info":{"ip":800893120}}})",
       R"({"net":{"info":[null,{}, {"ip":"800893120"}]}})",
       R"({"net":{"info":[{"ip":-1},{"ip":4294967296},{"ip":1.5},{"ip":1e999}]}})",
       R"({"net":{"info":[{"ip":0},{"ip":4294967295},{"ip":127},{"ip":224}]}})"}) {
    CHECK(host(json).empty());
  }
  auto code = [](const char* json) {
    auto* value = cJSON_Parse(json);
    auto result = cloud_lan_access_code(value);
    cJSON_Delete(value);
    return result;
  };
  CHECK(code(R"({"dev_access_code":"test1234"})") == "test1234");
  for (const char* json : {"null", "{}", R"({"dev_access_code":null})", R"({"dev_access_code":12345678})",
       R"({"dev_access_code":""})", R"({"dev_access_code":"********"})",
       R"({"dev_access_code":"test\r\nHeader:value"})", R"({"dev_access_code":"test\t1234"})",
       R"({"dev_access_code":"        "})"}) CHECK(code(json).empty());
  const auto long_code = std::string("{\"dev_access_code\":\"") + std::string(65, 'x') + "\"}";
  CHECK(code(long_code.c_str()).empty());
  std::puts("Cloud LAN: little-endian IP, malformed metadata and masked/control-character access codes passed");
}
