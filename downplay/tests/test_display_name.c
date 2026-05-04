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
   clean_eq("Tetris",                         "Tetris");
   clean_eq("",                               "");
}

static void test_clean_unmatched_paren(void)
{
   /* Don't gobble the rest of the string on a stray opener. */
   clean_eq("Foo (bar",                       "Foo (bar");
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

int main(void)
{
   test_clean_strips_parens();
   test_clean_strips_brackets();
   test_clean_mixed();
   test_clean_rotates_trailing_article();
   test_clean_idempotent();
   test_clean_unmatched_paren();
   test_sort_lowercase();
   test_sort_strips_articles();
   test_sort_empty();

   printf("test_display_name: %d passed, %d failed\n", g_pass, g_fail);
   return g_fail ? 1 : 0;
}
