#!/usr/bin/env python3
"""Compile actual BLE/display functions; model peripherals, not their scheduling.

The close-vs-read test uses a real pthread mutex and a blocked read callback.
No BLE radio, physical Flash, or screen is exercised by these host checks.
"""
from pathlib import Path
import os
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
ble = (ROOT / "src/ble_api.c").read_text()
main_source = (ROOT / "src/main.c").read_text()


def function(source, name):
    match = re.search(r'^(?:static )?(?:inline )?[\w *]+\b' + name +
                      r'\([^\n]*(?:\n[^\{]*)?\)\s*\{', source, re.M)
    if not match:
        raise RuntimeError(name)
    end = source.index('{', match.start())
    depth, pos = 1, end + 1
    while depth:
        depth += (source[pos] == '{') - (source[pos] == '}')
        pos += 1
    return source[match.start():pos]


def command_case(label):
    source = function(ble, "on_ctrl")
    start = source.index("    case " + label + ":")
    end = source.index("{", start)
    depth, pos = 1, end + 1
    while depth:
        depth += (source[pos] == '{') - (source[pos] == '}')
        pos += 1
    return source[start:pos]


prefix = r'''
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "ble_api.h"
#include "version.h"
#include "settings.h"
#include "store.h"
#include "crc_util.h"
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define pdTRUE 1
#define portMAX_DELAY 0xffffffffu
#define EPD_MAX_BUF_LEN 120000
static pthread_mutex_t host_mutex = PTHREAD_MUTEX_INITIALIZER;
static void *s_lock = &host_mutex;
static _Thread_local int lock_depth;
static uint8_t s_state;
static bool s_display_read_window;
static bool s_display_append_committed;
static void *s_job_q = (void *)1;
static int xSemaphoreTake(void *lock, unsigned wait) {
    (void)wait; assert(pthread_mutex_lock(lock) == 0); ++lock_depth; return pdTRUE;
}
static void xSemaphoreGive(void *lock) {
    assert(lock_depth == 1); --lock_depth; assert(pthread_mutex_unlock(lock) == 0);
}
static uint8_t wire[600];
static uint16_t wire_len, mtu = 517;
static uint16_t cur_mtu(void) { return mtu; }
static void notify_raw(const uint8_t *p, uint16_t n) {
    assert(n <= 512 && n <= mtu - 3); memcpy(wire, p, n); wire_len = n;
}
static int state_events, store_events, display_events;
static uint8_t event_arg, event_flags;
static uint8_t opening_flags;
void ble_api_notify_event(uint8_t e, uint8_t a, const void *p, uint8_t n) {
    assert(lock_depth == 0);
    if (e == MOINK_EVT_STATE) {
        assert(n == 1); state_events++; event_arg = a; event_flags = *(const uint8_t *)p;
        if (event_flags & MOINK_GALLERY_READ_WINDOW) opening_flags = event_flags;
    } else if (e == MOINK_EVT_STORE_DONE) {
        assert(!s_display_read_window); store_events++;
    } else if (e == MOINK_EVT_DISPLAY_DONE) {
        assert(!s_display_read_window); display_events++;
    }
}
static moink_settings_t cfg;
const moink_settings_t *settings_get(void) { return &cfg; }
static int power_battery_mv(void) { return 3870; }
static int64_t esp_timer_get_time(void) { return 2000000; }
static uint32_t esp_get_free_heap_size(void) { return 40000; }
static uint8_t s_wake_reason;
static void power_activity(void) {}
static void transfer_reset(void) {}
static void power_set_busy(bool busy) { if (!busy) assert(!s_display_read_window); }
static bool cache_has(void) { return true; }
static int count = 3, reads, queued, current_writes, cache_writes;
static uint32_t revision = 9;
static atomic_bool block_read, read_entered, release_read, close_started, close_done;
static void pause_ms(void) { struct timespec t = {0, 1000000}; nanosleep(&t, NULL); }
static void wait_true(atomic_bool *flag) {
    for (int i = 0; i < 3000 && !atomic_load(flag); ++i) pause_ms();
    assert(atomic_load(flag));
}
int store_slots(void) { return 8; }
void store_meta_get(int *c, int *cur) { if (c) *c=count; if (cur) *cur=0; }
uint32_t store_revision(void) { return revision; }
esp_err_t store_info(int slot, store_info_t *i) {
    if (slot < 0 || slot >= count) return ESP_ERR_NOT_FOUND;
    *i=(store_info_t){768,552,0x1234,105984,1,192,138,6624,0x5678}; return ESP_OK;
}
esp_err_t store_read(int slot, bool thumb, uint32_t off, void *out, size_t n) {
    (void)slot; (void)thumb; (void)off; assert(lock_depth == 1); reads++;
    if (atomic_load(&block_read)) {
        atomic_store(&read_entered, true); wait_true(&release_read);
    }
    memset(out, 0xa5, n); return ESP_OK;
}
static int xQueueSend(void *q, const ble_job_t *job, int wait) {
    (void)q; (void)job; (void)wait; assert(!s_display_read_window); queued++; return pdTRUE;
}
static bool fail_store, fail_load, fail_display;
static uint8_t frame[32];
uint8_t *ble_api_frame_buf(void) { return frame; }
const uint8_t *ble_api_thumb_buf(void) { return NULL; }
esp_err_t store_save_frame(const uint8_t *data, uint16_t w, uint16_t h,
    uint16_t crc, const uint8_t *thumb, uint32_t len, uint8_t format,
    uint16_t tw, uint16_t th, int *slot) {
    (void)data; (void)w; (void)h; (void)crc; (void)thumb;
    (void)len; (void)format; (void)tw; (void)th;
    assert(!s_display_read_window); *slot=0; return fail_store ? ESP_FAIL : ESP_OK;
}
esp_err_t store_load_frame(int slot, uint8_t *out, size_t cap, uint16_t *w, uint16_t *h) {
    (void)slot; (void)out; (void)cap; assert(!s_display_read_window);
    *w=16; *h=8; return fail_load ? ESP_FAIL : ESP_OK;
}
static esp_err_t cache_load(uint8_t *out, uint32_t cap, uint16_t *w, uint16_t *h, uint16_t *crc) {
    *crc=0; return store_load_frame(0, out, cap, w, h);
}
esp_err_t store_set_current(int slot) {
    (void)slot; assert(!s_display_read_window); current_writes++; return ESP_OK;
}
static esp_err_t cache_save(const uint8_t *data, uint16_t w, uint16_t h, uint16_t crc) {
    (void)data; (void)w; (void)h; (void)crc;
    assert(!s_display_read_window); cache_writes++; return ESP_OK;
}
esp_err_t store_delete(int slot) { (void)slot; assert(!s_display_read_window); return ESP_OK; }
esp_err_t store_clear(void) { assert(!s_display_read_window); return ESP_OK; }
static int display_calls, sleep_calls;
static int epd_display_2bpp_wh(const uint8_t *data, uint16_t w, uint16_t h) {
    (void)data; (void)w; (void)h; assert(s_display_read_window);
    display_calls++; return fail_display ? -1 : 0;
}
static int epd_clear_cycles(int cycles) { return epd_display_2bpp_wh(frame, cycles, 0); }
static void epd_panel_deep_sleep(void) { assert(s_display_read_window); sleep_calls++; }
static ble_job_t pending_job;
static bool job_taken;
static jmp_buf display_exit;
bool ble_api_job_wait(ble_job_t *job, uint32_t timeout) {
    (void)timeout; if (job_taken) longjmp(display_exit, 1);
    job_taken=true; *job=pending_job; return true;
}
'''
defines = '\n'.join(line for line in ble.splitlines() if re.match(
    r'#define (?:OP_|ST_|EVT_|MAX_ATT_MTU|MAX_ATTRIBUTE_VALUE|MAX_GALLERY_READ)', line))
functions = '\n\n'.join(function(ble, n) for n in [
    'rd16', 'rd32', 'wr16', 'wr32', 'respond', 'gallery_read_flags',
    'gallery_read_allowed', 'ble_api_set_display_read_window', 'store_status',
    'queue_job', 'queue_carousel', 'handle_store_list', 'handle_store_read', 'ble_api_job_complete'])
commands = r'''
static void command(uint8_t op, const uint8_t *a, uint16_t alen) {
    uint8_t seq=7; switch (op) {
'''+ '\n'.join(command_case(label) for label in [
    'OP_GET_STATUS', 'OP_STORE_INFO', 'OP_CACHE_SHOW', 'OP_STORE_SHOW',
    'OP_STORE_DELETE', 'OP_GHOST_CLEAR']) + r'''
    case OP_STORE_LIST: handle_store_list(seq, a, alen); break;
    case OP_STORE_READ: handle_store_read(seq, a, alen); break;
    default: assert(0);
    }
}
static void host_command(uint8_t op, const uint8_t *a, uint16_t n) {
    xSemaphoreTake(s_lock, portMAX_DELAY); command(op,a,n); xSemaphoreGive(s_lock);
}
'''
display = '\n\n'.join(function(main_source, n) for n in [
    'storage_status', 'display_with_gallery_reads', 'clear_with_gallery_reads', 'display_task'])
checks = r'''
static uint8_t request[12];
static void *reader(void *arg) {
    (void)arg; host_command(OP_STORE_READ,request,12); return NULL;
}
static void *closer(void *arg) {
    (void)arg; atomic_store(&close_started,true);
    ble_api_set_display_read_window(false,false); atomic_store(&close_done,true); return NULL;
}
static void run_job(uint8_t type, bool save, bool only, bool error_store, bool error_load, bool error_display) {
    s_state = type==BLE_JOB_CLEAR ? BLE_ST_CLEARING : BLE_ST_REFRESHING;
    pending_job=(ble_job_t){.type=type,.w=16,.h=8,.save_store=save,.save_cache=1,.no_show=only,.cycles=2};
    fail_store=error_store; fail_load=error_load; fail_display=error_display;
    display_calls=sleep_calls=current_writes=cache_writes=0;
    state_events=store_events=display_events=0; job_taken=false;
    if (setjmp(display_exit)==0) display_task(NULL);
    assert(s_state==BLE_ST_IDLE && !s_display_read_window && !s_display_append_committed && lock_depth==0);
    bool shown=type!=BLE_JOB_STORE_DELETE && type!=BLE_JOB_STORE_CLEAR &&
        !only && !error_store && !error_load;
    assert(display_calls==(shown?1:0));
    assert(state_events==(shown?2:0));
    if (shown) assert(opening_flags==((type==BLE_JOB_FRAME&&save)?7:3));
    assert(sleep_calls==(shown&&!error_display?1:0));
    if (shown&&!error_display) assert(current_writes==1);
    else assert(current_writes==0 && cache_writes==0);
    assert(store_events==((save||type==BLE_JOB_STORE_DELETE||type==BLE_JOB_STORE_CLEAR)?1:0));
    assert(display_events==((only||type==BLE_JOB_STORE_DELETE||type==BLE_JOB_STORE_CLEAR)?0:1));
}
int main(void) {
    uint8_t list[2]={0,1}, slot=0;
    wr16(request+6,65535); wr32(request+8,revision);
    const uint8_t states[]={0,1,2,3,4,6};
    for (unsigned i=0;i<sizeof(states);i++) {
        s_state=states[i]; s_display_read_window=false; state_events=0;
        host_command(OP_STORE_INFO,NULL,0); assert(wire[2]==(s_state==0?ST_OK:ST_BUSY));
        host_command(OP_STORE_LIST,list,2); assert(wire[2]==(s_state==0?ST_OK:ST_BUSY));
        host_command(OP_STORE_READ,request,12); assert(wire[2]==(s_state==0?ST_OK:ST_BUSY));
        ble_api_set_display_read_window(true,false);
        bool active=s_state==3||s_state==4;
        assert(s_display_read_window==active && state_events==(active?1:0));
        host_command(OP_GET_STATUS,NULL,0);
        assert(wire[2]==ST_OK && wire_len==3+27+strlen(MOINK_VERSION)+1);
        assert(!memcmp(wire+3+27,MOINK_VERSION,strlen(MOINK_VERSION)));
        assert(wire[wire_len-1]==(active?3:1));
        if (active) {
            assert(event_arg==s_state && event_flags==3);
            ble_api_set_display_read_window(true,false); assert(state_events==1);
            host_command(OP_STORE_INFO,NULL,0); assert(wire[2]==ST_OK);
            host_command(OP_STORE_LIST,list,2); assert(wire[2]==ST_OK);
            host_command(OP_STORE_READ,request,12); assert(wire[2]==ST_OK);
            const uint8_t writes[]={OP_CACHE_SHOW,OP_STORE_SHOW,OP_STORE_DELETE,OP_STORE_CLEAR,OP_GHOST_CLEAR};
            int before=queued;
            for (unsigned k=0;k<sizeof(writes);k++) {
                host_command(writes[k],&slot,1); assert(wire[2]==ST_BUSY);
            }
            assert(queued==before);
            ble_api_set_display_read_window(true,true);
            assert(state_events==2 && event_flags==7 && event_arg==s_state);
            host_command(OP_GET_STATUS,NULL,0); assert(wire[wire_len-1]==7);
            ble_api_set_display_read_window(false,false);
            assert(state_events==3 && event_flags==1 && event_arg==s_state);
            assert(!s_display_append_committed);
            host_command(OP_STORE_READ,request,12); assert(wire[2]==ST_BUSY);
        }
    }
    puts("PASS: production INFO/LIST/READ gates, status flags, window events, modifying-job BUSY");
    s_state=BLE_ST_IDLE;
    const uint16_t mtus[]={23,34,64,185,247,515,516,517};
    for (unsigned k=0;k<sizeof(mtus)/sizeof(mtus[0]);k++) {
        mtu=mtus[k];
        for (int thumb=0;thumb<2;thumb++) {
            request[1]=thumb; host_command(OP_STORE_READ,request,12);
            unsigned cap=mtu-19; if(cap>496)cap=496;
            assert(wire[2]==ST_OK && wire_len==3+13+cap && wire_len<=512);
        }
    }
    uint8_t payload[510]={0}; mtu=517;
    respond(1,ST_OK,payload,509); assert(wire_len==512&&wire[2]==ST_OK);
    respond(1,ST_OK,payload,510); assert(wire_len==3&&wire[2]==ST_UNSUPPORTED);
    puts("PASS: production READ caps and real RSP encoding at 8 MTUs / 512-byte boundary");
    s_state=BLE_ST_REFRESHING; ble_api_set_display_read_window(true,true);
    atomic_store(&block_read,true); pthread_t r,c;
    assert(pthread_create(&r,NULL,reader,NULL)==0); wait_true(&read_entered);
    assert(pthread_create(&c,NULL,closer,NULL)==0); wait_true(&close_started);
    for(int i=0;i<20;i++) pause_ms();
    assert(!atomic_load(&close_done));
    atomic_store(&release_read,true); assert(pthread_join(r,NULL)==0); assert(pthread_join(c,NULL)==0);
    assert(atomic_load(&close_done)&&!s_display_read_window&&!s_display_append_committed); atomic_store(&block_read,false);
    puts("PASS: real pthread host mutex drains an in-flight read before window close returns");
    ble_api_set_display_read_window(true,true);
    assert(s_display_read_window&&s_display_append_committed);
    ble_api_job_complete();
    assert(s_state==BLE_ST_IDLE&&!s_display_read_window&&!s_display_append_committed);
    cfg.cache_on=1;
    run_job(BLE_JOB_FRAME,true,false,false,false,false);
    run_job(BLE_JOB_FRAME,false,false,false,false,false);
    run_job(BLE_JOB_FRAME,true,true,false,false,false);
    run_job(BLE_JOB_FRAME,true,false,true,false,false);
    run_job(BLE_JOB_FRAME,true,false,false,false,true);
    run_job(BLE_JOB_STORE_SHOW,false,false,false,false,false);
    run_job(BLE_JOB_STORE_SHOW,false,false,false,true,false);
    run_job(BLE_JOB_STORE_SHOW,false,false,false,false,true);
    run_job(BLE_JOB_CACHE,false,false,false,false,false);
    run_job(BLE_JOB_CACHE,false,false,false,true,false);
    run_job(BLE_JOB_CACHE,false,false,false,false,true);
    run_job(BLE_JOB_CLEAR,false,false,false,false,false);
    run_job(BLE_JOB_CLEAR,false,false,false,false,true);
    run_job(BLE_JOB_STORE_DELETE,false,false,false,false,false);
    run_job(BLE_JOB_STORE_CLEAR,false,false,false,false,false);
    puts("PASS: 15 actual display_task paths keep load/save/current/cache writes outside the EPD window");
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="moink-display-read-") as temp:
    test = Path(temp) / "display_read.c"
    binary = Path(temp) / "display_read"
    test.write_text(prefix + defines + '\n' + functions + commands + display + checks)
    subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-pthread", "-I", str(ROOT / "src"), "-I", str(ROOT / "tests/store_stubs"),
                    str(test), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=20)
