#include <stdlib.h>
#include <string.h>

#include "escape_core.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

#define CMDLINE_MAX 4400            /* CONFIG_EMBPET_CMDLINE_MAX default */
#define ARGV_CAP    32

static char s_line[CMDLINE_MAX + 1];
static char *s_argv[ARGV_CAP];
static size_t s_argc;

static esc_err_t split(const char *text)
{
    TEST_ASSERT_TRUE(strlen(text) < sizeof(s_line));
    strcpy(s_line, text);
    return esc_split(s_line, s_argv, ARGV_CAP, &s_argc);
}

/* ---- esc_split ---- */

static void test_plain_words(void)
{
    TEST_ASSERT_EQUAL(ESC_OK, split("  sys   info\t-i 7  "));
    TEST_ASSERT_EQUAL_UINT(4, s_argc);
    TEST_ASSERT_EQUAL_STRING("sys", s_argv[0]);
    TEST_ASSERT_EQUAL_STRING("info", s_argv[1]);
    TEST_ASSERT_EQUAL_STRING("-i", s_argv[2]);
    TEST_ASSERT_EQUAL_STRING("7", s_argv[3]);
    TEST_ASSERT_NULL(s_argv[4]);
}

static void test_empty_line(void)
{
    TEST_ASSERT_EQUAL(ESC_OK, split(""));
    TEST_ASSERT_EQUAL_UINT(0, s_argc);
    TEST_ASSERT_NULL(s_argv[0]);
    TEST_ASSERT_EQUAL(ESC_OK, split("   \t "));
    TEST_ASSERT_EQUAL_UINT(0, s_argc);
}

static void test_quoted_escapes(void)
{
    TEST_ASSERT_EQUAL(ESC_OK, split("tg send -a A \"line1\\nsaid \\\"hi\\\" C:\\\\tmp\""));
    TEST_ASSERT_EQUAL_UINT(5, s_argc);
    TEST_ASSERT_EQUAL_STRING("line1\nsaid \"hi\" C:\\tmp", s_argv[4]);
}

static void test_quoted_spaces_and_empty(void)
{
    TEST_ASSERT_EQUAL(ESC_OK, split("wifi set \"my  ssid\" \"\""));
    TEST_ASSERT_EQUAL_UINT(4, s_argc);
    TEST_ASSERT_EQUAL_STRING("my  ssid", s_argv[2]);
    TEST_ASSERT_EQUAL_STRING("", s_argv[3]);
}

static void test_unquoted_backslash_and_quote_are_literal(void)
{
    TEST_ASSERT_EQUAL(ESC_OK, split("a\\nb c\"d e\\"));
    TEST_ASSERT_EQUAL_UINT(3, s_argc);
    TEST_ASSERT_EQUAL_STRING("a\\nb", s_argv[0]);
    TEST_ASSERT_EQUAL_STRING("c\"d", s_argv[1]);
    TEST_ASSERT_EQUAL_STRING("e\\", s_argv[2]);     /* trailing backslash, unquoted */
}

static void test_utf8_passthrough(void)
{
    /* "Grüße — привет 👋" plus an escaped newline between multibyte characters */
    const char *in = "tg send \"Gr\xC3\xBC\xC3\x9F" "e \xE2\x80\x94 \xD0\xBF\xD1\x80\xD0\xB8\\n"
                     "\xF0\x9F\x91\x8B\"";
    TEST_ASSERT_EQUAL(ESC_OK, split(in));
    TEST_ASSERT_EQUAL_UINT(3, s_argc);
    TEST_ASSERT_EQUAL_STRING("Gr\xC3\xBC\xC3\x9F" "e \xE2\x80\x94 \xD0\xBF\xD1\x80\xD0\xB8\n"
                             "\xF0\x9F\x91\x8B", s_argv[2]);
    /* unquoted UTF-8, and bytes that are not valid UTF-8 are not touched either */
    TEST_ASSERT_EQUAL(ESC_OK, split("\xC3\xA4\xFF\x80 x"));
    TEST_ASSERT_EQUAL_STRING("\xC3\xA4\xFF\x80", s_argv[0]);
}

static void test_trailing_backslash_in_quotes(void)
{
    TEST_ASSERT_EQUAL(ESC_ERR_TRAILING_BACKSLASH, split("tg send -i 9 \"abc\\"));
    TEST_ASSERT_EQUAL_UINT(4, s_argc);              /* completed args still usable */
    TEST_ASSERT_EQUAL_STRING("9", s_argv[3]);
    TEST_ASSERT_NULL(s_argv[4]);
}

static void test_errors(void)
{
    TEST_ASSERT_EQUAL(ESC_ERR_UNTERMINATED, split("x -i 1 \"abc"));
    TEST_ASSERT_EQUAL_UINT(3, s_argc);
    TEST_ASSERT_EQUAL_STRING("1", s_argv[2]);
    TEST_ASSERT_EQUAL(ESC_ERR_UNTERMINATED, split("x \"abc\\\""));   /* \" does not close */
    TEST_ASSERT_EQUAL(ESC_ERR_BAD_ESCAPE, split("x \"a\\tb\""));
    TEST_ASSERT_EQUAL_UINT(1, s_argc);
    TEST_ASSERT_EQUAL(ESC_ERR_AFTER_QUOTE, split("x \"a\"b"));
    TEST_ASSERT_EQUAL(ESC_OK, split("x \"a\"\t\"b\""));
    TEST_ASSERT_EQUAL_UINT(3, s_argc);
    TEST_ASSERT_NOT_EQUAL(0, strlen(esc_strerror(ESC_ERR_BAD_ESCAPE)));
    TEST_ASSERT_EQUAL_STRING("ok", esc_strerror(ESC_OK));
}

static void test_too_many_args(void)
{
    char text[3 * ARGV_CAP + 1] = "";
    for (int i = 0; i < ARGV_CAP; i++) {
        strcat(text, "a ");
    }
    TEST_ASSERT_EQUAL(ESC_ERR_TOO_MANY_ARGS, split(text));
    TEST_ASSERT_EQUAL_UINT(ARGV_CAP - 1, s_argc);
    TEST_ASSERT_NULL(s_argv[ARGV_CAP - 1]);
    text[2 * (ARGV_CAP - 1)] = '\0';                /* exactly ARGV_CAP - 1 args fit */
    TEST_ASSERT_EQUAL(ESC_OK, split(text));
    TEST_ASSERT_EQUAL_UINT(ARGV_CAP - 1, s_argc);
}

/* A maximal line: a quoted text arg made only of escapes decodes to half its size, and the
 * last byte of a CMDLINE_MAX line is still seen. */
static void test_max_length(void)
{
    static char text[CMDLINE_MAX + 1];
    const char *head = "tg send -a A -i r \"";
    size_t hl = strlen(head);
    memcpy(text, head, hl);
    size_t pos = hl;
    size_t body = CMDLINE_MAX - hl - 1;             /* bytes between the quotes */
    size_t n_esc = body / 2;
    size_t n_z = body % 2;
    for (size_t i = 0; i < n_esc; i++) {
        text[pos++] = '\\';
        text[pos++] = (i % 2 == 0) ? 'n' : '\\';
    }
    for (size_t i = 0; i < n_z; i++) {
        text[pos++] = 'z';
    }
    text[pos++] = '"';
    text[pos] = '\0';
    TEST_ASSERT_EQUAL_UINT(CMDLINE_MAX, strlen(text));
    TEST_ASSERT_EQUAL(ESC_OK, split(text));
    TEST_ASSERT_EQUAL_UINT(7, s_argc);
    TEST_ASSERT_EQUAL_STRING("r", s_argv[5]);
    TEST_ASSERT_EQUAL_UINT(n_esc + n_z, strlen(s_argv[6]));
    for (size_t i = 0; i < n_esc; i++) {
        TEST_ASSERT_EQUAL_CHAR((i % 2 == 0) ? '\n' : '\\', s_argv[6][i]);
    }
}

/* ---- ids ---- */

static void test_id_valid(void)
{
    TEST_ASSERT_TRUE(esc_id_valid("A", ESC_ACTOR_MAX));
    TEST_ASSERT_TRUE(esc_id_valid("build_bot-7", ESC_ACTOR_MAX));
    TEST_ASSERT_TRUE(esc_id_valid("0123456789abcdef", ESC_ACTOR_MAX));          /* 16 */
    TEST_ASSERT_FALSE(esc_id_valid("0123456789abcdefg", ESC_ACTOR_MAX));        /* 17 */
    TEST_ASSERT_TRUE(esc_id_valid("0123456789abcdef0123456789abcdef", ESC_RID_MAX));
    TEST_ASSERT_FALSE(esc_id_valid("0123456789abcdef0123456789abcdef0", ESC_RID_MAX));
    TEST_ASSERT_FALSE(esc_id_valid("", ESC_RID_MAX));
    TEST_ASSERT_FALSE(esc_id_valid(NULL, ESC_RID_MAX));
    TEST_ASSERT_FALSE(esc_id_valid("a b", ESC_RID_MAX));
    TEST_ASSERT_FALSE(esc_id_valid("a.b", ESC_RID_MAX));
    TEST_ASSERT_FALSE(esc_id_valid("\xC3\xA4", ESC_RID_MAX));
}

static void test_scan_ids(void)
{
    const char *actor;
    const char *rid;
    TEST_ASSERT_EQUAL(ESC_OK, split("tg send -i 42 -a A \"-i x\""));
    esc_scan_ids(s_argc, s_argv, &actor, &rid);
    TEST_ASSERT_EQUAL_STRING("A", actor);
    TEST_ASSERT_EQUAL_STRING("42", rid);

    TEST_ASSERT_EQUAL(ESC_OK, split("foo -iR1 -aB -i R2"));                    /* glued, first wins */
    esc_scan_ids(s_argc, s_argv, &actor, &rid);
    TEST_ASSERT_EQUAL_STRING("B", actor);
    TEST_ASSERT_EQUAL_STRING("R1", rid);

    TEST_ASSERT_EQUAL(ESC_OK, split("-i 1 foo -- -i 2 -a A"));                 /* argv[0] skipped */
    esc_scan_ids(s_argc, s_argv, &actor, &rid);
    TEST_ASSERT_NULL(actor);
    TEST_ASSERT_NULL(rid);

    TEST_ASSERT_EQUAL(ESC_OK, split("foo -a"));                                /* missing value */
    esc_scan_ids(s_argc, s_argv, &actor, &rid);
    TEST_ASSERT_NULL(actor);
}

/* ---- line assembly ---- */

static char s_buf[16];
static esc_line_t s_l;

static char s_last[sizeof(s_buf)];

/* Feed a string; return the number of completed lines, the last one copied to s_last. */
static int feed(const char *s, size_t n)
{
    int lines = 0;
    for (size_t i = 0; i < n; i++) {
        esc_line_ev_t ev = esc_line_feed(&s_l, (uint8_t)s[i]);
        if (ev == ESC_LINE_DONE) {
            lines++;
            strcpy(s_last, s_buf);
            esc_line_reset(&s_l);
        }
    }
    return lines;
}

static void test_line_endings(void)
{
    esc_line_init(&s_l, s_buf, sizeof(s_buf));
    TEST_ASSERT_EQUAL(1, feed("abc\n", 4));
    TEST_ASSERT_EQUAL_STRING("abc", s_last);
    TEST_ASSERT_EQUAL(1, feed("de\r\n", 4));           /* CRLF: one line, LF swallowed */
    TEST_ASSERT_EQUAL_STRING("de", s_last);
    TEST_ASSERT_EQUAL(2, feed("x\ry\r", 4));           /* bare CR terminates too */
    TEST_ASSERT_EQUAL_STRING("y", s_last);
    TEST_ASSERT_EQUAL(2, feed("\n\nz\n", 4));          /* first LF follows the CR above */
    TEST_ASSERT_EQUAL_STRING("z", s_last);
}

static void test_line_crlf_swallow_once(void)
{
    esc_line_init(&s_l, s_buf, sizeof(s_buf));
    TEST_ASSERT_EQUAL(1, feed("a\r", 2));
    TEST_ASSERT_EQUAL(ESC_LINE_IGNORED, esc_line_feed(&s_l, '\n'));
    TEST_ASSERT_EQUAL(ESC_LINE_DONE, esc_line_feed(&s_l, '\n'));   /* second LF = empty line */
    TEST_ASSERT_EQUAL_UINT(0, s_l.len);
}

static void test_line_controls_and_utf8(void)
{
    esc_line_init(&s_l, s_buf, sizeof(s_buf));
    TEST_ASSERT_EQUAL(ESC_LINE_IGNORED, esc_line_feed(&s_l, 0x1b));
    TEST_ASSERT_EQUAL(ESC_LINE_IGNORED, esc_line_feed(&s_l, 0x03));
    TEST_ASSERT_EQUAL(ESC_LINE_IGNORED, esc_line_feed(&s_l, 0x7f));   /* BS on empty line */
    TEST_ASSERT_EQUAL(ESC_LINE_STORED, esc_line_feed(&s_l, '\t'));    /* TAB is kept */
    TEST_ASSERT_EQUAL(ESC_LINE_ERASED, esc_line_feed(&s_l, 0x08));
    TEST_ASSERT_EQUAL_UINT(0, s_l.len);
    TEST_ASSERT_EQUAL(ESC_LINE_STORED, esc_line_feed(&s_l, 'a'));
    TEST_ASSERT_EQUAL(ESC_LINE_STORED, esc_line_feed(&s_l, 0xF0));    /* 4-byte emoji */
    TEST_ASSERT_EQUAL(ESC_LINE_STORED, esc_line_feed(&s_l, 0x9F));
    TEST_ASSERT_EQUAL(ESC_LINE_STORED, esc_line_feed(&s_l, 0x91));
    TEST_ASSERT_EQUAL(ESC_LINE_STORED, esc_line_feed(&s_l, 0x8B));
    TEST_ASSERT_EQUAL_UINT(5, s_l.len);
    TEST_ASSERT_EQUAL(ESC_LINE_ERASED, esc_line_feed(&s_l, 0x08));    /* erases all 4 bytes */
    TEST_ASSERT_EQUAL_UINT(1, s_l.len);
    TEST_ASSERT_EQUAL(ESC_LINE_STORED, esc_line_feed(&s_l, 0xC3));
    TEST_ASSERT_EQUAL(ESC_LINE_STORED, esc_line_feed(&s_l, 0xA4));
    TEST_ASSERT_EQUAL(ESC_LINE_DONE, esc_line_feed(&s_l, '\n'));
    TEST_ASSERT_EQUAL_STRING("a\xC3\xA4", s_buf);
}

/* BS removes a lead byte with at most 3 continuation bytes, even on invalid UTF-8. */
static void test_line_erase_bounded(void)
{
    esc_line_init(&s_l, s_buf, sizeof(s_buf));
    TEST_ASSERT_EQUAL(ESC_LINE_STORED, esc_line_feed(&s_l, 'a'));
    for (int i = 0; i < 6; i++) {
        TEST_ASSERT_EQUAL(ESC_LINE_STORED, esc_line_feed(&s_l, 0x80));
    }
    TEST_ASSERT_EQUAL_UINT(7, s_l.len);
    TEST_ASSERT_EQUAL(ESC_LINE_ERASED, esc_line_feed(&s_l, 0x7f));
    TEST_ASSERT_EQUAL_UINT(3, s_l.len);                /* 4 bytes per erase at most */
    TEST_ASSERT_EQUAL(ESC_LINE_ERASED, esc_line_feed(&s_l, 0x7f));
    TEST_ASSERT_EQUAL_UINT(0, s_l.len);                /* 'a' + 2 continuation bytes */
}

/* A line typed with TABs splits on them; inside quotes a TAB is text. */
static void test_line_tab_end_to_end(void)
{
    esc_line_init(&s_l, s_buf, sizeof(s_buf));
    const char *typed = "a\tb \"c\td\"\n";
    TEST_ASSERT_EQUAL(1, feed(typed, strlen(typed)));
    TEST_ASSERT_EQUAL(ESC_OK, split(s_last));
    TEST_ASSERT_EQUAL_UINT(3, s_argc);
    TEST_ASSERT_EQUAL_STRING("a", s_argv[0]);
    TEST_ASSERT_EQUAL_STRING("b", s_argv[1]);
    TEST_ASSERT_EQUAL_STRING("c\td", s_argv[2]);
}

/* Ctrl-U discards the line wherever it arrives; the next line starts clean. */
static void test_line_kill(void)
{
    esc_line_init(&s_l, s_buf, sizeof(s_buf));
    TEST_ASSERT_EQUAL(ESC_LINE_KILLED, esc_line_feed(&s_l, 0x15));    /* at line start */
    TEST_ASSERT_EQUAL_UINT(0, s_l.len);
    TEST_ASSERT_EQUAL(1, feed("tg send\x15sys info\n", 17));         /* plain */
    TEST_ASSERT_EQUAL_STRING("sys info", s_last);
    TEST_ASSERT_EQUAL(1, feed("a \"b c\x15ok\n", 10));               /* inside quotes */
    TEST_ASSERT_EQUAL_STRING("ok", s_last);
    TEST_ASSERT_EQUAL(ESC_OK, split(s_last));
    TEST_ASSERT_EQUAL_UINT(1, s_argc);
    TEST_ASSERT_EQUAL(1, feed("\"a\\\x15x\n", 6));                   /* after a backslash escape */
    TEST_ASSERT_EQUAL_STRING("x", s_last);
    TEST_ASSERT_EQUAL(ESC_OK, split(s_last));
    TEST_ASSERT_EQUAL(1, feed("ab\x15\x15\x15z\n", 7));              /* repeated */
    TEST_ASSERT_EQUAL_STRING("z", s_last);
    TEST_ASSERT_EQUAL(1, feed("ab\x15\n", 4));                       /* kill, then empty line */
    TEST_ASSERT_EQUAL_STRING("", s_last);
}

/* Ctrl-U also clears a line that overflowed, and a pending CR does not survive it. */
static void test_line_kill_overflow_and_cr(void)
{
    esc_line_init(&s_l, s_buf, sizeof(s_buf));
    for (int i = 0; i < 20; i++) {
        esc_line_feed(&s_l, 'x');
    }
    TEST_ASSERT_TRUE(s_l.overflow);
    TEST_ASSERT_EQUAL(ESC_LINE_KILLED, esc_line_feed(&s_l, 0x15));
    TEST_ASSERT_FALSE(s_l.overflow);
    TEST_ASSERT_EQUAL(1, feed("ok\n", 3));
    TEST_ASSERT_EQUAL_STRING("ok", s_last);
    TEST_ASSERT_EQUAL(1, feed("a\r", 2));
    TEST_ASSERT_EQUAL(ESC_LINE_KILLED, esc_line_feed(&s_l, 0x15));
    TEST_ASSERT_EQUAL(ESC_LINE_DONE, esc_line_feed(&s_l, '\n'));      /* not swallowed as CRLF */
}

static void test_line_overflow(void)
{
    esc_line_init(&s_l, s_buf, sizeof(s_buf));     /* holds 15 bytes */
    for (int i = 0; i < 15; i++) {
        TEST_ASSERT_EQUAL(ESC_LINE_STORED, esc_line_feed(&s_l, 'x'));
    }
    TEST_ASSERT_FALSE(s_l.overflow);
    TEST_ASSERT_EQUAL(ESC_LINE_IGNORED, esc_line_feed(&s_l, 'y'));
    TEST_ASSERT_TRUE(s_l.overflow);
    TEST_ASSERT_EQUAL(ESC_LINE_IGNORED, esc_line_feed(&s_l, 0x08));   /* no erase once lost */
    TEST_ASSERT_EQUAL(ESC_LINE_DONE, esc_line_feed(&s_l, '\n'));
    TEST_ASSERT_EQUAL_UINT(15, strlen(s_buf));        /* the prefix is kept */
    esc_line_reset(&s_l);
    TEST_ASSERT_FALSE(s_l.overflow);
    TEST_ASSERT_EQUAL(1, feed("ok\n", 3));
    TEST_ASSERT_EQUAL_STRING("ok", s_last);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_plain_words);
    RUN_TEST(test_empty_line);
    RUN_TEST(test_quoted_escapes);
    RUN_TEST(test_quoted_spaces_and_empty);
    RUN_TEST(test_unquoted_backslash_and_quote_are_literal);
    RUN_TEST(test_utf8_passthrough);
    RUN_TEST(test_trailing_backslash_in_quotes);
    RUN_TEST(test_errors);
    RUN_TEST(test_too_many_args);
    RUN_TEST(test_max_length);
    RUN_TEST(test_id_valid);
    RUN_TEST(test_scan_ids);
    RUN_TEST(test_line_endings);
    RUN_TEST(test_line_crlf_swallow_once);
    RUN_TEST(test_line_controls_and_utf8);
    RUN_TEST(test_line_erase_bounded);
    RUN_TEST(test_line_kill);
    RUN_TEST(test_line_kill_overflow_and_cr);
    RUN_TEST(test_line_tab_end_to_end);
    RUN_TEST(test_line_overflow);
    return UNITY_END();
}
