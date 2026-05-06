/* Unit tests for pastime/pastime_external.{c,h}.
 *
 * Production source is linked in directly with PASTIME_EXTERNAL_TEST_BUILD
 * defined, which swaps RA's verbosity macros for our local stub and stubs
 * out the JNI dispatch (we never actually fire an Intent in tests).
 *
 * Coverage: marker parser (valid/invalid forms) and preset lookup
 * (hit, miss, case sensitivity).  The Intent-firing path is verified
 * end-to-end on device — see plan's verification section.
 */

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../pastime_external.h"

/* ---------- log capture for the production source ---------- */

static char g_last_log[256];
static int  g_log_count;

void pastime_external_test_log(const char *fmt, ...)
{
   va_list ap;
   va_start(ap, fmt);
   vsnprintf(g_last_log, sizeof(g_last_log), fmt, ap);
   va_end(ap);
   g_log_count++;
}

/* ---------- test framework ---------- */

static int g_pass;
static int g_fail;

#define ASSERT_TRUE(cond) do { \
   if (cond) { g_pass++; } \
   else { g_fail++; \
      fprintf(stderr, "    FAIL %s:%d  %s\n", \
            __FILE__, __LINE__, #cond); } \
} while (0)

#define ASSERT_FALSE(cond) ASSERT_TRUE(!(cond))

#define ASSERT_STREQ(a, b) do { \
   const char *_a = (a); \
   const char *_b = (b); \
   if (_a && _b && strcmp(_a, _b) == 0) { g_pass++; } \
   else { g_fail++; \
      fprintf(stderr, "    FAIL %s:%d  '%s' == '%s'\n", \
            __FILE__, __LINE__, _a ? _a : "(null)", _b ? _b : "(null)"); } \
} while (0)

#define RUN_TEST(fn) do { \
   int p = g_pass, f = g_fail; \
   fn(); \
   fprintf(stderr, "  %s  %s  (+%d/+%d)\n", \
         g_fail > f ? "FAIL" : "ok  ", #fn, \
         g_pass - p, g_fail - f); \
} while (0)

/* ---------- marker parser ---------- */

static void test_parse_marker_accepts_well_formed_packages(void)
{
   const char *out = NULL;
   ASSERT_TRUE(pastime_external_parse_marker("ext-com.flycast.emulator", &out));
   ASSERT_STREQ(out, "com.flycast.emulator");

   out = NULL;
   ASSERT_TRUE(pastime_external_parse_marker("ext-org.dolphinemu.dolphinemu",
         &out));
   ASSERT_STREQ(out, "org.dolphinemu.dolphinemu");

   /* Mixed case + digits + underscores survive. */
   out = NULL;
   ASSERT_TRUE(pastime_external_parse_marker("ext-io.github.Lime3DS_v2.app",
         &out));
   ASSERT_STREQ(out, "io.github.Lime3DS_v2.app");
}

static void test_parse_marker_rejects_non_ext_prefix(void)
{
   const char *out = (const char*)0xdeadbeef;
   ASSERT_FALSE(pastime_external_parse_marker("snes9x", &out));
   ASSERT_FALSE(pastime_external_parse_marker("android-com.foo.bar", &out));
   ASSERT_FALSE(pastime_external_parse_marker("ex-com.foo.bar", &out));
   /* Pre-dash separator forms (`:` colon and `~` tilde) are no longer
    * accepted — the convention canonicalised on `-` once SD-card
    * staging revealed that exFAT silently mangles colons.  Rejecting
    * them loudly is better than silently parsing into an inconsistent
    * marker form across users. */
   ASSERT_FALSE(pastime_external_parse_marker("ext:com.foo.bar", &out));
   ASSERT_FALSE(pastime_external_parse_marker("ext~com.foo.bar", &out));
   ASSERT_FALSE(pastime_external_parse_marker("", &out));
   ASSERT_FALSE(pastime_external_parse_marker(NULL, &out));
}

static void test_parse_marker_requires_dot(void)
{
   const char *out = NULL;
   /* Single-segment "package" is a libretro-style ident, not an Android
    * package — caller must fall through to the existing parser. */
   ASSERT_FALSE(pastime_external_parse_marker("ext-standalone", &out));
   ASSERT_FALSE(pastime_external_parse_marker("ext-nodot_here", &out));
}

static void test_parse_marker_rejects_bad_chars(void)
{
   const char *out = NULL;
   ASSERT_FALSE(pastime_external_parse_marker("ext-com.foo bar", &out));
   ASSERT_FALSE(pastime_external_parse_marker("ext-com.foo/bar", &out));
   ASSERT_FALSE(pastime_external_parse_marker("ext-com.foo-bar", &out));
   ASSERT_FALSE(pastime_external_parse_marker("ext-com.foo!bar", &out));
}

static void test_parse_marker_rejects_empty_payload(void)
{
   const char *out = NULL;
   ASSERT_FALSE(pastime_external_parse_marker("ext-", &out));
}

/* ---------- preset lookup ---------- */

static void test_lookup_finds_known_packages(void)
{
   /* Spot-check the targets the plan's verification section exercises.
    * If the sync script ever drops one of these we want the test to
    * fail loudly so we notice before shipping. */
   ASSERT_TRUE(pastime_external_lookup("com.flycast.emulator") != NULL);
   ASSERT_TRUE(pastime_external_lookup("org.dolphinemu.dolphinemu") != NULL);
   ASSERT_TRUE(pastime_external_lookup("com.github.stenzek.duckstation")
         != NULL);
   ASSERT_TRUE(pastime_external_lookup("xyz.aethersx2.android") != NULL);
   ASSERT_TRUE(pastime_external_lookup("org.ppsspp.ppsspp") != NULL);
   ASSERT_TRUE(pastime_external_lookup("com.dsemu.drastic") != NULL);
   ASSERT_TRUE(pastime_external_lookup("me.magnum.melonds") != NULL);
   ASSERT_TRUE(pastime_external_lookup("io.github.lime3ds.android") != NULL);
}

static void test_lookup_misses_return_null(void)
{
   ASSERT_TRUE(pastime_external_lookup("com.does.not.exist") == NULL);
   ASSERT_TRUE(pastime_external_lookup("") == NULL);
   ASSERT_TRUE(pastime_external_lookup(NULL) == NULL);
}

static void test_lookup_is_case_sensitive(void)
{
   /* Android package names are case-sensitive identifiers; folder names
    * with a wrong-cased package should miss rather than silently match. */
   ASSERT_TRUE(pastime_external_lookup("COM.FLYCAST.EMULATOR") == NULL);
   ASSERT_TRUE(pastime_external_lookup("Com.Flycast.Emulator") == NULL);
}

static void test_preset_fields_are_populated(void)
{
   const pastime_external_spec_t *flycast =
      pastime_external_lookup("com.flycast.emulator");
   ASSERT_TRUE(flycast != NULL);
   if (flycast)
   {
      /* Flycast is a VIEW + setData entry — extra_key MUST be NULL or
       * we'd be putExtra'ing the URI instead of setting it as data. */
      ASSERT_TRUE(flycast->extra_key == NULL);
      ASSERT_TRUE(flycast->action != NULL);
      ASSERT_STREQ(flycast->action, "android.intent.action.VIEW");
      ASSERT_TRUE(flycast->component != NULL);
   }

   const pastime_external_spec_t *dolphin =
      pastime_external_lookup("org.dolphinemu.dolphinemu");
   ASSERT_TRUE(dolphin != NULL);
   if (dolphin)
   {
      /* Dolphin uses MAIN + AutoStartFile extra. */
      ASSERT_TRUE(dolphin->extra_key != NULL);
      ASSERT_STREQ(dolphin->extra_key, "AutoStartFile");
   }
}

/* ---------- launch stub on non-Android ---------- */

#ifndef ANDROID
static void test_launch_is_noop_on_desktop(void)
{
   const pastime_external_spec_t *flycast =
      pastime_external_lookup("com.flycast.emulator");
   ASSERT_TRUE(flycast != NULL);
   if (flycast)
   {
      g_log_count = 0;
      ASSERT_FALSE(pastime_external_launch(flycast, "/tmp/fake.gdi"));
      ASSERT_TRUE(g_log_count > 0); /* logged the no-op */
   }
}
#endif

static void test_launch_validates_inputs(void)
{
   const pastime_external_spec_t *flycast =
      pastime_external_lookup("com.flycast.emulator");
   ASSERT_FALSE(pastime_external_launch(NULL, "/tmp/x"));
   ASSERT_FALSE(pastime_external_launch(flycast, NULL));
   ASSERT_FALSE(pastime_external_launch(flycast, ""));
}

int main(void)
{
   RUN_TEST(test_parse_marker_accepts_well_formed_packages);
   RUN_TEST(test_parse_marker_rejects_non_ext_prefix);
   RUN_TEST(test_parse_marker_requires_dot);
   RUN_TEST(test_parse_marker_rejects_bad_chars);
   RUN_TEST(test_parse_marker_rejects_empty_payload);
   RUN_TEST(test_lookup_finds_known_packages);
   RUN_TEST(test_lookup_misses_return_null);
   RUN_TEST(test_lookup_is_case_sensitive);
   RUN_TEST(test_preset_fields_are_populated);
#ifndef ANDROID
   RUN_TEST(test_launch_is_noop_on_desktop);
#endif
   RUN_TEST(test_launch_validates_inputs);

   fprintf(stderr, "\n%s: %d passed, %d failed\n",
         g_fail ? "FAIL" : "PASS", g_pass, g_fail);
   return g_fail ? 1 : 0;
}
