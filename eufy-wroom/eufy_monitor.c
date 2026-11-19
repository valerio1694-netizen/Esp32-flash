/*
 * Eufy RoboVac 15C, GD32F103VCT6, stock MCU application 1.1.8.
 * Read-only SWD/AHB-AP monitor; NO target halts, reset or target memory writes.
 * Internal DP/AP configuration writes are required for SWD memory reads.
 * ESP32 CMSIS-DAP server task is sole owner of SWD; HTTP task only serves snapshots.
 * Source: verified static OEM disassembly, see DIAGNOSE-ENCODER.md.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include "esp_http_server.h"
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
    uint32_t swd_clock_hz;
    uint8_t swd_last_ack;
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
    bool diagnostics_valid, gpio_valid;
    uint32_t change_time, total_run_time, wall_count, wall_count_1, fan_pwm;
    uint16_t run_time, run_time_1, stop_time, stop_time_1;
    uint16_t dock_time, work_l, dock_l, fs_raw, diagnostic_213_a, diagnostic_213_b;
    uint8_t wall_flag, fan_level, fan_mode, power_boost, remote_wall_follow;
    uint16_t wall_bump_count, charge_counter, charge_control_state;
    uint8_t charge_activity_flag;
    uint32_t remote_spiral_raw, robot_flags;
    uint16_t gpio_idr[5];
    char status[36];
} eufy_data_t;
static eufy_data_t latest = { .status = "initializing" };
static portMUX_TYPE latest_mux = portMUX_INITIALIZER_UNLOCKED;
static bool swd_ready = false;
static int64_t next_poll_us = 0;
static bool external_host = false;
static uint8_t swd_last_ack = 0;
static uint32_t swd_last_clock = 0;
static uint32_t swd_observed_dpidr = 0;
static const char *swd_failed_step = "swd_not_initialized";

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
    if (size >= 3 && resp[0] == ID_DAP_Transfer)
        swd_last_ack = resp[2];
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

/* No MCU reset or halt. All failures are assigned an observable stage. */
static bool swd_init(uint32_t *dpidr, uint32_t clock_hz) {
    swd_last_ack = 0;
    swd_last_clock = clock_hz;
    swd_observed_dpidr = 0;
    *dpidr = 0;

    swd_failed_step = "swd_connect_failed";
    const uint8_t conn[] = {ID_DAP_Connect, DAP_PORT_SWD};
    uint8_t reply[16] = {0};
    uint32_t length = DAP_ProcessCommand(conn, reply) & 0xffffU;
    if (length < 2 || reply[0] != ID_DAP_Connect || reply[1] != DAP_PORT_SWD)
        return false;

    swd_failed_step = "swd_clock_failed";
    const uint8_t clock[] = {ID_DAP_SWJ_Clock,
        (uint8_t)clock_hz, (uint8_t)(clock_hz >> 8),
        (uint8_t)(clock_hz >> 16), (uint8_t)(clock_hz >> 24)};
    if (!dap_simple(clock, ID_DAP_SWJ_Clock)) return false;

    swd_failed_step = "swd_config_failed";
    const uint8_t config[] = {ID_DAP_TransferConfigure, 0, 100, 0, 0, 0};
    const uint8_t swd_conf[] = {ID_DAP_SWD_Configure, 0};
    if (!dap_simple(config, ID_DAP_TransferConfigure) ||
        !dap_simple(swd_conf, ID_DAP_SWD_Configure)) return false;

    swd_failed_step = "swd_sequence_failed";
    // ARM SWJ line reset -> JTAG-to-SWD (0xE79E LSB first) -> reset -> idle.
    const uint8_t reset1[] = {ID_DAP_SWJ_Sequence, 56, 255,255,255,255,255,255,255};
    const uint8_t switch_swd[] = {ID_DAP_SWJ_Sequence, 16, 0x9e, 0xe7};
    const uint8_t reset2[] = {ID_DAP_SWJ_Sequence, 56, 255,255,255,255,255,255,255};
    const uint8_t idle[] = {ID_DAP_SWJ_Sequence, 8, 0};
    if (!dap_simple(reset1, ID_DAP_SWJ_Sequence) ||
        !dap_simple(switch_swd, ID_DAP_SWJ_Sequence) ||
        !dap_simple(reset2, ID_DAP_SWJ_Sequence) ||
        !dap_simple(idle, ID_DAP_SWJ_Sequence)) return false;

    swd_failed_step = "swd_dpidr_read_failed";
    if (!dp_read(0x00, dpidr)) {
        swd_failed_step = swd_last_ack == DAP_TRANSFER_WAIT ? "swd_dpidr_wait" :
                          swd_last_ack == DAP_TRANSFER_FAULT ? "swd_dpidr_fault" :
                          "swd_dpidr_no_ack";
        return false;
    }
    swd_observed_dpidr = *dpidr;
    swd_failed_step = "swd_dpidr_mismatch";
    if (*dpidr != DP_ID_EXPECTED) return false;

    // Debug-port configuration is required for read-only AHB-AP access.
    // No writes to target memory or CPU registers, no reset, no halt.
    swd_failed_step = "swd_dp_power_failed";
    if (!dp_write(0x00, 0x1eU) || !dp_write(0x04, 0x50000000U))
        return false;
    bool powered = false;
    for (int i = 0; i < 20; ++i) {
        uint32_t ctrl = 0;
        if (dp_read(0x04, &ctrl) && (ctrl & 0xA0000000U) == 0xA0000000U) {
            powered = true;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    if (!powered) return false;
    swd_failed_step = "swd_ap_select_failed";
    if (!dp_write(0x08, 0)) return false;
    swd_failed_step = "swd_ap_config_failed";
    if (!ap_write(0x00, 0x23000052U)) return false;
    swd_failed_step = "ok";
    return true;
}

static void swd_disconnect_adapter(void) {
    // Releases only the probe's GPIO; does not reset the GD32.
    const uint8_t command[] = {ID_DAP_Disconnect};
    uint8_t response[4] = {0};
    (void)DAP_ProcessCommand(command, response);
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
    latest.swd_last_ack = swd_last_ack;
    latest.swd_clock_hz = swd_last_clock;
    latest.dpidr = swd_observed_dpidr;
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
        // If the initial SWD clock does not work, retry once at a lower
        // frequency. Both attempts are read-only for the GD32.
        const uint32_t frequencies[] = {100000U, 25000U};
        bool initialized = false;
        for (size_t i = 0; i < sizeof(frequencies)/sizeof(frequencies[0]); ++i) {
            if (swd_init(&dpidr, frequencies[i])) {
                initialized = true;
                break;
            }
            // Preserve the most recent stage/ACK in telemetry for diagnostics.
            status_set(swd_failed_step, true);
            swd_disconnect_adapter();
        }
        if (!initialized) {
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
    /* Extended OEM diagnostic items 202-219: read only, optional.
       All addresses are backed by disassembly's diagnostic dispatcher.
       Unmapped bumper bits remain explicitly unknown. */
    uint32_t timer_words[8] = {0}, stop_words[2] = {0}, wall_words[2] = {0};
    uint32_t fan_words[2] = {0}, wall_flag_words[2] = {0};
    uint32_t fan_level_word[1] = {0}, fs_word[1] = {0};
    uint32_t bump_word[1] = {0}, charge_stage[1] = {0}, charge_activity[1] = {0};
    uint32_t robot_flags[1] = {0}, change_time[1] = {0}, fan_pwm[1] = {0};
    bool diag_ok =
        read_words(0x20000390U, 8, timer_words) &&
        read_words(0x20000598U, 2, stop_words) &&
        read_words(0x20000638U, 2, wall_words) &&
        read_words(0x20000610U, 2, wall_flag_words) &&
        read_words(0x20000428U, 2, fan_words) &&
        read_words(0x20000180U, 1, fan_level_word) &&
        read_words(0x200003C4U, 1, fs_word) &&
        read_words(0x20000630U, 1, bump_word) &&
        read_words(0x2000030CU, 1, charge_stage) &&
        read_words(0x20000280U, 1, charge_activity) &&
        read_words(0x20000024U, 1, robot_flags) &&
        read_words(0x2000003CU, 1, change_time) &&
        read_words(0x40012C3CU, 1, fan_pwm);
    uint32_t gpio_inputs[5] = {0};
    bool gpio_ok = true;
    for (unsigned i = 0; i < 5; i++)
        if (!read_words(0x40010808U + 0x400U*i, 1, &gpio_inputs[i]))
            gpio_ok = false;
    portENTER_CRITICAL(&latest_mux);
    latest.diagnostics_valid = diag_ok;
    latest.gpio_valid = gpio_ok;
    if (diag_ok) {
        latest.run_time = (uint16_t)timer_words[3];
        latest.run_time_1 = (uint16_t)(timer_words[3] >> 16);
        latest.total_run_time = timer_words[0];
        latest.dock_time = (uint16_t)timer_words[7];
        latest.work_l = (uint16_t)(timer_words[4] >> 16);
        latest.dock_l = (uint16_t)timer_words[5];
        latest.stop_time = (uint16_t)(stop_words[0] >> 16);
        latest.stop_time_1 = (uint16_t)stop_words[1];
        latest.wall_count = wall_words[0];
        latest.wall_count_1 = wall_words[1];
        latest.wall_flag = (uint8_t)wall_flag_words[0];
        /* Diagnostic 202/206: FanLevel/g_FanLevel=0x20000397,
           Fan_Mode=0x20000180; PowerBoost=0x2000042A,
           WallFollow=0x2000042E, Spiral=0x2000042F. */
        latest.fan_level = (uint8_t)(status_word[0] >> 24);
        latest.fan_mode = (uint8_t)fan_level_word[0];
        latest.power_boost = (uint8_t)(fan_words[0] >> 16);
        latest.remote_wall_follow = (uint8_t)(fan_words[1] >> 16);
        latest.remote_spiral_raw = (uint8_t)(fan_words[1] >> 24);
        latest.wall_bump_count = (uint16_t)(bump_word[0] >> 16);
        latest.charge_counter = (uint16_t)charge_stage[0];
        latest.charge_control_state = (uint16_t)(charge_stage[0] >> 16);
        latest.charge_activity_flag = (uint8_t)(charge_activity[0] >> 8);
        latest.fs_raw = (uint16_t)fs_word[0];
        latest.diagnostic_213_a = (uint16_t)(analog[6] >> 16);
        latest.diagnostic_213_b = (uint16_t)wall_flag_words[1];
        latest.change_time = change_time[0];
        latest.fan_pwm = fan_pwm[0];
        latest.robot_flags = robot_flags[0];
    }
    if (gpio_ok)
        for (int i = 0; i < 5; i++) latest.gpio_idr[i] = (uint16_t)gpio_inputs[i];
    latest.valid = true;
    latest.dpidr = DP_ID_EXPECTED;
    latest.swd_last_ack = swd_last_ack;
    latest.swd_clock_hz = swd_last_clock;
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

/* ESP-IDF HTTP server, separate from OTA server (port 8081).
   Using the built-in HTTP parser avoids partial recv(), manual Content-Length
   mistakes and aborted TCP sessions. A dedicated HTTP server task avoids
   occupying the CMSIS-DAP/SWD polling task. */
static esp_err_t eufy_health_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, "{\"http\":true,\"service\":\"telemetry\"}");
}
static esp_err_t eufy_telemetry_handler(httpd_req_t *req) {
    /* No SWD access in the HTTP task. Read the latest snapshot only. */
    eufy_data_t s;
    portENTER_CRITICAL(&latest_mux);
    s = latest;
    portEXIT_CRITICAL(&latest_mux);
    int64_t age_ms = s.updated_us
        ? (esp_timer_get_time() - s.updated_us) / 1000LL : -1;
    const size_t payload_cap = 4096;
    char *payload = malloc(payload_cap);
    if (payload == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                  "Not enough memory for telemetry");
    }
    int pn = snprintf(payload, payload_cap,
        "{\"status\":\"%s\",\"valid\":%s,\"openocd_connected\":%s,"
        "\"sample_age_ms\":%lld,\"errors\":%lu,\"dpidr\":\"0x%08lx\","
        "\"swd_last_ack\":%u,\"swd_clock_hz\":%lu,"
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
        "\"mode\":\"read_only_swd\","
        "\"diagnostics\":{\"valid\":%s,\"run_time_raw\":%u,"
        "\"run_time_1_raw\":%u,\"total_run_time_raw\":%lu,"
        "\"change_time_raw\":%lu,\"dock_time_raw\":%u,"
        "\"work_l_raw\":%u,\"dock_l_raw\":%u,"
        "\"stop_time_raw\":%u,\"stop_time_1_raw\":%u,"
        "\"wall_count_raw\":%lu,\"wall_count_1_raw\":%lu,"
        "\"wall_flag_raw\":%u,\"fan_level_raw\":%u,"
        "\"fan_mode_raw\":%u,\"fan_pwm_raw\":%lu,"
        "\"power_boost_raw\":%u,\"remote_wall_follow_raw\":%u,"
        "\"remote_spiral_raw\":%lu,\"fs_raw\":%u,"
        "\"diag_213_first_raw\":%u,\"diag_213_second_raw\":%u,"
        "\"robot_flags_raw\":%lu,\"charge_counter_raw\":%u,"
        "\"charge_control_state_raw\":%u,\"charge_activity_flag_raw\":%u},"
        "\"gpio_inputs\":{\"valid\":%s,\"A\":%u,\"B\":%u,"
        "\"C\":%u,\"D\":%u,\"E\":%u},"
        "\"bumper\":{\"channels_mapped\":false,\"wall_bump_count_raw\":%u,"
        "\"reason\":\"individual_pins_unverified\"}}\n",
        s.status, s.valid ? "true" : "false",
        s.host_connected ? "true" : "false",
        (long long)age_ms, (unsigned long)s.errors, (unsigned long)s.dpidr,
        s.swd_last_ack, (unsigned long)s.swd_clock_hz,
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
        s.brush_main_adc, s.brush_l_adc, s.brush_r_adc,
        s.diagnostics_valid ? "true" : "false",
        s.run_time, s.run_time_1, (unsigned long)s.total_run_time,
        (unsigned long)s.change_time, s.dock_time, s.work_l, s.dock_l,
        s.stop_time, s.stop_time_1, (unsigned long)s.wall_count,
        (unsigned long)s.wall_count_1, s.wall_flag, s.fan_level,
        s.fan_mode, (unsigned long)s.fan_pwm, s.power_boost,
        s.remote_wall_follow, (unsigned long)s.remote_spiral_raw,
        s.fs_raw, s.diagnostic_213_a, s.diagnostic_213_b,
        (unsigned long)s.robot_flags,
        s.charge_counter, s.charge_control_state, s.charge_activity_flag,
        s.gpio_valid ? "true" : "false",
        s.gpio_idr[0], s.gpio_idr[1], s.gpio_idr[2],
        s.gpio_idr[3], s.gpio_idr[4], s.wall_bump_count);
    esp_err_t err;
    if (pn < 1 || (size_t)pn >= payload_cap) {
        printf("eufy monitor: failed JSON formatting (length=%d)\n", pn);
        err = httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                  "Telemetry JSON buffer error");
    } else {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        err = httpd_resp_send(req, payload, pn);
    }
    free(payload);
    return err;
}
void eufy_monitor_server_start(void) {
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = EUFY_HTTP_PORT;
    /* The OTA httpd (8081) owns the default ctrl_port=32768. */
    cfg.ctrl_port = 32769;
    cfg.stack_size = 8192;
    cfg.max_uri_handlers = 4;
    cfg.lru_purge_enable = true;
    httpd_handle_t server = NULL;
    esp_err_t err = httpd_start(&server, &cfg);
    if (err != ESP_OK) {
        printf("eufy monitor: HTTP server start failed (%s)\n",
               esp_err_to_name(err));
        return;
    }
    httpd_uri_t telem = {
        .uri="/telemetry", .method=HTTP_GET, .handler=eufy_telemetry_handler
    };
    httpd_uri_t health = {
        .uri="/health", .method=HTTP_GET, .handler=eufy_health_handler
    };
    httpd_uri_t root = {
        .uri="/", .method=HTTP_GET, .handler=eufy_telemetry_handler
    };
    esp_err_t a = httpd_register_uri_handler(server, &telem);
    esp_err_t b = httpd_register_uri_handler(server, &health);
    esp_err_t c = httpd_register_uri_handler(server, &root);
    if (a != ESP_OK || b != ESP_OK || c != ESP_OK)
        printf("eufy monitor: HTTP URI registration failed\n");
    else
        printf("eufy monitor: HTTP port %d, /health, /telemetry\n", EUFY_HTTP_PORT);
}
