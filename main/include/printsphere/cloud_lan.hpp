#pragma once
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include "cJSON.h"

namespace printsphere {
// Bambu encodes net.info[].ip in little-endian byte order (also used by HA).
inline std::string cloud_lan_host(const cJSON* print) {
  const auto* net=cJSON_GetObjectItemCaseSensitive(print,"net");
  const auto* info=cJSON_GetObjectItemCaseSensitive(net,"info");
  if (!cJSON_IsArray(info)) return {};
  const cJSON* item=nullptr;
  cJSON_ArrayForEach(item,info) {
    const auto* ip=cJSON_GetObjectItemCaseSensitive(item,"ip");
    if(!cJSON_IsNumber(ip) || !std::isfinite(ip->valuedouble) ||
       ip->valuedouble<=0 || ip->valuedouble>UINT32_MAX ||
       std::floor(ip->valuedouble)!=ip->valuedouble) continue;
    const auto value=static_cast<uint32_t>(ip->valuedouble);
    const unsigned first=value&255;
    if(first==0 || first==127 || first>=224) continue;
    char host[16];
    std::snprintf(host,sizeof(host),"%u.%u.%u.%u",first,
                  unsigned((value>>8)&255),unsigned((value>>16)&255),unsigned(value>>24));
    return host;
  }
  return {};
}
inline std::string cloud_lan_access_code(const cJSON* device) {
  const auto* code=cJSON_GetObjectItemCaseSensitive(device,"dev_access_code");
  if(!cJSON_IsString(code) || !code->valuestring) return {};
  const std::string value=code->valuestring;
  if(value.empty() || value.size()>64 || value.find_first_of("\r\n")!=std::string::npos ||
     value.find('*')!=std::string::npos) return {};
  for (unsigned char byte : value) if (byte <= 0x20 || byte == 0x7f) return {};
  return value;
}
}  // namespace printsphere
