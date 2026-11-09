#include "LightSleep.h"

#include <Arduino.h>

#if !defined(SIMULATOR)
#include <driver/gpio.h>
#include <esp_sleep.h>
#include <esp_timer.h>
#endif

namespace power {

const char* wakeName(Wake w) {
    switch (w) {
        case WAKE_TIMER: return "timer";
        case WAKE_KEY:   return "key";
        case WAKE_TOUCH: return "touch";
        case WAKE_LORA:  return "lora";
        case WAKE_OTHER: return "other";
        default:         return "none";
    }
}

#if defined(SIMULATOR)

Wake lightSleep(uint32_t ms, const WakePins&, uint32_t& sleptMs) {
    const uint32_t d = ms < 20 ? ms : 20;     // keep the simulated UI responsive
    delay(d);
    sleptMs = d;
    return WAKE_TIMER;
}

#else

namespace {
void armLevel(int pin, gpio_int_type_t level) {
    if (pin < 0) return;
    // Mask the pin's CPU interrupt first: a level-type interrupt would otherwise fire
    // continuously after wake-up while the line is still asserted.
    gpio_intr_disable((gpio_num_t)pin);
    gpio_wakeup_enable((gpio_num_t)pin, level);
}
}  // namespace

Wake lightSleep(uint32_t ms, const WakePins& pins, uint32_t& sleptMs) {
    sleptMs = 0;
    if (ms == 0) return WAKE_NONE;

    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    esp_sleep_enable_timer_wakeup((uint64_t)ms * 1000ULL);
    armLevel(pins.key, GPIO_INTR_LOW_LEVEL);
    armLevel(pins.touch, GPIO_INTR_LOW_LEVEL);
    armLevel(pins.lora, GPIO_INTR_HIGH_LEVEL);
    esp_sleep_enable_gpio_wakeup();

    const int64_t t0 = esp_timer_get_time();
    esp_light_sleep_start();
    sleptMs = (uint32_t)((esp_timer_get_time() - t0) / 1000);

    Wake why = WAKE_OTHER;
    const esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    if (cause == ESP_SLEEP_WAKEUP_TIMER) {
        why = WAKE_TIMER;
    } else if (cause == ESP_SLEEP_WAKEUP_GPIO) {
        if (pins.lora >= 0 && gpio_get_level((gpio_num_t)pins.lora) == 1)        why = WAKE_LORA;
        else if (pins.key >= 0 && gpio_get_level((gpio_num_t)pins.key) == 0)     why = WAKE_KEY;
        else if (pins.touch >= 0 && gpio_get_level((gpio_num_t)pins.touch) == 0) why = WAKE_TOUCH;
    }

    // Undo the wake configuration. gpio_wakeup_disable() leaves the level interrupt type
    // in place, so set each pin's type back explicitly.
    if (pins.key >= 0)   { gpio_wakeup_disable((gpio_num_t)pins.key);   gpio_set_intr_type((gpio_num_t)pins.key, GPIO_INTR_DISABLE); }
    if (pins.touch >= 0) { gpio_wakeup_disable((gpio_num_t)pins.touch); gpio_set_intr_type((gpio_num_t)pins.touch, GPIO_INTR_DISABLE); }
    if (pins.lora >= 0) {
        gpio_wakeup_disable((gpio_num_t)pins.lora);
        gpio_set_intr_type((gpio_num_t)pins.lora, GPIO_INTR_POSEDGE);   // the radio's RX-done ISR
        gpio_intr_enable((gpio_num_t)pins.lora);
    }
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    return why;
}

#endif

}  // namespace power
