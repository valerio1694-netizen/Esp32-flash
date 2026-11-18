#!/usr/bin/env python3
"""Patch pinned upstream CMSIS-DAP TCP to poll GD32 only while no debugger owns SWD."""
from pathlib import Path
import re
import sys
root = Path(sys.argv[1])
version = (Path(__file__).parent / "VERSION").read_text().strip()
if not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", version):
    raise SystemExit(f"Invalid Eufy firmware version: {version!r}")
project_cmake = root / "CMakeLists.txt"
project_source = project_cmake.read_text()
anchor = "project(cmsis_dap_tcp_esp32)"
if project_source.count(anchor) != 1:
    raise SystemExit("Cannot locate upstream project() to assign the version")
project_cmake.write_text(project_source.replace(anchor, f'set(PROJECT_VER "{version}")\n' + anchor))
(root / "main" / "eufy_version.h").write_text(
    f'#pragma once\n#define EUFY_FIRMWARE_VERSION "{version}"\n'
)
src = root / "main" / "cmsis_dap_tcp.c"
s = src.read_text()
def replace_once(old, new):
    global s
    n=s.count(old)
    if n != 1: raise SystemExit(f"Expected exactly one anchor, got {n}: {old[:90]!r}")
    s=s.replace(old,new)
replace_once('#include "cmsis_dap_tcp.h"', '#include "cmsis_dap_tcp.h"\n#include "eufy_monitor.h"\n#include "eufy_ota.h"')
replace_once('int sel = select(fdmax + 1, &read_fds, NULL, NULL, NULL);',
'''struct timeval poll_timeout = { .tv_sec = 0, .tv_usec = 200000 };
        int sel = select(fdmax + 1, &read_fds, NULL, NULL, &poll_timeout);
        if (sel == 0) {
            if (client_fd < 0 && config->instance == 1) eufy_monitor_poll();
            continue;
        }''')
replace_once('state->client_connected = true;',
             'state->client_connected = true;\n                if (config->instance == 1) eufy_monitor_client_changed(true);')
# Disconnect appears once after recv() failure, once after CMSIS-DAP error and once in shutdown.
needle='state->client_connected = false;'
count=s.count(needle)
if count != 4: raise SystemExit(f"Expected four reset points, got {count}")
s=s.replace(needle,needle+'\n                    if (config->instance == 1) eufy_monitor_client_changed(false);')
src.write_text(s)
main=root / "main" / "main.c"
m=main.read_text()
if m.count('#include "cmsis_dap_tcp.h"')!=1: raise SystemExit("main.c include missing")
m=m.replace('#include "cmsis_dap_tcp.h"','#include "cmsis_dap_tcp.h"\n#include "eufy_monitor.h"\n#include "eufy_ota.h"\n#include "eufy_network.h"')
key='    if(cmsis_dap_tcp_task_start() != pdPASS) {'
if m.count(key)!=1: raise SystemExit("main.c init anchor missing")
m=m.replace(key,'    eufy_network_start();\n    eufy_monitor_server_start();\n    eufy_ota_server_start();\n\n'+key)
main.write_text(m)
cm=root / "main" / "CMakeLists.txt"
c=cm.read_text()
key='    "cmsis_dap_tcp.c")'
if c.count(key)!=1: raise SystemExit("CMakeLists anchor missing")
c=c.replace(key,'    "cmsis_dap_tcp.c"\n    "eufy_monitor.c"\n    "eufy_ota.c"\n    "eufy_network.c")')
c=c.replace('"esp_timer" "esp_wifi"', '"esp_timer" "esp_wifi" "app_update" "esp_http_server"')
cm.write_text(c)
print("Eufy read-only monitor injected into CMSIS-DAP TCP owner task.")
