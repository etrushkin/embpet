/* tgbot: Telegram bridge (README 2.1): `tg send`, the getUpdates poller with reply routing,
 * the pending table and the `tg pending` / `tg ack` / `tg status` commands. */
#ifndef EMBPET_TGBOT_H
#define EMBPET_TGBOT_H

#include "esp_err.h"
#include "sdkconfig.h"

#ifdef CONFIG_EMBPET_TG_BRIDGE
/* Create the pending table, the tg_poll and tg_send tasks (each owns one Bot API
 * connection) and register the tg commands. Call once from app_main after tgapi_init(). */
esp_err_t tgbot_start(void);
#else
/* Bridge compiled out (the Bot API client test build, tgtest.defaults). */
static inline esp_err_t tgbot_start(void)
{
    return ESP_OK;
}
#endif

#endif /* EMBPET_TGBOT_H */
