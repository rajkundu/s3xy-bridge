#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_timer.h>

inline unsigned long millis() {
    return (unsigned long)(esp_timer_get_time() / 1000ULL);
}

inline void delay_ms(uint32_t ms) {
    vTaskDelay(pdMS_TO_TICKS(ms));
}
