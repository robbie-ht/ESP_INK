#include "../store_stubs/nvs.h"
#include <stdint.h>
esp_err_t nvs_get_u8(nvs_handle_t h,const char *key,uint8_t *v);
esp_err_t nvs_get_u32(nvs_handle_t h,const char *key,uint32_t *v);
esp_err_t nvs_get_str(nvs_handle_t h,const char *key,char *v,size_t *n);
esp_err_t nvs_erase_all(nvs_handle_t h);
