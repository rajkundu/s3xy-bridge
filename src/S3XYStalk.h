#pragma once

#include <Arduino.h>

typedef void (*s3xy_stalk_notify_cb_t)(
    const uint8_t* data,
    size_t len
);

void s3xy_stalk_begin();
void s3xy_stalk_loop();

bool s3xy_stalk_connected();
bool s3xy_stalk_ready();

void s3xy_stalk_on_notify(s3xy_stalk_notify_cb_t cb);
