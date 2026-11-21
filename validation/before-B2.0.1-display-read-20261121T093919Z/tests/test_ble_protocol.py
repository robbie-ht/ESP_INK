#!/usr/bin/env python3
"""Host checks for extracted production BLE parsers; no ESP32 hardware required.
Run from any directory: python3 tests/test_ble_protocol.py
A C11 host compiler and tests/store_stubs/esp_err.h are required.
"""
from pathlib import Path
import os
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
src = (ROOT / "src/ble_api.c").read_text()
def fn(name):
    match=re.search(r'^(?:static )?(?:inline )?[\w *]+\b'+name+r'\([^\n]*(?:\n[^\{]*)?\)\s*\{',src,re.M)
    if not match: raise RuntimeError(name)
    end=src.index('{',match.start()); depth=1; pos=end+1
    while depth:
        if src[pos]=='{': depth+=1
        if src[pos]=='}': depth-=1
        pos+=1
    return src[match.start():pos]
prefix=r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ble_api.h"
#include "settings.h"
#include "store.h"
#include "crc_util.h"
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define pdTRUE 1
#define portMAX_DELAY 0xffffffffu
static uint8_t *s_fb,*s_thumb,s_state;
static uint16_t s_fw,s_fh,s_expect,s_chunk,s_crc_calc,s_crc_want,s_thumb_w,s_thumb_h,s_thumb_crc_want,s_thumb_crc_calc;
static uint32_t s_framelen,s_recv,s_thumb_len,s_ota_total,s_ota_written,s_ota_erased,s_ota_crc_calc;
static uint8_t s_begin_flags,s_thumb_format,s_ota_err;
static bool s_gap_notified;
static void *s_ota_part,*s_job_q;
static void *s_lock=(void*)1;
static bool idle_expired;
static int lock_depth;
static int xSemaphoreTake(void *lock,unsigned wait){(void)lock;(void)wait;lock_depth++;return 1;}
static void xSemaphoreGive(void *lock){(void)lock;lock_depth--;}
static bool power_should_sleep(void){assert(lock_depth==1);return idle_expired;}
static int64_t s_last_rx_ms;
static uint8_t rsp_status,rsp[600]; static uint16_t rsp_len, mtu=517;
static int count=1, reads, applied,queued;static bool fail_queue,fail_flash;
static uint32_t revision=9;
static ble_job_t last_job;
static moink_settings_t cfg;
static esp_err_t apply_error;
const pin_cfg_t PIN_PRESETS[GPIO_PRESET_NPRESET]={{{1}},{{3}},{{4}}};
typedef struct {uint16_t w,h;uint32_t buf_len;} epd_profile_t;
typedef int epd_panel_t;
static const epd_profile_t profile={16,8,32};
static const epd_profile_t *epd_profile(void) {return &profile;}
static int64_t esp_timer_get_time(void){return 2000000;}
static void power_activity(void){}
static uint16_t cur_mtu(void){return mtu;}
static uint16_t max_chunk(void){return mtu-5;}
static void respond(uint8_t seq,uint8_t status,const void *p,uint16_t n){(void)seq;rsp_status=status;rsp_len=n;if(n)memcpy(rsp,p,n);}
void ble_api_notify_event(uint8_t e,uint8_t a,const void*p,uint8_t n){(void)e;(void)a;(void)p;(void)n;}
int store_slots(void){return 8;}
void store_meta_get(int*c,int*cur){if(c)*c=count;if(cur)*cur=0;}
uint32_t store_revision(void){return revision;}
esp_err_t store_info(int slot,store_info_t*i){if(slot>=count)return ESP_ERR_NOT_FOUND;*i=(store_info_t){16,8,0x1234,32,1,4,4,4,0x5678};return ESP_OK;}
esp_err_t store_read(int slot,bool thumb,uint32_t off,void*out,size_t n){(void)slot;(void)thumb;(void)off;reads++;memset(out,0xA5,n);return ESP_OK;}
static int xQueueSend(void*q,const ble_job_t*j,int t){(void)q;(void)t;if(fail_queue)return 0;queued++;last_job=*j;return 1;}
static esp_err_t ota_flash_write(uint32_t off,const uint8_t*p,uint32_t n){(void)off;(void)p;(void)n;return fail_flash?ESP_FAIL:ESP_OK;}
const moink_settings_t *settings_get(void){return &cfg;}
bool pin_cfg_valid(const pin_cfg_t*p){return p->v[0]>0;}
esp_err_t settings_apply(const moink_settings_t*p){applied++;if(apply_error==ESP_OK)cfg=*p;return apply_error;}
static void epd_set_panel(epd_panel_t panel){(void)panel;}
void ble_api_restart_adv(void){}
'''
defines='\n'.join(x for x in src.splitlines() if re.match(r'#define (?:ST_|TLV_|MAX_ATT_MTU|MIN_CHUNK|EVT_TRANSFER_GAP)',x))
body='\n\n'.join(fn(n) for n in ['rd16','rd32','wr16','wr32','transfer_reset','handle_frame_begin','handle_frame_end','handle_set_settings','store_status','handle_store_list','handle_store_read','on_data','ble_api_prepare_idle_sleep'])
main=r'''
int main(void){
 uint8_t image[32],thumb[4]={0,1,2,3},begin[24]={6},data[20]={0};
 for(unsigned i=0;i<32;i++)image[i]=(uint8_t)(i+11);
 s_fb=malloc(32);wr16(begin+1,16);wr16(begin+3,8);wr32(begin+5,32);wr16(begin+9,crc16_of(image,32));wr16(begin+11,18);
 begin[13]=1;wr16(begin+14,4);wr16(begin+16,4);wr32(begin+18,4);wr16(begin+22,crc16_of(thumb,4));
 handle_frame_begin(1,begin,24);assert(rsp_status==ST_OK&&s_state==BLE_ST_RECV_FRAME);
 memcpy(data+2,image,18);on_data(data,19);assert(s_recv==0&&s_expect==0); /* short chunk rejected */
 on_data(data,20);assert(s_recv==18&&s_expect==1);
 on_data(data,20);assert(s_recv==18); /* duplicate ignored */
 wr16(data,1);memcpy(data+2,image+18,14);memcpy(data+16,thumb,4);on_data(data,20);
 assert(s_recv==36&&!memcmp(image,s_fb,32)&&!memcmp(thumb,s_thumb,4));
 s_thumb_crc_calc^=1;handle_frame_end(2);assert(rsp_status==ST_CRC&&s_recv==0&&s_expect==0);
 wr16(data,0);memcpy(data+2,image,18);on_data(data,20);
 wr16(data,1);memcpy(data+2,image+18,14);memcpy(data+16,thumb,4);on_data(data,20);
 fail_queue=true;handle_frame_end(2);assert(rsp_status==ST_BUSY&&s_state==BLE_ST_RECV_FRAME);
 fail_queue=false;handle_frame_end(2);assert(rsp_status==ST_OK&&queued==1&&last_job.no_show&&last_job.save_store&&last_job.thumb_len==4);
 transfer_reset();s_state=BLE_ST_IDLE;count=8;handle_frame_begin(1,begin,24);assert(rsp_status==ST_STORE_FULL);count=3;
 begin[0]=4;handle_frame_begin(1,begin,24);assert(rsp_status==ST_BAD_ARG);begin[0]=6;
 wr16(begin+14,201);handle_frame_begin(1,begin,24);assert(rsp_status==ST_BAD_ARG);wr16(begin+14,4);
 cfg.pins.v[0]=1;uint8_t tlv[]={TLV_WAKE_SHOW,1,1,TLV_SLEEP,4,42,0,0,0};
 handle_set_settings(1,tlv,sizeof(tlv));assert(rsp_status==ST_OK&&cfg.wake_show==1&&cfg.sleep_s==42&&applied==1);
 uint8_t bad[]={TLV_WAKE_SHOW,1,0,TLV_SLEEP};handle_set_settings(1,bad,sizeof(bad));assert(rsp_status==ST_BAD_ARG&&cfg.wake_show==1&&applied==1);
 apply_error=ESP_FAIL;tlv[2]=0;handle_set_settings(1,tlv,sizeof(tlv));assert(rsp_status==ST_FLASH_ERROR&&cfg.wake_show==1);apply_error=ESP_OK;
 uint8_t pins_first[]={TLV_PINS,8,1,2,3,4,5,6,7,8,TLV_PRESET,1,3};handle_set_settings(1,pins_first,sizeof(pins_first));assert(rsp_status==ST_OK&&cfg.gpio_preset==3);
 uint8_t request[12]={0,0};wr16(request+6,100);wr32(request+8,revision);mtu=23;
 handle_store_read(1,request,12);assert(rsp_status==ST_OK&&rsp_len==17&&rsp[0]==0&&rsp[12]==1&&reads==1);
 wr32(request+8,revision-1);handle_store_read(1,request,12);assert(rsp_status==ST_STALE&&reads==1);
 uint8_t list[2]={0,8};handle_store_list(1,list,2);assert(rsp_status==ST_UNSUPPORTED);
 mtu=64;handle_store_list(1,list,2);assert(rsp_status==ST_OK&&rsp_len==50&&rsp[5]==2&&rsp[6]==0&&rsp[28]==1);
 s_state=BLE_ST_RECV_OTA;s_ota_part=(void*)1;s_ota_total=36;s_chunk=18;s_expect=0;s_ota_err=0;s_ota_written=0;fail_flash=true;wr16(data,0);
 on_data(data,20);assert(s_state==BLE_ST_RECV_OTA&&s_ota_err==ST_FLASH_ERROR&&s_ota_written==0);
 s_state=BLE_ST_IDLE;idle_expired=true;bool stale_idle_decision=idle_expired;
 idle_expired=false; /* host PING after idle task's initial timeout sample */
 assert(stale_idle_decision&&!ble_api_prepare_idle_sleep()&&s_state==BLE_ST_IDLE&&lock_depth==0);
 idle_expired=true;assert(ble_api_prepare_idle_sleep()&&s_state==BLE_ST_SLEEPING&&lock_depth==0);
 s_state=BLE_ST_RECV_FRAME;assert(!ble_api_prepare_idle_sleep()&&s_state==BLE_ST_RECV_FRAME&&lock_depth==0);
 free(s_fb);puts("BLE production parser checks passed: chunk boundary/CRC, queue retry, gallery limits, atomic settings, stale read, MTU pagination, OTA error latch, idle activity recheck");
}
'''
with tempfile.TemporaryDirectory(prefix="moink-ble-test-") as temp:
    test_c = Path(temp) / "ble_protocol.c"
    binary = Path(temp) / "ble_protocol"
    test_c.write_text(prefix + defines + "\n" + body + main)
    subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-I", str(ROOT / "src"), "-I", str(ROOT / "tests/store_stubs"),
                    str(test_c), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
