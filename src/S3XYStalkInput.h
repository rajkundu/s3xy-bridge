#pragma once

#include <Arduino.h>
#include "S3XYButton.h"

void s3xy_stalk_input_queue_action(S3XYButtonAction action);
bool s3xy_stalk_input_dequeue_action(S3XYButtonAction* action);
void s3xy_stalk_input_handle_notify(const uint8_t* data, size_t len);