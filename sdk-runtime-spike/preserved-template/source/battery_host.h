#pragma once
#include <stdint.h>

/* Shared C ABI for the ArkTS bridge. Return 0 on success, nonzero on error.
 * Voltage is millivolts; temperature is tenths Celsius. Unknown fields have
 * their present_mask bit clear and are omitted from Intent extras. */
typedef struct battery_host_event {
 uint64_t present_mask, generation;
 int32_t level, scale, status, health, plugged, temperature, voltage, present;
 char technology[64];
} battery_host_event;
enum { BATTERY_HAS_LEVEL=1u<<0, BATTERY_HAS_SCALE=1u<<1,
 BATTERY_HAS_STATUS=1u<<2, BATTERY_HAS_HEALTH=1u<<3,
 BATTERY_HAS_PLUGGED=1u<<4, BATTERY_HAS_TEMPERATURE=1u<<5,
 BATTERY_HAS_VOLTAGE=1u<<6, BATTERY_HAS_PRESENT=1u<<7,
 BATTERY_HAS_TECHNOLOGY=1u<<8 };
typedef void (*battery_host_emit)(void *user, const battery_host_event *event);
typedef struct battery_host_callbacks {
 int (*subscribe)(void *host, battery_host_emit emit, void *user);
 void (*unsubscribe)(void *host, void *user); /* synchronous callback quiescence */
 int (*snapshot)(void *host, battery_host_event *out);
} battery_host_callbacks;
