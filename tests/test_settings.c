#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "settings.h"
#include "nvs.h"
static unsigned char saved[64], pending[64];
static size_t saved_len, pending_len;
static int fail_set, fail_commit;
esp_err_t nvs_open(const char *ns,int mode,nvs_handle_t *h) {(void)ns;(void)mode;*h=1;return ESP_OK;}
void nvs_close(nvs_handle_t h) {(void)h;}
esp_err_t nvs_get_u8(nvs_handle_t h,const char *k,uint8_t *v) {(void)h;(void)k;(void)v;return ESP_ERR_NVS_NOT_FOUND;}
esp_err_t nvs_get_u32(nvs_handle_t h,const char *k,uint32_t *v) {(void)h;(void)k;(void)v;return ESP_ERR_NVS_NOT_FOUND;}
esp_err_t nvs_get_str(nvs_handle_t h,const char *k,char *v,size_t *n) {(void)h;(void)k;(void)v;(void)n;return ESP_ERR_NVS_NOT_FOUND;}
esp_err_t nvs_get_blob(nvs_handle_t h,const char *k,void *v,size_t *n) {
 (void)h;assert(!strcmp(k,"config_v2"));if(!saved_len)return ESP_ERR_NVS_NOT_FOUND;
 if(*n<saved_len)return ESP_ERR_NVS_INVALID_LENGTH;memcpy(v,saved,saved_len);*n=saved_len;return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t h,const char *k,const void *v,size_t n) {
 (void)h;assert(!strcmp(k,"config_v2"));assert(n<=sizeof(pending));if(fail_set)return ESP_FAIL;
 memcpy(pending,v,n);pending_len=n;return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t h) {(void)h;if(fail_commit)return ESP_FAIL;memcpy(saved,pending,pending_len);saved_len=pending_len;return ESP_OK;}
esp_err_t nvs_erase_all(nvs_handle_t h) {(void)h;pending_len=0;return ESP_OK;}
int main(void) {
 settings_init();assert(settings_get()->wake_show==1);assert(settings_get()->sleep_s==180);
 moink_settings_t next=*settings_get();next.sleep_s=1234;next.wake_show=0;next.panel=1;
 strcpy(next.name,"墨印");assert(settings_apply(&next)==ESP_OK);assert(saved_len==44);
 settings_init();assert(settings_get()->sleep_s==1234);assert(settings_get()->wake_show==0);
 assert(!strcmp(settings_get()->name,"墨印"));
 moink_settings_t before=*settings_get();next=before;next.sleep_s=999;next.panel=2;
 assert(settings_apply(&next)==ESP_ERR_INVALID_ARG);assert(!memcmp(settings_get(),&before,sizeof(before)));
 next=before;next.name[0]=(char)0xc0;next.name[1]=(char)0x80;next.name[2]=0;
 assert(settings_apply(&next)==ESP_ERR_INVALID_ARG);assert(!memcmp(settings_get(),&before,sizeof(before)));
 next=before;next.gpio_preset=GPIO_PRESET_CUSTOM;next.pins.v[0]=PIN_SELF_LOCK;
 assert(settings_apply(&next)==ESP_ERR_INVALID_ARG);
 next=before;next.sleep_s=999;fail_set=1;assert(settings_apply(&next)==ESP_FAIL);
 assert(!memcmp(settings_get(),&before,sizeof(before)));fail_set=0;settings_init();assert(settings_get()->sleep_s==1234);
 fail_commit=1;assert(settings_apply(&next)==ESP_FAIL);assert(settings_get()->sleep_s==1234);fail_commit=0;
 settings_init();assert(settings_get()->sleep_s==1234);
 assert(settings_set_name("")==ESP_OK);assert(settings_get()->name[0]==0);
 assert(settings_set_gpio(GPIO_PRESET_LINE,NULL)==ESP_OK);assert(settings_pins()->v[1]==1);
 assert(settings_set_wake(UINT32_MAX)==ESP_OK);settings_init();assert(settings_get()->wake_s==UINT32_MAX);
 settings_factory_reset();settings_init();assert(settings_get()->sleep_s==180);assert(settings_get()->wake_show==1);
 puts("PASS settings: persistence, atomic validation, UTF-8, GPIO, failures, reset");return 0;
}
