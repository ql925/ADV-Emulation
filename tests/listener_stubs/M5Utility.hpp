#pragma once
#include <algorithm>
#include <stdint.h>
#include <cstring>
#include "m5_utility/compatibility_feature.hpp"
namespace listener_test { extern uint32_t clock_ms; }
namespace m5 {
namespace utility {
inline uint32_t millis() { return listener_test::clock_ms; }
inline bool hasElapsed(uint32_t start, uint32_t limit) { return millis() - start >= limit; }
}
}
#define M5_LIB_LOGE(...) ((void)0)
#define M5_LIB_LOGW(...) ((void)0)
#define M5_LIB_LOGD(...) ((void)0)
