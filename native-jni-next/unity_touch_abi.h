#pragma once
#include <stdint.h>
#define UNITY_TOUCH_MAX 10
typedef struct {int32_t id;float x,y,pressure;} unity_touch_point;
typedef struct {
 uint32_t size,version;
 int32_t action,count;
 int64_t event_ms,down_ms;
 unity_touch_point points[UNITY_TOUCH_MAX];
} unity_touch_packet;
