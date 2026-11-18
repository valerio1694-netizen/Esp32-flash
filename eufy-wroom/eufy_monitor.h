#pragma once
/*
 * Eufy RoboVac 15C telemetry via non-halting SWD AHB-AP reads.
 * The CMSIS-DAP TCP task owns SWCLK/SWDIO exclusively.
 * Do not call these polling functions from another task.
 */
#include <stdbool.h>
void eufy_monitor_poll(void);
void eufy_monitor_client_changed(bool connected);
void eufy_monitor_server_start(void);
