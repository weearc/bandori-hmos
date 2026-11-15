/*
 * Android NDK sensor shims: honest "no sensor backend available" semantics.
 *
 * getInstance() returns NULL.  Unity checks it (cbz) before creating an event
 * queue and only passes it to destroyEventQueue() on teardown, where AOSP
 * rejects a NULL manager with BAD_VALUE.  Everything else fails closed so no
 * fabricated sensor data can ever reach the guest.
 */
#include "android_sensor_shim.h"

#include <errno.h>
#include <stdio.h>

extern int sdk_trace_fprintf(FILE *, const char *, ...);

#define SENSOR_BAD_VALUE (-EINVAL)

void *android_sensor_manager_get_instance(void)
{
    sdk_trace_fprintf(stderr,
        "ANDROID_SENSOR getInstance=null backend=unavailable honest_no_sensor_data=1\n");
    return NULL;
}

void *android_sensor_manager_get_instance_for_package(const char *package)
{
    (void)package;
    sdk_trace_fprintf(stderr,
        "ANDROID_SENSOR getInstanceForPackage=null backend=unavailable\n");
    return NULL;
}

void *android_sensor_manager_get_default_sensor(const void *manager, int type)
{
    (void)type;
    if (!manager) return NULL;
    return NULL;
}

int android_sensor_manager_get_sensor_list(const void *manager, void **list)
{
    if (!manager || !list) return SENSOR_BAD_VALUE;
    *list = NULL;
    return 0; /* empty sensor list */
}

void *android_sensor_manager_create_event_queue(const void *manager, void *looper,
                                                int ident, void *callback,
                                                void *data)
{
    (void)looper;
    (void)ident;
    (void)callback;
    (void)data;
    if (!manager) return NULL;
    return NULL;
}

int android_sensor_manager_destroy_event_queue(const void *manager, void *queue)
{
    /* AOSP: NULL manager or queue is BAD_VALUE; a queue can only exist for a
     * real manager, and this build never creates one. */
    if (!manager || !queue) return SENSOR_BAD_VALUE;
    return SENSOR_BAD_VALUE;
}

int android_sensor_event_queue_enable_sensor(void *queue, const void *sensor)
{
    if (!queue || !sensor) return SENSOR_BAD_VALUE;
    return SENSOR_BAD_VALUE;
}

int android_sensor_event_queue_disable_sensor(void *queue, const void *sensor)
{
    if (!queue || !sensor) return SENSOR_BAD_VALUE;
    return SENSOR_BAD_VALUE;
}

int android_sensor_event_queue_set_event_rate(void *queue, const void *sensor,
                                              int32_t usec)
{
    (void)usec;
    if (!queue || !sensor) return SENSOR_BAD_VALUE;
    return SENSOR_BAD_VALUE;
}

int android_sensor_event_queue_has_events(const void *queue)
{
    if (!queue) return SENSOR_BAD_VALUE;
    return 0;
}

long android_sensor_event_queue_get_events(void *queue, void *events, size_t count)
{
    (void)events;
    (void)count;
    if (!queue) return SENSOR_BAD_VALUE;
    return 0; /* no events; nothing fabricated */
}

int android_sensor_get_type(const void *sensor)
{
    (void)sensor;
    return 0; /* SENSOR_TYPE_ACCELEROMETER is never exposed: no sensor objects exist */
}

const char *android_sensor_get_name(const void *sensor)
{
    (void)sensor;
    return NULL;
}

const char *android_sensor_get_vendor(const void *sensor)
{
    (void)sensor;
    return NULL;
}

float android_sensor_get_resolution(const void *sensor)
{
    (void)sensor;
    return 0.0f;
}

int32_t android_sensor_get_min_delay(const void *sensor)
{
    (void)sensor;
    return 0;
}
