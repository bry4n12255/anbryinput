#ifndef AINPUT_LATENCY_MATCH_H
#define AINPUT_LATENCY_MATCH_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define AINPUT_MATCH_CAPACITY 8192

typedef struct {
    double time, x, y;
    uint32_t server_time;
    bool after_idle;
} AInputSample;
typedef struct {
    AInputSample items[AINPUT_MATCH_CAPACITY];
    size_t head, count;
} AInputSampleQueue;

static inline bool ainput_sample_push(AInputSampleQueue *q, AInputSample sample)
{
    if (q->count == AINPUT_MATCH_CAPACITY)
        return false;
    q->items[(q->head + q->count) % AINPUT_MATCH_CAPACITY] = sample;
    q->count++;
    return true;
}

/* 0: wait for the other observer, 1: matched, -1: invalid measurement. */
static inline int ainput_sample_match(AInputSampleQueue *hardware,
                                      AInputSampleQueue *observed,
                                      bool check_values, double *latency,
                                      bool *after_idle,
                                      double *hardware_time,
                                      double *observed_time,
                                      uint32_t *server_time)
{
    if (!hardware->count || !observed->count)
        return 0;
    AInputSample a = hardware->items[hardware->head];
    AInputSample b = observed->items[observed->head];
    if ((check_values && (a.x != b.x || a.y != b.y)) || b.time < a.time)
        return -1;
    *latency = b.time - a.time;
    *after_idle = a.after_idle;
    if (hardware_time)
        *hardware_time = a.time;
    if (observed_time)
        *observed_time = b.time;
    if (server_time)
        *server_time = b.server_time;
    hardware->head = (hardware->head + 1) % AINPUT_MATCH_CAPACITY;
    observed->head = (observed->head + 1) % AINPUT_MATCH_CAPACITY;
    hardware->count--; observed->count--;
    return 1;
}
static inline AInputSample ainput_sequence_sample(size_t index)
{
    return (AInputSample){.x = (double)(index % 251) - 125.0,
                          .y = (double)(index / 251) + 1.0};
}
#endif
