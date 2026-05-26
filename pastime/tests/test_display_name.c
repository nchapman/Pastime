/* Unit tests for pastime/pastime_display_name.c.
 *
 * Pure string transforms — no stubs needed.  Build line in run_tests.sh.
 */

#include <stdio.h>
#include <string.h>

#include "../pastime_display_name.h"

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
   pastime_display_name_clean(raw, buf, sizeof(buf));
   ASSERT_STR_EQ(buf, want);
}

static void clean_keep_tag_eq(const char *raw,
      const char *want_clean, const char *want_tag)
{
   char clean[128];
   char tag[128];
   pastime_display_name_clean_keep_tag(raw, clean, sizeof(clean),
         tag, sizeof(tag));
   ASSERT_STR_EQ(clean, want_clean);
   ASSERT_STR_EQ(tag,   want_tag);
}

static void sort_eq(const char *display, const char *want)
{
   char buf[128];
   pastime_display_name_sort_key(display, buf, sizeof(buf));
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
   pastime_display_name_clean(
         "Legend of Zelda, The (USA)", first, sizeof(first));
   pastime_display_name_clean(first, second, sizeof(second));
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
   pastime_display_name_clean(NULL, buf, sizeof(buf));
   ASSERT_STR_EQ(buf, "");
}

static void test_clean_null_out_does_not_crash(void)
{
   /* NULL out with nonzero out_size — guard at the top of _clean
    * must short-circuit without dereferencing.  No buffer to inspect;
    * reaching the sentinel proves we returned cleanly. */
   pastime_display_name_clean("Foo (USA)", NULL, 16);
   clean_eq("Foo",                            "Foo");
}

static void test_clean_zero_size_does_not_crash(void)
{
   /* out_size == 0 — same guard, different limb.  Buffer is non-NULL
    * but the size is zero, so writing out[0] would also be invalid.
    * Sentinel byte must remain untouched. */
   char buf[4] = "ok";
   pastime_display_name_clean("Foo (USA)", buf, 0);
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
      pastime_display_name_clean("Foo, The", tight, sizeof(tight));
      ASSERT_STR_EQ(tight, "The Foo");
   }
   {
      /* tight[8]: _clean truncates to "Foo, Th" before rotation can
       * see a complete article token, so the source-form survives
       * (truncated) rather than the rotated form. */
      char tight[8];
      pastime_display_name_clean("Foo, The", tight, sizeof(tight));
      ASSERT_STR_EQ(tight, "Foo, Th");
   }
}

static void test_clean_truncates_oversize_input(void)
{
   /* len >= out_size path in _clean: input is bigger than the buffer
    * — must truncate, not overrun.  Bracket-strip then runs over the
    * truncated copy; that's by design. */
   char tight[8];
   pastime_display_name_clean(
         "Super Mario Bros. 3", tight, sizeof(tight));
   /* First 7 chars of the source, NUL terminated.  Bracket-strip
    * touches nothing here since there's no '(' or '['. */
   ASSERT_STR_EQ(tight, "Super M");
}

static void test_clean_keep_tag_basic(void)
{
   /* Single trailing parens block. */
   clean_keep_tag_eq("Super Mario Bros. 3 (USA)",
         "Super Mario Bros. 3", "(USA)");
   /* Multiple bracketed tags collapse into one tag string. */
   clean_keep_tag_eq("Super Mario Bros. 3 (USA) (Rev A)",
         "Super Mario Bros. 3", "(USA) (Rev A)");
   /* Mixed parens + brackets. */
   clean_keep_tag_eq("Sonic the Hedgehog (USA) [!]",
         "Sonic the Hedgehog", "(USA) [!]");
   /* No tag → empty tag_out, clean unchanged. */
   clean_keep_tag_eq("Super Mario Bros.", "Super Mario Bros.", "");
   /* The disambig case the system-overlay pass needs. */
   clean_keep_tag_eq("Ape Escape (USA)",
         "Ape Escape", "(USA)");
   clean_keep_tag_eq("Ape Escape (USA) (Demo 1)",
         "Ape Escape", "(USA) (Demo 1)");
}

static void test_clean_keep_tag_null_tag_out(void)
{
   /* tag_out NULL must not crash; clean side still works. */
   char buf[64];
   pastime_display_name_clean_keep_tag("Foo (USA)", buf, sizeof(buf),
         NULL, 0);
   ASSERT_STR_EQ(buf, "Foo");
}

static void test_clean_keep_tag_unmatched(void)
{
   /* Unmatched closer at end → not treated as a tag.  Bracket-strip
    * leaves the unmatched fragment alone (per existing semantics) so
    * the cleaned form keeps it; tag_out stays empty. */
   char clean[64];
   char tag[64];
   pastime_display_name_clean_keep_tag("Mystery Game )broken",
         clean, sizeof(clean), tag, sizeof(tag));
   ASSERT_STR_EQ(tag, "");
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
   pastime_display_name_sort_key(NULL, buf, sizeof(buf));
   ASSERT_STR_EQ(buf, "");
}

static void test_sort_null_out_does_not_crash(void)
{
   pastime_display_name_sort_key("The Legend of Zelda", NULL, 16);
   sort_eq("Tetris",                          "tetris");
}

static void test_sort_zero_size_does_not_crash(void)
{
   char buf[4] = "ok";
   pastime_display_name_sort_key("The Legend of Zelda", buf, 0);
   ASSERT_STR_EQ(buf, "ok");
}

static void test_sort_truncates_oversize_input(void)
{
   /* The i + 1 < out_size cap in _sort_key — must NUL-terminate at
    * the boundary, not overrun. */
   char tight[8];
   pastime_display_name_sort_key(
         "The Legend of Zelda", tight, sizeof(tight));
   /* Article "The " stripped, then 7 lowercase chars + NUL. */
   ASSERT_STR_EQ(tight, "legend ");
}

static void strip_eq(const char *raw, const char *want)
{
   char buf[64];
   size_t n = strlen(raw);
   if (n >= sizeof(buf)) n = sizeof(buf) - 1;
   memcpy(buf, raw, n);
   buf[n] = '\0';
   pastime_display_name_strip_rom_extension(buf);
   ASSERT_STR_EQ(buf, want);
}

static void test_strip_rom_extension(void)
{
   /* Common single-extension cases. */
   strip_eq("Tetris.nes",            "Tetris");
   strip_eq("Mario.smc",             "Mario");
   strip_eq("game.zip",              "game");
   /* PICO-8 PNG-encoded cart — the case this exists for. */
   strip_eq("Celeste.p8.png",        "Celeste");
   /* PICO-8 plain-text cart — single ext drop hits ".p8" directly,
    * second peel must NOT fire (no leftover ".p8"). */
   strip_eq("Celeste.p8",            "Celeste");
   /* No extension at all — leave alone. */
   strip_eq("README",                "README");
   /* Hidden-file leading dot is fine — strip removes the trailing
    * extension only. */
   strip_eq("Game.Name.With.Dots.smc", "Game.Name.With.Dots");
   /* The ".p8" peel is anchored — a name that *happens* to end in
    * ".p8" only because the first peel removed something else IS the
    * intended target.  But "snippet.snip" → "snippet" must not
    * spuriously pop more. */
   strip_eq("snippet.snip",          "snippet");
   /* No false positive on names that legitimately end in 'p8' without
    * a preceding dot (the anchor requires a literal '.'). */
   strip_eq("up8.bin",               "up8");
   /* Documenting current behavior: the second peel is anchored to a
    * literal trailing ".p8", not gated on the system context.  A name
    * like "foo.p8.nds" (.p8 sandwiched between a base name and another
    * system's extension) WILL strip to "foo".  Acceptable today since
    * .p8 inside a non-PICO-8 ROM file would be an extremely odd
    * collision, but flag this when adding a second double-extension
    * core (e.g. another system that also uses ".p8.something"). */
   strip_eq("foo.p8.nds",            "foo");
   /* NULL / empty safety. */
   pastime_display_name_strip_rom_extension(NULL);
   {
      char empty[4] = "";
      pastime_display_name_strip_rom_extension(empty);
      ASSERT_STR_EQ(empty, "");
   }
}

/* ---- relative time formatting ---- */

static void reltime_eq(int64_t mtime, int64_t now, const char *want)
{
   char buf[64];
   pastime_format_relative_time(mtime, now, buf, sizeof(buf));
   ASSERT_STR_EQ(buf, want);
}

static void test_reltime_just_now(void)
{
   reltime_eq(1000000, 1000000, "Just now");
   reltime_eq(999941, 1000000, "Just now");
}

static void test_reltime_minutes(void)
{
   reltime_eq(999940, 1000000, "1 minute ago");
   reltime_eq(999100, 1000000, "15 minutes ago");
   reltime_eq(996401, 1000000, "59 minutes ago");
}

static void test_reltime_hours(void)
{
   reltime_eq(996399, 1000000, "1 hour ago");
   reltime_eq(964000, 1000000, "10 hours ago");
}

static void test_reltime_yesterday(void)
{
   reltime_eq(913600, 1000000, "Yesterday");
}

static void test_reltime_days(void)
{
   reltime_eq(827200, 1000000, "2 days ago");
   reltime_eq(100000, 1000000, "10 days ago");
}

static void test_reltime_unknown(void)
{
   reltime_eq(0, 1000000, "Unknown");
   reltime_eq(-1, 1000000, "Unknown");
}

static void test_reltime_future(void)
{
   /* mtime in the future → delta clamped to 0 → "Just now" */
   reltime_eq(1000001, 1000000, "Just now");
}

static void test_reltime_null_safe(void)
{
   pastime_format_relative_time(100, 200, NULL, 0);
   g_pass++;
}

/* ---- sort prefix stripping ---- */

static void prefix_eq(const char *input, const char *want)
{
   const char *got = pastime_display_name_strip_sort_prefix(input);
   ASSERT_STR_EQ(got, want);
}

static void test_sort_prefix_basic(void)
{
   prefix_eq("1) Game Title",    "Game Title");
   prefix_eq("01) Game Boy",     "Game Boy");
   prefix_eq("123) Arcade",      "Arcade");
   prefix_eq("9) X",             "X");
}

static void test_sort_prefix_no_space(void)
{
   prefix_eq("1)Game",           "Game");
   prefix_eq("99)Tight",         "Tight");
}

static void test_sort_prefix_tab_separator(void)
{
   prefix_eq("1)\tTabbed",       "Tabbed");
}

static void test_sort_prefix_no_prefix(void)
{
   prefix_eq("Game Title",       "Game Title");
   prefix_eq("No Prefix",        "No Prefix");
   prefix_eq("",                  "");
}

static void test_sort_prefix_missing_paren(void)
{
   prefix_eq("1 Game",           "1 Game");
   prefix_eq("12 Foo",           "12 Foo");
}

static void test_sort_prefix_only_prefix(void)
{
   prefix_eq("1) ",              "");
   prefix_eq("1)",               "");
}

static void test_sort_prefix_null(void)
{
   const char *got = pastime_display_name_strip_sort_prefix(NULL);
   g_pass++;
   (void)got;
}

static void test_sort_prefix_non_digit_start(void)
{
   prefix_eq("A) Not a number",  "A) Not a number");
   prefix_eq(") No digits",      ") No digits");
}

static void test_sort_prefix_multi_space(void)
{
   prefix_eq("1)   Lots of space", "Lots of space");
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
   test_clean_keep_tag_basic();
   test_clean_keep_tag_null_tag_out();
   test_clean_keep_tag_unmatched();
   test_sort_lowercase();
   test_sort_strips_articles();
   test_sort_empty();
   test_sort_null_input();
   test_sort_null_out_does_not_crash();
   test_sort_zero_size_does_not_crash();
   test_sort_truncates_oversize_input();
   test_strip_rom_extension();
   test_reltime_just_now();
   test_reltime_minutes();
   test_reltime_hours();
   test_reltime_yesterday();
   test_reltime_days();
   test_reltime_unknown();
   test_reltime_future();
   test_reltime_null_safe();
   test_sort_prefix_basic();
   test_sort_prefix_no_space();
   test_sort_prefix_tab_separator();
   test_sort_prefix_no_prefix();
   test_sort_prefix_missing_paren();
   test_sort_prefix_only_prefix();
   test_sort_prefix_null();
   test_sort_prefix_non_digit_start();
   test_sort_prefix_multi_space();

   printf("test_display_name: %d passed, %d failed\n", g_pass, g_fail);
   return g_fail ? 1 : 0;
}
