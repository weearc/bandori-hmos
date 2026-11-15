#ifndef ANDROID_SENSOR_SHIM_H
#define ANDROID_SENSOR_SHIM_H

/*
 * Minimal Android NDK sensor (android/sensor.h) symbol shims for guest ELF
 * relocation.
 *
 * The compatibility layer has no Android sensor backend on this host, so
 * ASensorManager_getInstance() returns NULL: the honest Android semantics for
 * "no sensor manager available".  Disassembly of libunity shows every creation
 * path checks the result with `cbz` and skips sensor setup, and the teardown
 * path only passes it to ASensorManager_destroyEventQueue(), which AOSP
 * defines to reject a NULL manager with BAD_VALUE rather than crash.
 *
 * No sensor object, event, or fake data is ever fabricated.  All other entry
 * points reject NULL/foreign arguments with the AOSP failure values so an
 * accidental call cannot proceed.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void *android_sensor_manager_get_instance(void);
void *android_sensor_manager_get_instance_for_package(const char *package);
void *android_sensor_manager_get_default_sensor(const void *manager, int type);
int android_sensor_manager_get_sensor_list(const void *manager, void **list);
void *android_sensor_manager_create_event_queue(const void *manager, void *looper,
                                                int ident, void *callback,
                                                void *data);
int android_sensor_manager_destroy_event_queue(const void *manager, void *queue);
int android_sensor_event_queue_enable_sensor(void *queue, const void *sensor);
int android_sensor_event_queue_disable_sensor(void *queue, const void *sensor);
int android_sensor_event_queue_set_event_rate(void *queue, const void *sensor,
                                              int32_t usec);
int android_sensor_event_queue_has_events(const void *queue);
long android_sensor_event_queue_get_events(void *queue, void *events, size_t count);
int android_sensor_get_type(const void *sensor);
const char *android_sensor_get_name(const void *sensor);
const char *android_sensor_get_vendor(const void *sensor);
float android_sensor_get_resolution(const void *sensor);
int32_t android_sensor_get_min_delay(const void *sensor);

#ifdef __cplusplus
}
#endif

#endif
