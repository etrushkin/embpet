#include <string.h>

#include "cfg_core.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

#define ELL "\xE2\x80\xA6"

static void test_mask_standard_token(void)
{
    char out[CFG_MASK_BUF_SIZE];
    size_t n = cfg_mask_token("123456:AAHdqTcvCH1vGWJxfSeofSAs0K5PALDsaw", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("123456:AA" ELL "saw", out);
    TEST_ASSERT_EQUAL_UINT(strlen(out), n);
}

static void test_mask_never_contains_middle(void)
{
    const char *tok = "7654321098:AAFmiddlesecretXYZpartQ8d_wLast";
    char out[CFG_MASK_BUF_SIZE];
    cfg_mask_token(tok, out, sizeof(out));
    TEST_ASSERT_NULL(strstr(out, "middlesecret"));
    TEST_ASSERT_NULL(strstr(out, "XYZpart"));
    TEST_ASSERT_NULL(strstr(out, "AAFm"));          /* only 2 chars after ':' are shown */
    TEST_ASSERT_NULL(strstr(out, "wLast"));         /* only 3 trailing chars */
    TEST_ASSERT_EQUAL_STRING("7654321098:AA" ELL "ast", out);
}

static void test_mask_reveals_at_most_5_secret_chars(void)
{
    /* every secret char is distinct 'z'-free filler except the revealed ones */
    char tok[64] = "42:";
    for (int i = 0; i < 20; i++) {
        strcat(tok, "q");
    }
    char out[CFG_MASK_BUF_SIZE];
    cfg_mask_token(tok, out, sizeof(out));
    int q = 0;
    for (const char *p = out; *p; p++) {
        q += (*p == 'q');
    }
    TEST_ASSERT_EQUAL_INT(5, q);
}

static void test_mask_malformed_and_short(void)
{
    char out[CFG_MASK_BUF_SIZE];
    const char *bad[] = {
        "notatoken", "123456", "123456:", "123456:short", ":AAAAAAAAAAAAAAAAAAAA",
        "abc:AAAAAAAAAAAAAAAAAAAA", "1234567890123:AAAAAAAAAAAAAAAAAAAA",
        "123456AAAAAAAAAAAAAAAAAAAAAAAA", "123456:AAAAAAAAAAAAAAA" /* 15 chars */,
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        cfg_mask_token(bad[i], out, sizeof(out));
        TEST_ASSERT_EQUAL_STRING_MESSAGE("***", out, bad[i]);
    }
    /* exactly 16 secret chars is masked normally */
    cfg_mask_token("123456:AAAAAAAAAAAAAAAA", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("123456:AA" ELL "AAA", out);
}

static void test_mask_empty_null_and_truncation(void)
{
    char out[CFG_MASK_BUF_SIZE] = "x";
    TEST_ASSERT_EQUAL_UINT(0, cfg_mask_token("", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("", out);
    TEST_ASSERT_EQUAL_UINT(0, cfg_mask_token(NULL, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("", out);

    char small[5];
    size_t n = cfg_mask_token("123456:AAHdqTcvCH1vGWJxfSeofSAs0K5PALDsaw", small, sizeof(small));
    TEST_ASSERT_EQUAL_UINT(strlen("123456:AA" ELL "saw"), n);
    TEST_ASSERT_EQUAL_STRING("1234", small);
    cfg_mask_token("x", small, 0);   /* cap 0: no write, no crash */
}

static void test_schema_decision(void)
{
    TEST_ASSERT_EQUAL_INT(CFG_VER_STAMP, cfg_schema_decide(false, 0));
    TEST_ASSERT_EQUAL_INT(CFG_VER_KEEP, cfg_schema_decide(true, CFG_SCHEMA_VERSION));
    TEST_ASSERT_EQUAL_INT(CFG_VER_ERASE, cfg_schema_decide(true, CFG_SCHEMA_VERSION + 1));
    TEST_ASSERT_EQUAL_INT(CFG_VER_ERASE, cfg_schema_decide(true, 0));
    TEST_ASSERT_EQUAL_INT(CFG_VER_ERASE, cfg_schema_decide(true, UINT32_MAX));
}

static void test_txq(void)
{
    TEST_ASSERT_TRUE(cfg_txq_valid(8));
    TEST_ASSERT_TRUE(cfg_txq_valid(34));
    TEST_ASSERT_TRUE(cfg_txq_valid(80));
    TEST_ASSERT_FALSE(cfg_txq_valid(7));
    TEST_ASSERT_FALSE(cfg_txq_valid(81));
    TEST_ASSERT_FALSE(cfg_txq_valid(-1));
    TEST_ASSERT_EQUAL_INT8(8, cfg_txq_clamp(-100));
    TEST_ASSERT_EQUAL_INT8(8, cfg_txq_clamp(0));
    TEST_ASSERT_EQUAL_INT8(34, cfg_txq_clamp(34));
    TEST_ASSERT_EQUAL_INT8(80, cfg_txq_clamp(127));
    TEST_ASSERT_EQUAL_INT8(80, cfg_txq_clamp(100000));
}

static void test_debounce(void)
{
    TEST_ASSERT_TRUE(cfg_debounce_valid(0));
    TEST_ASSERT_TRUE(cfg_debounce_valid(1000));
    TEST_ASSERT_FALSE(cfg_debounce_valid(-1));
    TEST_ASSERT_FALSE(cfg_debounce_valid(1001));
    TEST_ASSERT_EQUAL_UINT16(0, cfg_debounce_clamp(-5));
    TEST_ASSERT_EQUAL_UINT16(20, cfg_debounce_clamp(20));
    TEST_ASSERT_EQUAL_UINT16(1000, cfg_debounce_clamp(65535));

    cfg_key_t k = CFG_KEY_COUNT;
    TEST_ASSERT_TRUE(cfg_debounce_key(10, &k));
    TEST_ASSERT_EQUAL_INT(CFG_KEY_IN_DEB_10, k);
    TEST_ASSERT_TRUE(cfg_debounce_key(20, &k));
    TEST_ASSERT_EQUAL_INT(CFG_KEY_IN_DEB_20, k);
    TEST_ASSERT_TRUE(cfg_debounce_key(21, &k));
    TEST_ASSERT_EQUAL_INT(CFG_KEY_IN_DEB_21, k);
    TEST_ASSERT_FALSE(cfg_debounce_key(9, &k));
    TEST_ASSERT_FALSE(cfg_debounce_key(0, &k));
    TEST_ASSERT_FALSE(cfg_debounce_key(-1, &k));
}

static void test_owner_and_out_index(void)
{
    TEST_ASSERT_TRUE(cfg_owner_valid(123456789));
    TEST_ASSERT_TRUE(cfg_owner_valid(-1001234567890LL));
    TEST_ASSERT_FALSE(cfg_owner_valid(0));
    TEST_ASSERT_TRUE(cfg_out_index_valid(0));
    TEST_ASSERT_TRUE(cfg_out_index_valid(7));
    TEST_ASSERT_FALSE(cfg_out_index_valid(8));
    TEST_ASSERT_FALSE(cfg_out_index_valid(-1));
}

static void test_string_validators(void)
{
    TEST_ASSERT_TRUE(cfg_ssid_valid("home"));
    TEST_ASSERT_FALSE(cfg_ssid_valid(""));
    TEST_ASSERT_FALSE(cfg_ssid_valid(NULL));
    TEST_ASSERT_TRUE(cfg_ssid_valid("12345678901234567890123456789012"));
    TEST_ASSERT_FALSE(cfg_ssid_valid("123456789012345678901234567890123"));

    TEST_ASSERT_TRUE(cfg_pass_valid(""));
    TEST_ASSERT_FALSE(cfg_pass_valid("short"));
    TEST_ASSERT_TRUE(cfg_pass_valid("12345678"));
    TEST_ASSERT_FALSE(cfg_pass_valid(NULL));
    TEST_ASSERT_FALSE(cfg_pass_valid("1234567"));                 /* 7 */
    TEST_ASSERT_TRUE(cfg_pass_valid("012345678901234567890123456789012345678901234567890123456789012"));   /* 63 */
    TEST_ASSERT_TRUE(cfg_pass_valid("0123456789abcdefABCDEF0123456789abcdefABCDEF0123456789abcdefABCD")); /* 64 hex */
    TEST_ASSERT_FALSE(cfg_pass_valid("0123456789abcdefABCDEF0123456789abcdefABCDEF0123456789abcdefABCg")); /* 64 non-hex */
    TEST_ASSERT_FALSE(cfg_pass_valid("0123456789abcdefABCDEF0123456789abcdefABCDEF0123456789abcdefABCDE")); /* 65 */

    TEST_ASSERT_TRUE(cfg_token_valid("123456:AAHdqTcvCH1vGWJxfSeofSAs0K5PALDsaw"));
    TEST_ASSERT_TRUE(cfg_token_valid("1:AAAAAAAAAAAAAAAA"));              /* 16 secret chars */
    TEST_ASSERT_TRUE(cfg_token_valid("123456789012:AA_-aZ09AAAAAAAA"));   /* 12-digit id */
    TEST_ASSERT_FALSE(cfg_token_valid("1:AAAAAAAAAAAAAAA"));              /* 15 secret chars */
    TEST_ASSERT_FALSE(cfg_token_valid("1234567890123:AAAAAAAAAAAAAAAA")); /* 13-digit id */
    TEST_ASSERT_FALSE(cfg_token_valid(":AAAAAAAAAAAAAAAA"));
    TEST_ASSERT_FALSE(cfg_token_valid("abc:AAAAAAAAAAAAAAAA"));
    TEST_ASSERT_FALSE(cfg_token_valid("123456AAAAAAAAAAAAAAAA"));
    TEST_ASSERT_FALSE(cfg_token_valid("123456:AAAAAAAAAAAAAAA!"));        /* bad secret char */
    TEST_ASSERT_FALSE(cfg_token_valid("123456:AAAAAAAAAAAA:AAAA"));       /* second colon */
    {
        char longtok[CFG_TOKEN_MAX + 3] = "1:";
        memset(longtok + 2, 'A', CFG_TOKEN_MAX + 1 - 2);              /* 128 chars */
        longtok[CFG_TOKEN_MAX + 1] = '\0';
        TEST_ASSERT_FALSE(cfg_token_valid(longtok));
        longtok[CFG_TOKEN_MAX] = '\0';                                /* 127 chars */
        TEST_ASSERT_TRUE(cfg_token_valid(longtok));
    }
    TEST_ASSERT_FALSE(cfg_token_valid(""));
    TEST_ASSERT_FALSE(cfg_token_valid("has space"));
    TEST_ASSERT_FALSE(cfg_token_valid("tab\t"));
    TEST_ASSERT_FALSE(cfg_token_valid("caf\xC3\xA9"));
    TEST_ASSERT_FALSE(cfg_token_valid(NULL));
}

static void test_key_names(void)
{
    TEST_ASSERT_EQUAL_STRING("wifi_ssid", cfg_key_name(CFG_KEY_WIFI_SSID));
    TEST_ASSERT_EQUAL_STRING("wifi_txq", cfg_key_name(CFG_KEY_WIFI_TXQ));
    TEST_ASSERT_EQUAL_STRING("tg_owner", cfg_key_name(CFG_KEY_TG_OWNER));
    TEST_ASSERT_EQUAL_STRING("in_deb_21", cfg_key_name(CFG_KEY_IN_DEB_21));
    TEST_ASSERT_EQUAL_STRING("?", cfg_key_name(CFG_KEY_COUNT));
    for (int k = 0; k < CFG_KEY_COUNT; k++) {
        TEST_ASSERT_TRUE(strlen(cfg_key_name((cfg_key_t)k)) <= 15);   /* NVS key limit */
        TEST_ASSERT_NOT_EQUAL(0, strcmp("?", cfg_key_name((cfg_key_t)k)));
    }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_mask_standard_token);
    RUN_TEST(test_mask_never_contains_middle);
    RUN_TEST(test_mask_reveals_at_most_5_secret_chars);
    RUN_TEST(test_mask_malformed_and_short);
    RUN_TEST(test_mask_empty_null_and_truncation);
    RUN_TEST(test_schema_decision);
    RUN_TEST(test_txq);
    RUN_TEST(test_debounce);
    RUN_TEST(test_owner_and_out_index);
    RUN_TEST(test_string_validators);
    RUN_TEST(test_key_names);
    return UNITY_END();
}
