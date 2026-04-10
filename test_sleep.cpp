#include <Arduino.h>
#include <driver/rtc_io.h>
void setup() {
  gpio_pullup_en(GPIO_NUM_7);
  gpio_hold_en(GPIO_NUM_7);
  gpio_deep_sleep_hold_en();
}
void loop() {}
