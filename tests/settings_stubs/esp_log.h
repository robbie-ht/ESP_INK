#include <stdio.h>
#define ESP_LOGW(tag,...) do {(void)(tag); if(0) printf(__VA_ARGS__);}while(0)
#define ESP_LOGE(tag,...) ESP_LOGW(tag,__VA_ARGS__)
#define ESP_LOGI(tag,...) ESP_LOGW(tag,__VA_ARGS__)
static inline const char *esp_err_to_name(int error) {(void)error;return "test-error";}
