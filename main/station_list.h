/*
 * station_list.h - built-in list of verified MP3 internet radio streams.
 */
#pragma once

#include <stddef.h>

typedef struct {
    const char *name;
    const char *genre;
    const char *url;
    const char *note;   /* verification note, exposed by GET /api/stations */
} station_t;

size_t station_list_count(void);
const station_t *station_list_get(size_t index);
