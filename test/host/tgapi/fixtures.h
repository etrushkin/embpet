/* Bot API response fixtures in Telegram's wire format (non-ASCII as \u escapes, astral
 * characters as surrogate pairs). Ids and names are made up. */
#ifndef TGAPI_FIXTURES_H
#define TGAPI_FIXTURES_H

static const char FX_GETME[] =
    "{\"ok\":true,\"result\":{\"id\":7123456789,\"is_bot\":true,\"first_name\":\"embpet\","
    "\"username\":\"embpet_test_bot\",\"can_join_groups\":true,"
    "\"can_read_all_group_messages\":false,\"supports_inline_queries\":false,"
    "\"can_connect_to_business\":false,\"has_main_web_app\":false}}";

static const char FX_DELETE_WEBHOOK[] =
    "{\"ok\":true,\"result\":true,\"description\":\"Webhook is already deleted\"}";

static const char FX_SEND_MESSAGE[] =
    "{\"ok\":true,\"result\":{\"message_id\":1234,\"from\":{\"id\":7123456789,\"is_bot\":true,"
    "\"first_name\":\"embpet\",\"username\":\"embpet_test_bot\"},\"chat\":{\"id\":111222333,"
    "\"first_name\":\"Owner\",\"type\":\"private\"},\"date\":1760450100,"
    "\"text\":\"[A] \\u041f\\u0440\\u0438\\u0432\\u0435\\u0442 \\ud83d\\ude00\","
    "\"entities\":[{\"offset\":0,\"length\":3,\"type\":\"bold\"}]}}";

static const char FX_GET_UPDATES[] =
    "{\"ok\":true,\"result\":[{\"update_id\":861234567,\n"
    "\"message\":{\"message_id\":503,\"from\":{\"id\":111222333,\"is_bot\":false,"
    "\"first_name\":\"Owner\",\"language_code\":\"en\"},\"chat\":{\"id\":111222333,"
    "\"first_name\":\"Owner\",\"type\":\"private\"},\"date\":1760450130,"
    "\"reply_to_message\":{\"message_id\":502,\"from\":{\"id\":7123456789,\"is_bot\":true,"
    "\"first_name\":\"embpet\",\"username\":\"embpet_test_bot\"},\"chat\":{\"id\":111222333,"
    "\"first_name\":\"Owner\",\"type\":\"private\"},\"date\":1760450100,"
    "\"text\":\"[B] Disk at 91% on nas01. Clean /tmp?\","
    "\"entities\":[{\"offset\":0,\"length\":3,\"type\":\"bold\"}]},"
    "\"text\":\"\\u0434\\u0430 \\ud83d\\udc4d\"}},"
    "{\"update_id\":861234568,\n"
    "\"message\":{\"message_id\":504,\"from\":{\"id\":111222333,\"is_bot\":false,"
    "\"first_name\":\"Owner\"},\"chat\":{\"id\":111222333,\"first_name\":\"Owner\","
    "\"type\":\"private\"},\"date\":1760450140,\"text\":\"hello\"}}]}";

/* An owner reply in Cyrillic, CJK and emoji to a bot message, with the escapes Telegram
 * keeps (quote, backslash, slash, newline) and a literal "\u00e9" (escaped backslash). */
static const char FX_GET_UPDATES_INTL[] =
    "{\"ok\":true,\"result\":[{\"update_id\":861234569,\n"
    "\"message\":{\"message_id\":506,\"from\":{\"id\":111222333,\"is_bot\":false,"
    "\"first_name\":\"\\u0412\\u043b\\u0430\\u0434\\u0435\\u043b\\u0435\\u0446\","
    "\"language_code\":\"ru\"},\"chat\":{\"id\":111222333,"
    "\"first_name\":\"\\u0412\\u043b\\u0430\\u0434\\u0435\\u043b\\u0435\\u0446\","
    "\"type\":\"private\"},\"date\":1760450150,"
    "\"reply_to_message\":{\"message_id\":505,\"from\":{\"id\":7123456789,\"is_bot\":true,"
    "\"first_name\":\"embpet\",\"username\":\"embpet_test_bot\"},\"chat\":{\"id\":111222333,"
    "\"type\":\"private\"},\"date\":1760450100,"
    "\"text\":\"[C] \\u4e2d\\u6587 \\\"q\\\" C:\\\\u00e9 a\\/b\\n\\ud83d\\ude00\\ud83d\\udc4d"
    "\\ud83c\\udffd caf\\u00e9\","
    "\"entities\":[{\"offset\":0,\"length\":3,\"type\":\"bold\"}]},"
    "\"text\":\"\\u0434\\u0430, \\u043f\\u043e\\u0447\\u0438\\u0441\\u0442\\u0438 "
    "\\u2014 \\u597d\\u7684 \\ud83d\\udc4d\"}}]}";

static const char FX_EMPTY_UPDATES[] = "{\"ok\":true,\"result\":[]}";

static const char FX_401[] = "{\"ok\":false,\"error_code\":401,\"description\":\"Unauthorized\"}";

static const char FX_404[] = "{\"ok\":false,\"error_code\":404,\"description\":\"Not Found\"}";

static const char FX_429[] =
    "{\"ok\":false,\"error_code\":429,\"description\":\"Too Many Requests: retry after 17\","
    "\"parameters\":{\"retry_after\":17}}";

static const char FX_400_CHAT[] =
    "{\"ok\":false,\"error_code\":400,\"description\":\"Bad Request: chat not found\"}";

static const char FX_403_BLOCKED[] =
    "{\"ok\":false,\"error_code\":403,\"description\":\"Forbidden: bot was blocked by the user\"}";

static const char FX_409_WEBHOOK[] =
    "{\"ok\":false,\"error_code\":409,\"description\":\"Conflict: can't use getUpdates method "
    "while webhook is active; use deleteWebhook to delete the webhook first\"}";

static const char FX_502_HTML[] =
    "<html>\r\n<head><title>502 Bad Gateway</title></head>\r\n<body>\r\n"
    "<center><h1>502 Bad Gateway</h1></center>\r\n<hr><center>nginx/1.18.0</center>\r\n"
    "</body>\r\n</html>\r\n";

#endif /* TGAPI_FIXTURES_H */
