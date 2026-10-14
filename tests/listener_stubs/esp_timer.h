#pragma once
#include "M5Utility.hpp"
inline int64_t esp_timer_get_time() { return int64_t(listener_test::clock_ms) * 1000; }
