#pragma once
#include <cstdlib>
#include <cstring>
#include "cJSON.h"
#include "printsphere/printer_state.hpp"

namespace printsphere {
inline std::shared_ptr<AmsSnapshot> detached_ams(const std::shared_ptr<AmsSnapshot>& current) {
  return current ? std::make_shared<AmsSnapshot>(*current) : nullptr;
}
inline bool tray_is_explicitly_empty(const cJSON* tray) {
  const cJSON* type = cJSON_GetObjectItemCaseSensitive(tray, "tray_type");
  return cJSON_IsNull(type) || (cJSON_IsString(type) && type->valuestring[0] == 0);
}

inline int protocol_snow_tray(const cJSON* print, int active_nozzle) {
  const cJSON* device = cJSON_GetObjectItemCaseSensitive(print, "device");
  const cJSON* extruder = cJSON_GetObjectItemCaseSensitive(device, "extruder");
  const cJSON* info = cJSON_GetObjectItemCaseSensitive(extruder, "info");
  const cJSON* item = nullptr;
  cJSON_ArrayForEach(item, info) {
    const cJSON* id = cJSON_GetObjectItemCaseSensitive(item, "id");
    const int nozzle = cJSON_IsString(id) ? std::atoi(id->valuestring) :
                       (cJSON_IsNumber(id) ? id->valueint : -1);
    if (nozzle != (active_nozzle >= 0 ? active_nozzle : 0)) continue;
    const cJSON* snow = cJSON_GetObjectItemCaseSensitive(item, "snow");
    if (!cJSON_IsNumber(snow)) return -1;
    const int ams = (snow->valueint >> 8) & 255;
    const int slot = snow->valueint & 255;
    return ams == 255 ? 254 : policy::tray_id(ams, slot);
  }
  return -1;
}

inline void merge_nozzle_temperature_patch(const cJSON* array, int active_nozzle,
    float* active, bool* active_present, float* secondary, bool* secondary_present) {
  const cJSON* item = nullptr;
  int index = 0;
  bool inferred_active=false;
  const int count=cJSON_GetArraySize(array);
  cJSON_ArrayForEach(item, array) {
    const cJSON* id = cJSON_GetObjectItemCaseSensitive(item, "id");
    int nozzle=count==1 && active_nozzle>=0 ? active_nozzle : index;
    if(id) {
      if(cJSON_IsNumber(id)) {
        if(!std::isfinite(id->valuedouble) || id->valuedouble<0 || id->valuedouble>255 ||
           std::floor(id->valuedouble)!=id->valuedouble) { ++index; continue; }
        nozzle=id->valueint;
      } else if(cJSON_IsString(id)) {
        char* end=nullptr;
        const long parsed=std::strtol(id->valuestring,&end,10);
        if(end==id->valuestring || *end || parsed<0 || parsed>255) { ++index; continue; }
        nozzle=static_cast<int>(parsed);
      } else { ++index; continue; }
    }
    ++index;
    const cJSON* field = cJSON_GetObjectItemCaseSensitive(item, "temp");
    if (!cJSON_IsNumber(field) && !cJSON_IsString(field)) continue;
    char* end = nullptr;
    double value = cJSON_IsNumber(field) ? field->valuedouble : std::strtod(field->valuestring, &end);
    if ((end && (*end || end == field->valuestring)) || !std::isfinite(value) ||
        value < 0 || value > UINT32_MAX) continue;
    if (value > 65535) value = static_cast<uint32_t>(value) & 0xffff;
    if (nozzle == active_nozzle || (active_nozzle<0 && !inferred_active)) {
      *active = value; *active_present = true; inferred_active=true;
    }
    else if (nozzle >= 0) { *secondary = value; *secondary_present = true; }
  }
}

inline bool json_mask(const cJSON* object, const char* key, uint32_t* mask) {
  const cJSON* value = cJSON_GetObjectItemCaseSensitive(object, key);
  if (cJSON_IsNumber(value)) { *mask = static_cast<uint32_t>(value->valuedouble); return true; }
  if (!cJSON_IsString(value) || !value->valuestring || !*value->valuestring) return false;
  char* end = nullptr;
  const unsigned long parsed = std::strtoul(value->valuestring, &end, 16);
  if (!end || *end) return false;
  *mask = static_cast<uint32_t>(parsed);
  return true;
}

inline bool is_full_report(const cJSON* root, const cJSON* print) {
  const cJSON* command = cJSON_GetObjectItemCaseSensitive(print, "command");
  return cJSON_GetObjectItemCaseSensitive(root, "pushall") ||
      (cJSON_IsString(command) && std::strcmp(command->valuestring, "pushall") == 0);
}

// Absence in an ordinary patch preserves a value. Only an explicit presence
// mask or an explicitly identified full report removes an AMS/slot.
inline void apply_ams_presence(const cJSON* ams, AmsSnapshot& state) {
  uint32_t units = 0;
  if (json_mask(ams, "ams_exist_bits", &units)) {
    for (int i = 0; i < 4; ++i) {
      if (!(units & (1U << i))) state.units[i] = {};
    }
  }
  const cJSON* array = cJSON_GetObjectItemCaseSensitive(ams, "ams");
  const cJSON* item = nullptr;
  cJSON_ArrayForEach(item, array) {
    const cJSON* id = cJSON_GetObjectItemCaseSensitive(item, "id");
    const int protocol_id = cJSON_IsString(id) ? std::atoi(id->valuestring) :
                            (cJSON_IsNumber(id) ? id->valueint : -1);
    const int index = policy::ams_index(protocol_id);
    uint32_t trays = 0;
    if (index < 0 || !json_mask(item, "tray_exist_bits", &trays)) continue;
    auto& unit = state.units[index];
    unit.protocol_id = protocol_id;
    unit.present = true;
    for (int slot = 0; slot < kMaxAmsTrays; ++slot) {
      if (!(trays & (1U << slot))) unit.trays[slot] = {};
      else unit.trays[slot].present = true;
    }
  }
  state.count = 0;
  for (int i = 0; i < kMaxAmsUnits; ++i) {
    if (state.units[i].present) state.count = i + 1;
  }
}

inline bool has_core_status(const cJSON* print) {
  for (const char* key : {"gcode_state", "mc_percent", "layer_num", "mc_remaining_time", "stg_cur",
                         "stage", "status", "task_state", "taskState", "print_status"}) {
    if (cJSON_GetObjectItemCaseSensitive(print, key)) return true;
  }
  return false;
}

inline bool has_temperature_patch(const cJSON* object) {
  if (!object) return false;
  for (const cJSON* item = object->child; item; item = item->next) {
    if (item->string && (std::strcmp(item->string, "temp") == 0 ||
        std::strcmp(item->string, "temperature") == 0)) return true;
    if ((cJSON_IsArray(item) || cJSON_IsObject(item)) && has_temperature_patch(item)) return true;
  }
  return false;
}
}  // namespace printsphere
