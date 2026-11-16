#pragma once
#include <cassert>
#define ESP_ERROR_CHECK(expression) assert((expression) == 0)
#define ESP_RETURN_ON_ERROR(expression, ...) do { int error = (expression); if (error) return error; } while (0)
