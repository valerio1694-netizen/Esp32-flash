/*
 * Eufy RoboVac 15C, GD32F103VCT6, stock MCU application 1.1.8.
 * Read-only SWD/AHB-AP monitor; NO target halts, reset or target memory writes.
 * Internal DP/AP configuration writes are required for SWD memory reads.
 * ESP32 CMSIS-DAP server task is sole owner of SWD; HTTP task only serves snapshots.
 * Source: verified static OEM disassembly, see DIAGNOSE-ENCODER.md.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <esp_timer.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "DAP.h"
#include "eufy_monitor.h"

#define EUFY_HTTP_PORT 8080
#define EUFY_POLL_MS 250U
#define EUFY_RETRY_MS 2000U
#define DP_ID_EXPECTED 0x1ba01477U

typedef struct {
    bool valid;
    bool host_connected;
    uint32_t dpidr;
    uint32_t errors;
    int64_t updated_us;
    int32_t left_ticks, right_ticks;
    uint16_t left_window, right_window;
    uint16_t left_last_window, right_last_window;
    uint16_t left_current_adc, right_current_adc;
    uint16_t battery_adc, charge_adc;
    uint8_t battery_percent_reported, robot_status, run_model;
    uint16_t obstacle_l, obstacle_f, obstacle_r;
    uint16_t cliff_l, cliff_f, cliff_r;
    uint16_t brush_main_adc, brush_l_adc, brush_r_adc;
    uint32_t pwm_l_pos, pwm_l_neg, pwm_r_pos, pwm_r_neg, brush_pwm;
    char status[36];
} eufy_data_t;
static eufy_data_t latest = { .status = "initializing" };
static portMUX_TYPE latest_mux = portMUX_INITIALIZER_UNLOCKED;
static bool swd_ready = false;
static int64_t next_poll_us = 0;
static bool external_host = false;

static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void put32(uint8_t *p, uint32_t v) {
    for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i));
}
static bool dap_simple(const uint8_t *req, uint8_t id) {
    uint8_t resp[24] = {0};
    uint32_t n = DAP_ProcessCommand(req, resp) & 0xffffU;
    return n >= 2 && resp[0] == id && resp[1] == 0;
}
static bool dpap(uint8_t reg, bool read, uint32_t *word) {
    uint8_t req[8] = {ID_DAP_Transfer, 0, 1, reg, 0, 0, 0, 0};
    uint8_t resp[16] = {0};
    if (!read) put32(&req[4], *word);
    uint32_t size = DAP_ProcessCommand(req, resp) & 0xffffU;
    if (resp[0] != ID_DAP_Transfer || resp[1] != 1 ||
        resp[2] != DAP_TRANSFER_OK || size != (read ? 7U : 3U))
        return false;
    if (read) *word = le32(&resp[3]);
    return true;
}
static bool dp_write(uint8_t offset, uint32_t value) {
    return dpap(offset, false, &value);
}
static bool dp_read(uint8_t offset, uint32_t *value) {
    return dpap(offset | DAP_TRANSFER_RnW, true, value);
}
static bool ap_write(uint8_t offset, uint32_t value) {
    return dpap(offset | DAP_TRANSFER_APnDP, false, &value);
}

static bool swd_init(uint32_t *dpidr) {
    const uint8_t conn[] = {ID_DAP_Connect, DAP_PORT_SWD};
    uint8_t reply[16] = {0};
    (void)DAP_ProcessCommand(conn, reply);
    if (reply[0] != ID_DAP_Connect || reply[1] != DAP_PORT_SWD) return false;

    // 100 kHz, no SWD reset of the target CPU.
    const uint8_t clock[] = {ID_DAP_SWJ_Clock, 0xA0, 0x86, 0x01, 0x00};
    if (!dap_simple(clock, ID_DAP_SWJ_Clock)) return false;
    const uint8_t config[] = {ID_DAP_TransferConfigure, 0, 100, 0, 0, 0};
    if (!dap_simple(config, ID_DAP_TransferConfigure)) return false;
    const uint8_t swd_conf[] = {ID_DAP_SWD_Configure, 0};
    if (!dap_simple(swd_conf, ID_DAP_SWD_Configure)) return false;

    // ARM SWJ line reset -> JTAG-to-SWD (0xE79E LSB first) -> reset -> idle.
    const uint8_t reset1[] = {ID_DAP_SWJ_Sequence, 56, 255,255,255,255,255,255,255};
    const uint8_t switch_swd[] = {ID_DAP_SWJ_Sequence, 16, 0x9e, 0xe7};
    const uint8_t reset2[] = {ID_DAP_SWJ_Sequence, 56, 255,255,255,255,255,255,255};
    const uint8_t idle[] = {ID_DAP_SWJ_Sequence, 8, 0};
    if (!dap_simple(reset1, ID_DAP_SWJ_Sequence) ||
        !dap_simple(switch_swd, ID_DAP_SWJ_Sequence) ||
        !dap_simple(reset2, ID_DAP_SWJ_Sequence) ||
        !dap_simple(idle, ID_DAP_SWJ_Sequence)) return false;

    if (!dp_read(0x00, dpidr) || *dpidr != DP_ID_EXPECTED) return false;
    // Clear debug-port sticky fault bits, request debug/system power.
    if (!dp_write(0x00, 0x1eU) ||
        !dp_write(0x04, 0x50000000U)) return false;
    bool powered = false;
    for (int i = 0; i < 20; ++i) {
        uint32_t ctrl = 0;
        if (dp_read(0x04, &ctrl) && (ctrl & 0xA0000000U) == 0xA0000000U) {
            powered = true;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    if (!powered || !dp_write(0x08, 0)) return false;
    // MEM-AP CSW: HPROT=0x23, 32-bit transfer, single auto-increment.
    return ap_write(0x00, 0x23000052U);
}

static bool read_words(uint32_t address, uint16_t count, uint32_t *dst) {
    if (count == 0 || count > 8 || !ap_write(0x04, address)) return false;
    uint8_t cmd[] = {ID_DAP_TransferBlock, 0, (uint8_t)count,
                     (uint8_t)(count >> 8), 0x0f}; // AP DRW read
    uint8_t resp[40] = {0};
    uint32_t n = DAP_ProcessCommand(cmd, resp) & 0xffffU;
    if (n != 4U + 4U * count || resp[0] != ID_DAP_TransferBlock ||
        resp[1] != (uint8_t)count || resp[2] != 0 ||
        resp[3] != DAP_TRANSFER_OK) return false;
    for (unsigned i = 0; i < count; ++i)
        dst[i] = le32(&resp[4 + i * 4]);
    return true;
}
static void status_set(const char *state, bool invalidate) {
    portENTER_CRITICAL(&latest_mux);
    if (invalidate) latest.valid = false;
    latest.errors++;
    snprintf(latest.status, sizeof(latest.status), "%s", state);
    portEXIT_CRITICAL(&latest_mux);
}
void eufy_monitor_client_changed(bool connected) {
    external_host = connected;  // Only called by CMSIS-DAP owner task.
    swd_ready = false;
    next_poll_us = 0;
    portENTER_CRITICAL(&latest_mux);
    latest.host_connected = connected;
    latest.valid = false;
    snprintf(latest.status, sizeof(latest.status), "%s",
             connected ? "paused_for_openocd" : "reconnecting");
    portEXIT_CRITICAL(&latest_mux);
}

void eufy_monitor_poll(void) {
    if (external_host) return;
    int64_t now = esp_timer_get_time();
    if (now < next_poll_us) return;
    next_poll_us = now + EUFY_POLL_MS * 1000LL;

    uint32_t dpidr = 0;
    if (!swd_ready) {
        if (!swd_init(&dpidr)) {
            status_set("swd_init_failed", true);
            next_poll_us = now + EUFY_RETRY_MS * 1000LL;
            return;
        }
        swd_ready = true;
    }

    uint32_t wheels[4], windows[1], analog[8], status_word[1], battery_state[1], pwm[4], brush[1];
    if (!read_words(0x20000098U, 4, wheels) ||
        !read_words(0x20000138U, 1, windows) ||
        !read_words(0x200006B4U, 8, analog) ||
        !read_words(0x20000394U, 1, status_word) ||
        !read_words(0x200001D8U, 1, battery_state) ||
        !read_words(0x40000434U, 4, pwm) ||
        !read_words(0x40012C38U, 1, brush)) {
        swd_ready = false;
        status_set("read_failed", true);
        next_poll_us = now + EUFY_RETRY_MS * 1000LL;
        return;
    }
    portENTER_CRITICAL(&latest_mux);
    latest.valid = true;
    latest.dpidr = DP_ID_EXPECTED;
    latest.updated_us = esp_timer_get_time();
    latest.left_window = wheels[0] & 0xffffU;
    latest.right_window = wheels[0] >> 16;
    latest.left_ticks = (int32_t)wheels[2];
    latest.right_ticks = (int32_t)wheels[3];
    latest.right_last_window = windows[0] & 0xffffU;
    latest.left_last_window = windows[0] >> 16;
    latest.right_current_adc = analog[0] & 0xffffU;
    latest.left_current_adc = analog[2] >> 16;
    latest.cliff_l = analog[0] >> 16;  // 0x200006B6
    latest.cliff_r = analog[1] & 0xffffU; // 0x200006B8
    latest.brush_r_adc = analog[1] >> 16; // 0x200006BA
    latest.brush_main_adc = analog[2] & 0xffffU; // 0x200006BC
    latest.battery_adc = analog[3] & 0xffffU; // 0x200006C0
    latest.cliff_f = analog[3] >> 16; // 0x200006C2
    latest.obstacle_l = analog[4] & 0xffffU; // 0x200006C4
    latest.obstacle_f = analog[4] >> 16; // 0x200006C6
    latest.obstacle_r = analog[5] & 0xffffU; // 0x200006C8
    latest.brush_l_adc = analog[5] >> 16; // 0x200006CA
    latest.charge_adc = analog[6] & 0xffffU; // 0x200006CC
    latest.robot_status = (status_word[0] >> 8) & 0xffU; // 0x20000395
    latest.run_model = (status_word[0] >> 16) & 0xffU; // 0x20000396
    latest.battery_percent_reported = (battery_state[0] >> 16) & 0xffU; // 0x200001DA
    latest.pwm_l_pos = pwm[0]; latest.pwm_l_neg = pwm[1];
    latest.pwm_r_pos = pwm[2]; latest.pwm_r_neg = pwm[3];
    latest.brush_pwm = brush[0];
    strcpy(latest.status, "ok");
    portEXIT_CRITICAL(&latest_mux);
}

static void eufy_http_task(void *arg) {
    (void)arg;
    int listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener < 0) { vTaskDelete(NULL); return; }
    int opt = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(EUFY_HTTP_PORT),
        .sin_addr.s_addr = INADDR_ANY,
    };
    if (bind(listener, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
        listen(listener, 3) < 0) {
        printf("eufy monitor: HTTP bind/listen failed (%d)\n", errno);
        close(listener); vTaskDelete(NULL); return;
    }
    printf("eufy monitor: http://<esp-ip>:%d/telemetry\n", EUFY_HTTP_PORT);
    while (true) {
        int fd = accept(listener, NULL, NULL);
        if (fd < 0) { vTaskDelay(pdMS_TO_TICKS(100)); continue; }
        struct timeval tv = { .tv_sec=1, .tv_usec=0 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        char req[192] = {0};
        int rn = recv(fd, req, sizeof(req)-1, 0);
        if (rn <= 0 || (strncmp(req, "GET /telemetry ", 15) != 0 &&
                        strncmp(req, "GET / ", 6) != 0)) {
            const char *error = "HTTP/1.0 404 Not Found\r\nContent-Length: 0\r\n\r\n";
            send(fd, error, strlen(error), 0);
            close(fd); continue;
        }
        eufy_data_t s;
        portENTER_CRITICAL(&latest_mux);
        s = latest;
        portEXIT_CRITICAL(&latest_mux);
        int64_t age_ms = s.updated_us
            ? (esp_timer_get_time() - s.updated_us) / 1000LL : -1;
        char payload[1300];
        int pn = snprintf(payload, sizeof(payload),
            "{\"status\":\"%s\",\"valid\":%s,\"openocd_connected\":%s,"
            "\"sample_age_ms\":%lld,\"errors\":%lu,\"dpidr\":\"0x%08lx\","
            "\"wheel\":{\"left_ticks\":%ld,\"right_ticks\":%ld,"
            "\"left_window\":%u,\"right_window\":%u,"
            "\"left_last_window\":%u,\"right_last_window\":%u,"
            "\"left_current_adc\":%u,\"right_current_adc\":%u},"
            "\"pwm_raw\":{\"left_positive\":%lu,\"left_negative\":%lu,"
            "\"right_positive\":%lu,\"right_negative\":%lu,\"brush\":%lu},"
            "\"battery\":{\"adc\":%u,\"charge_adc\":%u,\"percent_reported\":%u},"
            "\"state\":{\"robot_status\":%u,\"run_model\":%u},"
            "\"obstacle_adc\":{\"left\":%u,\"front\":%u,\"right\":%u},"
            "\"cliff_adc\":{\"left\":%u,\"front\":%u,\"right\":%u},"
            "\"brush_adc\":{\"main\":%u,\"left\":%u,\"right\":%u},"
            "\"units\":\"ticks_and_raw_register_values\","
            "\"mode\":\"read_only_swd\"}\n",
            s.status, s.valid ? "true" : "false",
            s.host_connected ? "true" : "false",
            (long long)age_ms, (unsigned long)s.errors, (unsigned long)s.dpidr,
            (long)s.left_ticks, (long)s.right_ticks, s.left_window, s.right_window,
            s.left_last_window, s.right_last_window,
            s.left_current_adc, s.right_current_adc,
            (unsigned long)s.pwm_l_pos, (unsigned long)s.pwm_l_neg,
            (unsigned long)s.pwm_r_pos, (unsigned long)s.pwm_r_neg,
            (unsigned long)s.brush_pwm,
            s.battery_adc, s.charge_adc, s.battery_percent_reported,
            s.robot_status, s.run_model,
            s.obstacle_l, s.obstacle_f, s.obstacle_r,
            s.cliff_l, s.cliff_f, s.cliff_r,
            s.brush_main_adc, s.brush_l_adc, s.brush_r_adc);
        if (pn > 0 && pn < sizeof(payload)) {
            char head[180];
            int hn = snprintf(head, sizeof(head),
                "HTTP/1.0 200 OK\r\nContent-Type: application/json\r\n"
                "Access-Control-Allow-Origin: *\r\nContent-Length: %d\r\n"
                "Connection: close\r\n\r\n", pn);
            if (hn > 0) send(fd, head, hn, 0);
            send(fd, payload, pn, 0);
        }
        close(fd);
    }
}
void eufy_monitor_server_start(void) {
    if (xTaskCreate(eufy_http_task, "eufy_http", 4096, NULL, 3, NULL) != pdPASS)
        printf("eufy monitor: HTTP task creation failed\n");
}
