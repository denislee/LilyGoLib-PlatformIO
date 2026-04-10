#include "esp_sleep.h"
void test() {
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_GPIO);
}
