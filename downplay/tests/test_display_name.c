/* Unit tests for downplay/downplay_display_name.c.
 *
 * Pure string transforms — no stubs needed.  Build line in run_tests.sh.
 */

#include <stdio.h>
#include <string.h>

#include "../downplay_display_name.h"

static int g_pass;
static int g_fail;

#define ASSERT_STR_EQ(a, b) do { \
   const char *_va = (a); \
   const char *_vb = (b); \
   if (_va && _vb && strcmp(_va, _vb) == 0) { g_pass++; } \
   else { g_fail++; \
      fprintf(stderr, "    FAIL %s:%d  %s == %s  (\"%s\" != \"%s\")\n", \
            __FILE__, __LINE__, #a, #b, \
            _va ? _va : "(null)", _vb ? _vb : "(null)"); } \
} while (0)

static void clean_eq(const char *raw, const char *want)
{
   char buf[128];
   downplay_display_name_clean(raw, buf, sizeof(buf));
   ASSERT_STR_EQ(buf, want);
}

static void sort_eq(const char *display, const char *want)
{
   char buf[128];
   downplay_display_name_sort_key(display, buf, sizeof(buf));
   ASSERT_STR_EQ(buf, want);
}

static void test_clean_strips_parens(void)
{
   clean_eq("Super Mario Bros. 3 (USA)",      "Super Mario Bros. 3");
   clean_eq("Donkey Kong (Japan, USA) (SGB Enhanced)", "Donkey Kong");
   clean_eq("Final Fantasy VII (USA) (Disc 1)", "Final Fantasy VII");
   clean_eq("Pokemon Red (Rev 1)",            "Pokemon Red");
}

static void test_clean_strips_brackets(void)
{
   clean_eq("Some Game [!]",                  "Some Game");
   clean_eq("Game [b1][T+Eng]",               "Game");
}

static void test_clean_mixed(void)
{
   clean_eq("Game (USA) [b1]",                "Game");
}

static void test_clean_rotates_trailing_article(void)
{
   clean_eq("Legend of Zelda, The (USA)",
         "The Legend of Zelda");
   /* Article is in the middle, before a " - subtitle" — rotate it
    * to the front, leave the subtitle intact. */
   clean_eq("Legend of Zelda, The - Oracle of Ages (USA)",
         "The Legend of Zelda - Oracle of Ages");
   clean_eq("Bug's Life, A",                  "A Bug's Life");
   clean_eq("American Tail, An",              "An American Tail");
   /* Lowercase article in source still produces canonical capitalization,
    * both at end-of-string and through the subtitle branch. */
   clean_eq("Foo, the",                       "The Foo");
   clean_eq("Legend of Zelda, the - Oracle of Ages (USA)",
         "The Legend of Zelda - Oracle of Ages");
   /* No false positives — "Antonio" is not "An". */
   clean_eq("Foo, Antonio",                   "Foo, Antonio");
}

static void test_clean_idempotent(void)
{
   /* Re-feed the output of a non-trivial case and assert it's stable
    * — the header explicitly promises idempotence. */
   char first[128];
   char second[128];
   downplay_display_name_clean(
         "Legend of Zelda, The (USA)", first, sizeof(first));
   downplay_display_name_clean(first, second, sizeof(second));
   ASSERT_STR_EQ(first,  "The Legend of Zelda");
   ASSERT_STR_EQ(second, "The Legend of Zelda");
   /* Already-canonical inputs round-trip too. */
   clean_eq("Tetris",                         "Tetris");
   clean_eq("",                               "");
}

static void test_clean_unmatched_paren(void)
{
   /* Don't gobble the rest of the string on a stray opener. */
   clean_eq("Foo (bar",                       "Foo (bar");
}

static void test_clean_null_input(void)
{
   /* Header explicitly promises NULL safety. */
   char buf[16] = "preserved";
   downplay_display_name_clean(NULL, buf, sizeof(buf));
   ASSERT_STR_EQ(buf, "");
}

static void test_clean_null_out_does_not_crash(void)
{
   /* NULL out with nonzero out_size — guard at the top of _clean
    * must short-circuit without dereferencing.  No buffer to inspect;
    * reaching the sentinel proves we returned cleanly. */
   downplay_display_name_clean("Foo (USA)", NULL, 16);
   clean_eq("Foo",                            "Foo");
}

static void test_clean_zero_size_does_not_crash(void)
{
   /* out_size == 0 — same guard, different limb.  Buffer is non-NULL
    * but the size is zero, so writing out[0] would also be invalid.
    * Sentinel byte must remain untouched. */
   char buf[4] = "ok";
   downplay_display_name_clean("Foo (USA)", buf, 0);
   ASSERT_STR_EQ(buf, "ok");
}

static void test_clean_rotation_at_buffer_boundary(void)
{
   /* The rotation pipeline is _clean → strip_brackets → rstrip →
    * rotate_article.  _clean's `len >= out_size` truncation runs
    * FIRST, so if the source string itself doesn't fit, it gets
    * cropped before rotate_article ever sees it.  For "Foo, The"
    * (8 chars + NUL = 9 bytes) the boundary is therefore out_size = 9,
    * not the rotation guard's smaller threshold inside the function. */
   {
      /* tight[9]: source fits exactly → rotation runs → "The Foo". */
      char tight[9];
      downplay_display_name_clean("Foo, The", tight, sizeof(tight));
      ASSERT_STR_EQ(tight, "The Foo");
   }
   {
      /* tight[8]: _clean truncates to "Foo, Th" before rotation can
       * see a complete article token, so the source-form survives
       * (truncated) rather than the rotated form. */
      char tight[8];
      downplay_display_name_clean("Foo, The", tight, sizeof(tight));
      ASSERT_STR_EQ(tight, "Foo, Th");
   }
}

static void test_clean_truncates_oversize_input(void)
{
   /* len >= out_size path in _clean: input is bigger than the buffer
    * — must truncate, not overrun.  Bracket-strip then runs over the
    * truncated copy; that's by design. */
   char tight[8];
   downplay_display_name_clean(
         "Super Mario Bros. 3", tight, sizeof(tight));
   /* First 7 chars of the source, NUL terminated.  Bracket-strip
    * touches nothing here since there's no '(' or '['. */
   ASSERT_STR_EQ(tight, "Super M");
}

static void test_clean_folder_name_convention(void)
{
   /* The launcher feeds folder names like "GBA (mgba)" through
    * dp_strip_brackets to produce a clean display row.  This is the
    * docstring-promised use case but wasn't covered. */
   clean_eq("GBA (mgba)",                     "GBA");
   clean_eq("Super Nintendo Entertainment System (snes9x)",
         "Super Nintendo Entertainment System");
   clean_eq("Game Boy Advance (mgba)",        "Game Boy Advance");
}

static void test_sort_lowercase(void)
{
   sort_eq("Super Mario Bros.",               "super mario bros.");
}

static void test_sort_strips_articles(void)
{
   sort_eq("The Legend of Zelda",             "legend of zelda");
   sort_eq("A Bug's Life",                    "bug's life");
   sort_eq("An American Tail",                "american tail");
   /* Internal "the" untouched. */
   sort_eq("Curse of the Moon",               "curse of the moon");
}

static void test_sort_empty(void)
{
   sort_eq("",                                "");
}

static void test_sort_null_input(void)
{
   /* Header explicitly promises NULL safety. */
   char buf[16] = "preserved";
   downplay_display_name_sort_key(NULL, buf, sizeof(buf));
   ASSERT_STR_EQ(buf, "");
}

static void test_sort_null_out_does_not_crash(void)
{
   downplay_display_name_sort_key("The Legend of Zelda", NULL, 16);
   sort_eq("Tetris",                          "tetris");
}

static void test_sort_zero_size_does_not_crash(void)
{
   char buf[4] = "ok";
   downplay_display_name_sort_key("The Legend of Zelda", buf, 0);
   ASSERT_STR_EQ(buf, "ok");
}

static void test_sort_truncates_oversize_input(void)
{
   /* The i + 1 < out_size cap in _sort_key — must NUL-terminate at
    * the boundary, not overrun. */
   char tight[8];
   downplay_display_name_sort_key(
         "The Legend of Zelda", tight, sizeof(tight));
   /* Article "The " stripped, then 7 lowercase chars + NUL. */
   ASSERT_STR_EQ(tight, "legend ");
}

int main(void)
{
   test_clean_strips_parens();
   test_clean_strips_brackets();
   test_clean_mixed();
   test_clean_rotates_trailing_article();
   test_clean_idempotent();
   test_clean_unmatched_paren();
   test_clean_null_input();
   test_clean_null_out_does_not_crash();
   test_clean_zero_size_does_not_crash();
   test_clean_rotation_at_buffer_boundary();
   test_clean_truncates_oversize_input();
   test_clean_folder_name_convention();
   test_sort_lowercase();
   test_sort_strips_articles();
   test_sort_empty();
   test_sort_null_input();
   test_sort_null_out_does_not_crash();
   test_sort_zero_size_does_not_crash();
   test_sort_truncates_oversize_input();

   printf("test_display_name: %d passed, %d failed\n", g_pass, g_fail);
   return g_fail ? 1 : 0;
}
