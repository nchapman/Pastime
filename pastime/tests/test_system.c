/* Unit tests for pastime_parse_system_folder().
 *
 * Links against the real pastime_system.c and pastime_external.c
 * (with PASTIME_EXTERNAL_TEST_BUILD + PASTIME_SYSTEM_TEST_BUILD). */

#ifndef PASTIME_EXTERNAL_TEST_BUILD
#define PASTIME_EXTERNAL_TEST_BUILD
#endif
#ifndef PASTIME_SYSTEM_TEST_BUILD
#define PASTIME_SYSTEM_TEST_BUILD
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

/* Shared log stub for both modules' test builds */
void pastime_external_test_log(const char *fmt, ...) { (void)fmt; }
void pastime_system_test_log(const char *fmt, ...) { (void)fmt; }

#include "../pastime_external.h"
#include "../pastime_external.c"
#include "../pastime_system.h"
#include "../pastime_system.c"

/* ------------------------------------------------------------------ */
/* Tiny test framework                                                 */
/* ------------------------------------------------------------------ */
static int g_tests_run    = 0;
static int g_tests_passed = 0;

#define RUN_TEST(fn) do { \
   g_tests_run++; \
   if (fn()) g_tests_passed++; \
   else fprintf(stderr, "  FAIL: %s\n", #fn); \
} while (0)

#define ASSERT_TRUE(expr) do { \
   if (!(expr)) { \
      fprintf(stderr, "    assertion failed: %s (%s:%d)\n", \
            #expr, __FILE__, __LINE__); \
      return 0; \
   } \
} while (0)

#define ASSERT_FALSE(expr) ASSERT_TRUE(!(expr))
#define ASSERT_NULL(p) ASSERT_TRUE((p) == NULL)
#define ASSERT_NOT_NULL(p) ASSERT_TRUE((p) != NULL)
#define ASSERT_STR_EQ(a, b) do { \
   const char *_a = (a), *_b = (b); \
   if (!_a || !_b || strcmp(_a, _b) != 0) { \
      fprintf(stderr, "    strings differ: \"%s\" vs \"%s\" (%s:%d)\n", \
            _a ? _a : "(null)", _b ? _b : "(null)", \
            __FILE__, __LINE__); \
      return 0; \
   } \
} while (0)

/* ------------------------------------------------------------------ */
/* Tests                                                               */
/* ------------------------------------------------------------------ */

static int test_basic_libretro_core(void)
{
   char *display = NULL, *ident = NULL;
   const pastime_external_spec_t *ext = NULL;

   ASSERT_TRUE(pastime_parse_system_folder(
         "Super Nintendo (snes9x)", &display, &ident, &ext));
   ASSERT_STR_EQ(display, "Super Nintendo");
   ASSERT_STR_EQ(ident, "snes9x");
   ASSERT_NULL(ext);

   free(display);
   free(ident);
   return 1;
}

static int test_core_with_underscores_and_digits(void)
{
   char *display = NULL, *ident = NULL;
   const pastime_external_spec_t *ext = NULL;

   ASSERT_TRUE(pastime_parse_system_folder(
         "Nintendo 64 (mupen64plus_next)", &display, &ident, &ext));
   ASSERT_STR_EQ(display, "Nintendo 64");
   ASSERT_STR_EQ(ident, "mupen64plus_next");
   ASSERT_NULL(ext);

   free(display);
   free(ident);
   return 1;
}

static int test_display_name_with_parens(void)
{
   char *display = NULL, *ident = NULL;
   const pastime_external_spec_t *ext = NULL;

   ASSERT_TRUE(pastime_parse_system_folder(
         "Game Boy Advance (hacks) (mgba)", &display, &ident, &ext));
   ASSERT_STR_EQ(display, "Game Boy Advance (hacks)");
   ASSERT_STR_EQ(ident, "mgba");

   free(display);
   free(ident);
   return 1;
}

static int test_missing_closing_paren(void)
{
   char *display = NULL, *ident = NULL;
   const pastime_external_spec_t *ext = NULL;

   ASSERT_FALSE(pastime_parse_system_folder(
         "No Closing (paren", &display, &ident, &ext));
   return 1;
}

static int test_no_space_before_paren(void)
{
   char *display = NULL, *ident = NULL;
   const pastime_external_spec_t *ext = NULL;

   ASSERT_FALSE(pastime_parse_system_folder(
         "NoSpace(core)", &display, &ident, &ext));
   return 1;
}

static int test_empty_ident(void)
{
   char *display = NULL, *ident = NULL;
   const pastime_external_spec_t *ext = NULL;

   ASSERT_FALSE(pastime_parse_system_folder(
         "Empty ()", &display, &ident, &ext));
   return 1;
}

static int test_empty_display(void)
{
   char *display = NULL, *ident = NULL;
   const pastime_external_spec_t *ext = NULL;

   ASSERT_FALSE(pastime_parse_system_folder(
         " (core)", &display, &ident, &ext));
   return 1;
}

static int test_null_input(void)
{
   char *display = NULL, *ident = NULL;
   const pastime_external_spec_t *ext = NULL;

   ASSERT_FALSE(pastime_parse_system_folder(NULL, &display, &ident, &ext));
   return 1;
}

static int test_empty_string(void)
{
   char *display = NULL, *ident = NULL;
   const pastime_external_spec_t *ext = NULL;

   ASSERT_FALSE(pastime_parse_system_folder("", &display, &ident, &ext));
   return 1;
}

static int test_too_short(void)
{
   char *display = NULL, *ident = NULL;
   const pastime_external_spec_t *ext = NULL;

   ASSERT_FALSE(pastime_parse_system_folder("a()", &display, &ident, &ext));
   return 1;
}

static int test_invalid_ident_uppercase(void)
{
   char *display = NULL, *ident = NULL;
   const pastime_external_spec_t *ext = NULL;

   ASSERT_FALSE(pastime_parse_system_folder(
         "Game (Snes9x)", &display, &ident, &ext));
   return 1;
}

static int test_invalid_ident_hyphen(void)
{
   char *display = NULL, *ident = NULL;
   const pastime_external_spec_t *ext = NULL;

   ASSERT_FALSE(pastime_parse_system_folder(
         "Game (some-core)", &display, &ident, &ext));
   return 1;
}

static int test_invalid_ident_space(void)
{
   char *display = NULL, *ident = NULL;
   const pastime_external_spec_t *ext = NULL;

   ASSERT_FALSE(pastime_parse_system_folder(
         "Game (two words)", &display, &ident, &ext));
   return 1;
}

static int test_external_known_package(void)
{
   char *display = NULL, *ident = NULL;
   const pastime_external_spec_t *ext = NULL;

   ASSERT_TRUE(pastime_parse_system_folder(
         "Dreamcast (ext-flycast)", &display, &ident, &ext));
   ASSERT_STR_EQ(display, "Dreamcast");
   ASSERT_NULL(ident);
   ASSERT_NOT_NULL(ext);

   free(display);
   return 1;
}

static int test_external_unknown_package(void)
{
   char *display = NULL, *ident = NULL;
   const pastime_external_spec_t *ext = NULL;

   ASSERT_FALSE(pastime_parse_system_folder(
         "Unknown (ext-com.nonexistent.app)", &display, &ident, &ext));
   return 1;
}

static int test_single_char_display(void)
{
   char *display = NULL, *ident = NULL;
   const pastime_external_spec_t *ext = NULL;

   ASSERT_TRUE(pastime_parse_system_folder(
         "X (mgba)", &display, &ident, &ext));
   ASSERT_STR_EQ(display, "X");
   ASSERT_STR_EQ(ident, "mgba");

   free(display);
   free(ident);
   return 1;
}

static int test_single_char_ident(void)
{
   char *display = NULL, *ident = NULL;
   const pastime_external_spec_t *ext = NULL;

   ASSERT_TRUE(pastime_parse_system_folder(
         "Game (a)", &display, &ident, &ext));
   ASSERT_STR_EQ(display, "Game");
   ASSERT_STR_EQ(ident, "a");

   free(display);
   free(ident);
   return 1;
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

int main(void)
{
   printf("== pastime_system tests ==\n\n");

   RUN_TEST(test_basic_libretro_core);
   RUN_TEST(test_core_with_underscores_and_digits);
   RUN_TEST(test_display_name_with_parens);
   RUN_TEST(test_missing_closing_paren);
   RUN_TEST(test_no_space_before_paren);
   RUN_TEST(test_empty_ident);
   RUN_TEST(test_empty_display);
   RUN_TEST(test_null_input);
   RUN_TEST(test_empty_string);
   RUN_TEST(test_too_short);
   RUN_TEST(test_invalid_ident_uppercase);
   RUN_TEST(test_invalid_ident_hyphen);
   RUN_TEST(test_invalid_ident_space);
   RUN_TEST(test_external_known_package);
   RUN_TEST(test_external_unknown_package);
   RUN_TEST(test_single_char_display);
   RUN_TEST(test_single_char_ident);

   printf("\n%d/%d passed\n", g_tests_passed, g_tests_run);
   return g_tests_passed == g_tests_run ? 0 : 1;
}
