/* Console command groups registered from app_main. */
#ifndef EMBPET_CMDS_H
#define EMBPET_CMDS_H

#include "esp_log.h"

/* sys info, sys log, sys reboot, evt since (cmd_sys.c). Call before console_start(). */
void cmd_sys_register(void);

/* wifi set, wifi status, wifi txpower (cmd_wifi.c). Call before console_start(). */
void cmd_wifi_register(void);

/* Set the console log level for all tags and remember it for `sys info`. */
void cmd_sys_set_log_level(esp_log_level_t level);

#endif /* EMBPET_CMDS_H */
