/* Boot stall trace (CONFIG_EMBPET_BOOT_TRACE, test builds only; embpet-s2j.5,
 * tools/boot_trace.py). Started from the soak generator, records until uptime TRACE_END_S:
 *  - gap5 / gap24   a priority-5 / priority-24 task that sleeps one tick woke late
 *                   (gap24 = interrupts or the scheduler were off, not just a busy task)
 *  - timer          a 10 ms periodic esp_timer callback came late (the LED timer and the
 *                   `input` event drain run on the same esp_timer task)
 *  - cpu            a task used more than CPU_REC_US of a ~50 ms window (run-time counters)
 *  - phy, pbkdf2, erase, write, tls, wifi_init, wifi_start: start and duration of
 *                   register_chipv7_phy (PHY calibration), the WPA PMK derivation, flash
 *                   erase/write, mbedtls_ssl_handshake and the Wi-Fi driver calls (linker
 *                   --wrap, proto CMakeLists.txt)
 *  - wifi_evt / ip_evt  Wi-Fi and IP events (dur = event id)
 * `trace [<from>]` returns up to PAGE records from index from. */
#include <inttypes.h>
#include <string.h>

#include "argtable3/argtable3.h"
#include "console.h"
#include "console_priv.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define REC_MAX      256
#define PAGE         20
#define TRACE_END_S  20
#define GAP5_REC_US  25000
#define GAP24_REC_US 15000
#define TIMER_REC_US 20000
#define CPU_WINDOW   5              /* ticks of the priority-24 sampler */
#define CPU_REC_US   20000
#define MAX_TASKS    24

enum { K_GAP5, K_GAP24, K_TIMER, K_CPU, K_PHY, K_PBKDF2, K_ERASE, K_WRITE, K_TLS,
       K_WIFI_INIT, K_WIFI_START, K_WIFI_EVT, K_IP_EVT, K_SAE_PT, K_SAE_COMMIT_PT,
       K_SAE_COMMIT, K_SAE_PROCESS };
static const char *const KIND[] = {"gap5", "gap24", "timer", "cpu", "phy", "pbkdf2", "erase",
                                   "write", "tls", "wifi_init", "wifi_start", "wifi_evt",
                                   "ip_evt", "sae_pt", "sae_cmt_pt", "sae_commit",
                                   "sae_proc"};

typedef struct {
    int64_t t_us;
    int32_t dur_us;
    uint8_t kind;
    char who[configMAX_TASK_NAME_LEN];
} rec_t;

static rec_t s_rec[REC_MAX];
static unsigned s_n;
static unsigned s_lost;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

static void rec(uint8_t kind, int64_t t_us, int64_t dur_us, const char *who)
{
    if (t_us >= TRACE_END_S * 1000000LL) {
        return;
    }
    portENTER_CRITICAL(&s_mux);
    if (s_n < REC_MAX) {
        rec_t *r = &s_rec[s_n++];
        r->t_us = t_us;
        r->dur_us = dur_us > INT32_MAX ? INT32_MAX : (int32_t)dur_us;
        r->kind = kind;
        strlcpy(r->who, who != NULL ? who : "", sizeof(r->who));
    } else {
        s_lost++;
    }
    portEXIT_CRITICAL(&s_mux);
}

static const char *me(void)
{
    return pcTaskGetName(NULL);
}

/* Linker wraps. Prototypes use void * for the IDF-private types. */
int __real_register_chipv7_phy(const void *init, void *cal, int mode);
int __wrap_register_chipv7_phy(const void *init, void *cal, int mode);
int __wrap_register_chipv7_phy(const void *init, void *cal, int mode)
{
    int64_t t0 = esp_timer_get_time();
    int r = __real_register_chipv7_phy(init, cal, mode);
    rec(K_PHY, t0, esp_timer_get_time() - t0, me());
    return r;
}

int __real_pbkdf2_sha1(const char *pass, const uint8_t *ssid, size_t ssid_len, int iter,
                       uint8_t *buf, size_t buflen);
int __wrap_pbkdf2_sha1(const char *pass, const uint8_t *ssid, size_t ssid_len, int iter,
                       uint8_t *buf, size_t buflen);
int __wrap_pbkdf2_sha1(const char *pass, const uint8_t *ssid, size_t ssid_len, int iter,
                       uint8_t *buf, size_t buflen)
{
    int64_t t0 = esp_timer_get_time();
    int r = __real_pbkdf2_sha1(pass, ssid, ssid_len, iter, buf, buflen);
    rec(K_PBKDF2, t0, esp_timer_get_time() - t0, me());
    return r;
}

/* WPA3-SAE (src/common/sae.c, called from esp_wpa3.c). */
void *__real_sae_derive_pt(int *groups, const uint8_t *ssid, size_t ssid_len,
                           const uint8_t *pw, size_t pw_len, const char *id);
void *__wrap_sae_derive_pt(int *groups, const uint8_t *ssid, size_t ssid_len,
                           const uint8_t *pw, size_t pw_len, const char *id);
void *__wrap_sae_derive_pt(int *groups, const uint8_t *ssid, size_t ssid_len,
                           const uint8_t *pw, size_t pw_len, const char *id)
{
    int64_t t0 = esp_timer_get_time();
    void *r = __real_sae_derive_pt(groups, ssid, ssid_len, pw, pw_len, id);
    rec(K_SAE_PT, t0, esp_timer_get_time() - t0, me());
    return r;
}

int __real_sae_prepare_commit_pt(void *sae, const void *pt, const uint8_t *a1,
                                 const uint8_t *a2, int *rejected, const void *pk);
int __wrap_sae_prepare_commit_pt(void *sae, const void *pt, const uint8_t *a1,
                                 const uint8_t *a2, int *rejected, const void *pk);
int __wrap_sae_prepare_commit_pt(void *sae, const void *pt, const uint8_t *a1,
                                 const uint8_t *a2, int *rejected, const void *pk)
{
    int64_t t0 = esp_timer_get_time();
    int r = __real_sae_prepare_commit_pt(sae, pt, a1, a2, rejected, pk);
    rec(K_SAE_COMMIT_PT, t0, esp_timer_get_time() - t0, me());
    return r;
}

int __real_sae_prepare_commit(const uint8_t *a1, const uint8_t *a2, const uint8_t *pw,
                              size_t pw_len, void *sae);
int __wrap_sae_prepare_commit(const uint8_t *a1, const uint8_t *a2, const uint8_t *pw,
                              size_t pw_len, void *sae);
int __wrap_sae_prepare_commit(const uint8_t *a1, const uint8_t *a2, const uint8_t *pw,
                              size_t pw_len, void *sae)
{
    int64_t t0 = esp_timer_get_time();
    int r = __real_sae_prepare_commit(a1, a2, pw, pw_len, sae);
    rec(K_SAE_COMMIT, t0, esp_timer_get_time() - t0, me());
    return r;
}

int __real_sae_process_commit(void *sae);
int __wrap_sae_process_commit(void *sae);
int __wrap_sae_process_commit(void *sae)
{
    int64_t t0 = esp_timer_get_time();
    int r = __real_sae_process_commit(sae);
    rec(K_SAE_PROCESS, t0, esp_timer_get_time() - t0, me());
    return r;
}

/* The flash wrappers live in flash, not IRAM, so they are only safe for task-context callers
 * (NVS, the Wi-Fi driver); a caller with the cache disabled (panic, coredump, IRAM-safe ISR)
 * would crash here. Test-only, and the trace stops at uptime 20 s. */
esp_err_t __real_esp_flash_erase_region(void *chip, uint32_t start, uint32_t len);
esp_err_t __wrap_esp_flash_erase_region(void *chip, uint32_t start, uint32_t len);
esp_err_t __wrap_esp_flash_erase_region(void *chip, uint32_t start, uint32_t len)
{
    int64_t t0 = esp_timer_get_time();
    esp_err_t r = __real_esp_flash_erase_region(chip, start, len);
    rec(K_ERASE, t0, esp_timer_get_time() - t0, me());
    return r;
}

esp_err_t __real_esp_flash_write(void *chip, const void *buf, uint32_t addr, uint32_t len);
esp_err_t __wrap_esp_flash_write(void *chip, const void *buf, uint32_t addr, uint32_t len);
esp_err_t __wrap_esp_flash_write(void *chip, const void *buf, uint32_t addr, uint32_t len)
{
    int64_t t0 = esp_timer_get_time();
    esp_err_t r = __real_esp_flash_write(chip, buf, addr, len);
    rec(K_WRITE, t0, esp_timer_get_time() - t0, me());
    return r;
}

int __real_mbedtls_ssl_handshake(void *ssl);
int __wrap_mbedtls_ssl_handshake(void *ssl);
int __wrap_mbedtls_ssl_handshake(void *ssl)
{
    int64_t t0 = esp_timer_get_time();
    int r = __real_mbedtls_ssl_handshake(ssl);
    rec(K_TLS, t0, esp_timer_get_time() - t0, me());
    return r;
}

esp_err_t __real_esp_wifi_init(const void *cfg);
esp_err_t __wrap_esp_wifi_init(const void *cfg);
esp_err_t __wrap_esp_wifi_init(const void *cfg)
{
    int64_t t0 = esp_timer_get_time();
    esp_err_t r = __real_esp_wifi_init(cfg);
    rec(K_WIFI_INIT, t0, esp_timer_get_time() - t0, me());
    return r;
}

esp_err_t __real_esp_wifi_start(void);
esp_err_t __wrap_esp_wifi_start(void);
esp_err_t __wrap_esp_wifi_start(void)
{
    int64_t t0 = esp_timer_get_time();
    esp_err_t r = __real_esp_wifi_start();
    rec(K_WIFI_START, t0, esp_timer_get_time() - t0, me());
    return r;
}

/* Wi-Fi and IP events, recorded when posted (WIFI_EVENT / IP_EVENT are these strings). */
esp_err_t __real_esp_event_post(const char *base, int32_t id, const void *data, size_t size,
                                TickType_t wait);
esp_err_t __wrap_esp_event_post(const char *base, int32_t id, const void *data, size_t size,
                                TickType_t wait);
esp_err_t __wrap_esp_event_post(const char *base, int32_t id, const void *data, size_t size,
                                TickType_t wait)
{
    if (base != NULL && strcmp(base, "WIFI_EVENT") == 0) {
        rec(K_WIFI_EVT, esp_timer_get_time(), id, me());
    } else if (base != NULL && strcmp(base, "IP_EVENT") == 0) {
        rec(K_IP_EVT, esp_timer_get_time(), id, me());
    }
    return __real_esp_event_post(base, id, data, size, wait);
}

static void timer_cb(void *arg)
{
    static int64_t last;
    (void)arg;
    int64_t now = esp_timer_get_time();
    if (last != 0 && now - last > TIMER_REC_US) {
        rec(K_TIMER, last, now - last, NULL);
    }
    last = now;
}

static void gap5_task(void *arg)
{
    (void)arg;
    int64_t last = esp_timer_get_time();
    for (;;) {
        vTaskDelay(1);
        int64_t now = esp_timer_get_time();
        if (now - last > GAP5_REC_US) {
            rec(K_GAP5, last, now - last, NULL);
        }
        last = now;
    }
}

/* CPU per task over the last window: tasks above CPU_REC_US, IDLE left out. */
static void cpu_window(int64_t t0)
{
    static TaskStatus_t a[MAX_TASKS], b[MAX_TASKS];
    static TaskStatus_t *prev = a, *cur = b;
    static UBaseType_t nprev;
    UBaseType_t n = uxTaskGetSystemState(cur, MAX_TASKS, NULL);
    for (UBaseType_t i = 0; i < n; i++) {
        if (strncmp(cur[i].pcTaskName, "IDLE", 4) == 0) {
            continue;
        }
        for (UBaseType_t j = 0; j < nprev; j++) {
            if (prev[j].xHandle == cur[i].xHandle) {
                uint32_t d = cur[i].ulRunTimeCounter - prev[j].ulRunTimeCounter;
                if (d > CPU_REC_US) {
                    rec(K_CPU, t0, d, cur[i].pcTaskName);
                }
                break;
            }
        }
    }
    TaskStatus_t *t = prev;
    prev = cur;
    cur = t;
    nprev = n;
}

static void gap24_task(void *arg)
{
    (void)arg;
    unsigned k = 0;
    int64_t last = esp_timer_get_time();
    int64_t win = last;
    while (esp_timer_get_time() < TRACE_END_S * 1000000LL) {
        vTaskDelay(1);
        int64_t now = esp_timer_get_time();
        if (now - last > GAP24_REC_US) {
            rec(K_GAP24, last, now - last, NULL);
        }
        last = now;
        if (++k % CPU_WINDOW == 0) {
            cpu_window(win);
            win = now;
        }
    }
    vTaskDelete(NULL);
}

static struct arg_int *s_from;
static void *s_args[2];

static void trace_cmd(console_req_t *req)
{
    unsigned from = s_from->count > 0 && s_from->ival[0] > 0 ? (unsigned)s_from->ival[0] : 0;
    cJSON *p = cJSON_CreateObject();
    cJSON *recs = cJSON_AddArrayToObject(p, "recs");
    portENTER_CRITICAL(&s_mux);
    unsigned n = s_n;
    unsigned lost = s_lost;
    portEXIT_CRITICAL(&s_mux);
    for (unsigned i = from; i < n && i < from + PAGE; i++) {
        const rec_t *r = &s_rec[i];
        cJSON *e = cJSON_CreateArray();
        cJSON_AddItemToArray(e, cJSON_CreateNumber((double)r->t_us));
        cJSON_AddItemToArray(e, cJSON_CreateString(KIND[r->kind]));
        cJSON_AddItemToArray(e, cJSON_CreateNumber(r->dur_us));
        cJSON_AddItemToArray(e, cJSON_CreateString(r->who));
        cJSON_AddItemToArray(recs, e);
    }
    cJSON_AddNumberToObject(p, "n", n);
    cJSON_AddNumberToObject(p, "lost", lost);
    console_ok(req, p);
}

static const console_cmd_t s_trace = {
    .name = "trace",
    .help = "Test build: boot stall trace records from <from>",
    .args = s_args,
    .handler = trace_cmd,
};

void boot_trace_start(void)
{
    s_from = arg_int0(NULL, NULL, "<from>", "first record");
    s_args[0] = s_from;
    console_register(&s_trace);
    static esp_timer_handle_t timer;
    const esp_timer_create_args_t a = {.callback = timer_cb, .name = "trace"};
    if (esp_timer_create(&a, &timer) == ESP_OK) {
        esp_timer_start_periodic(timer, 10000);
    }
    xTaskCreate(gap24_task, "trace24", 3072, NULL, configMAX_PRIORITIES - 1, NULL);
    xTaskCreate(gap5_task, "trace5", 2048, NULL, 5, NULL);
}
