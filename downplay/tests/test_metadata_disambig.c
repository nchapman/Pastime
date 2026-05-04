/* Unit tests for downplay/downplay_metadata_disambig.c — the
 * "Display Name (core_ident)" → libretro-thumbnails system-name
 * resolver.
 *
 * Links against the real production source — no carbon copy of the
 * disambiguation table here.  See run_tests.sh for the build line.
 *
 * Two external symbols the production source needs at link time:
 *   - core_info_find: stubbed below to always return false (we test
 *     the table-walking path; the core_info fallback is exercised
 *     end-to-end via real device runs).  This is also how RA's own
 *     core_info_find behaves before core_info is initialised
 *     (core_info.c:2507), so it's a faithful no-op rather than a lie.
 *   - core_info_t struct shape: pulled in via the real core_info.h
 *     so this stays in sync with upstream.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../downplay_metadata.h"
#include "../../core_info.h"

/* ---------- core_info_find stub ---------- */

bool core_info_find(const char *core_path, core_info_t **info)
{
   (void)core_path;
   if (info)
      *info = NULL;
   return false;
}

/* ---------- test framework ---------- */

static int g_pass;
static int g_fail;
static int g_test_pass_at_start;
static int g_test_fail_at_start;

#define ASSERT_TRUE(cond) do { \
   if (cond) { g_pass++; } \
   else { g_fail++; \
      fprintf(stderr, "    FAIL %s:%d  %s\n", \
            __FILE__, __LINE__, #cond); } \
} while (0)

#define ASSERT_STR_EQ(a, b) do { \
   const char *_va = (a); \
   const char *_vb = (b); \
   if (_va && _vb && strcmp(_va, _vb) == 0) { g_pass++; } \
   else { g_fail++; \
      fprintf(stderr, "    FAIL %s:%d  %s == %s  (\"%s\" != \"%s\")\n", \
            __FILE__, __LINE__, #a, #b, \
            _va ? _va : "(null)", _vb ? _vb : "(null)"); } \
} while (0)

#define ASSERT_NULL(p) do { \
   if ((p) == NULL) { g_pass++; } \
   else { g_fail++; \
      fprintf(stderr, "    FAIL %s:%d  %s is not NULL  (\"%s\")\n", \
            __FILE__, __LINE__, #p, (const char*)(p)); } \
} while (0)

#define RUN_TEST(fn) do { \
   g_test_pass_at_start = g_pass; \
   g_test_fail_at_start = g_fail; \
   fn(); \
   printf("  %-60s  %s  (+%d -%d)\n", #fn, \
         (g_fail == g_test_fail_at_start) ? "ok  " : "FAIL", \
         g_pass - g_test_pass_at_start, \
         g_fail - g_test_fail_at_start); \
} while (0)

/* Convenience: resolve and assert on the result. */
static const char *resolve(const char *display)
{
   /* core_ident NULL → table-only path; the stub returns false on the
    * fallback, so misses come back as NULL. */
   return downplay_metadata_resolve_db_name(display, NULL);
}

/* ============================================================
 * Tests
 * ============================================================ */

/* ---- canonical names match ---- */

static void test_canonical_full_names(void)
{
   /* The "human-readable, full" form for every system in the table —
    * if these break, an alias rename or a typo got introduced. */
   ASSERT_STR_EQ(resolve("Nintendo Entertainment System"),
         "Nintendo - Nintendo Entertainment System");
   ASSERT_STR_EQ(resolve("Super Nintendo Entertainment System"),
         "Nintendo - Super Nintendo Entertainment System");
   ASSERT_STR_EQ(resolve("Nintendo 64"),
         "Nintendo - Nintendo 64");
   ASSERT_STR_EQ(resolve("Game Boy"),
         "Nintendo - Game Boy");
   ASSERT_STR_EQ(resolve("Game Boy Color"),
         "Nintendo - Game Boy Color");
   ASSERT_STR_EQ(resolve("Game Boy Advance"),
         "Nintendo - Game Boy Advance");
   ASSERT_STR_EQ(resolve("Nintendo DS"),
         "Nintendo - Nintendo DS");
   ASSERT_STR_EQ(resolve("GameCube"),
         "Nintendo - GameCube");
   ASSERT_STR_EQ(resolve("Virtual Boy"),
         "Nintendo - Virtual Boy");
   ASSERT_STR_EQ(resolve("Wii"),
         "Nintendo - Wii");
   ASSERT_STR_EQ(resolve("Sega Genesis"),
         "Sega - Mega Drive - Genesis");
   ASSERT_STR_EQ(resolve("Master System"),
         "Sega - Master System - Mark III");
   ASSERT_STR_EQ(resolve("Game Gear"),
         "Sega - Game Gear");
   ASSERT_STR_EQ(resolve("Saturn"),
         "Sega - Saturn");
   ASSERT_STR_EQ(resolve("Dreamcast"),
         "Sega - Dreamcast");
   ASSERT_STR_EQ(resolve("Sega CD"),
         "Sega - Mega-CD - Sega CD");
   ASSERT_STR_EQ(resolve("PC Engine"),
         "NEC - PC Engine - TurboGrafx 16");
   ASSERT_STR_EQ(resolve("PlayStation"),
         "Sony - PlayStation");
   ASSERT_STR_EQ(resolve("PlayStation Portable"),
         "Sony - PlayStation Portable");
   ASSERT_STR_EQ(resolve("Atari 2600"),
         "Atari - 2600");
   ASSERT_STR_EQ(resolve("Atari Lynx"),
         "Atari - Lynx");
   ASSERT_STR_EQ(resolve("Atari Jaguar"),
         "Atari - Jaguar");
   ASSERT_STR_EQ(resolve("Neo Geo Pocket"),
         "SNK - Neo Geo Pocket");
   ASSERT_STR_EQ(resolve("Neo Geo Pocket Color"),
         "SNK - Neo Geo Pocket Color");
   ASSERT_STR_EQ(resolve("WonderSwan"),
         "Bandai - WonderSwan");
   ASSERT_STR_EQ(resolve("WonderSwan Color"),
         "Bandai - WonderSwan Color");
   ASSERT_STR_EQ(resolve("MAME"),
         "MAME");
   ASSERT_STR_EQ(resolve("DOS"),
         "DOS");
}

/* ---- common abbreviations match the same db_name ---- */

static void test_short_form_abbreviations(void)
{
   /* The abbreviations users type at the keyboard — the whole point of
    * having a table rather than just trusting core_info. */
   ASSERT_STR_EQ(resolve("NES"),
         "Nintendo - Nintendo Entertainment System");
   ASSERT_STR_EQ(resolve("SNES"),
         "Nintendo - Super Nintendo Entertainment System");
   ASSERT_STR_EQ(resolve("N64"),
         "Nintendo - Nintendo 64");
   ASSERT_STR_EQ(resolve("GB"),
         "Nintendo - Game Boy");
   ASSERT_STR_EQ(resolve("GBC"),
         "Nintendo - Game Boy Color");
   ASSERT_STR_EQ(resolve("GBA"),
         "Nintendo - Game Boy Advance");
   ASSERT_STR_EQ(resolve("DS"),
         "Nintendo - Nintendo DS");
   ASSERT_STR_EQ(resolve("PSX"),
         "Sony - PlayStation");
   ASSERT_STR_EQ(resolve("PS1"),
         "Sony - PlayStation");
   ASSERT_STR_EQ(resolve("PSP"),
         "Sony - PlayStation Portable");
   ASSERT_STR_EQ(resolve("VCS"),
         "Atari - 2600");
}

/* ---- regional / alternate names alias to the same db_name ---- */

static void test_regional_aliases_collapse(void)
{
   /* Same hardware, different names by region or generation.  All
    * three Game Boy variants and the multiple Genesis spellings are
    * the most-likely-to-bite cases. */
   const char *snes = "Nintendo - Super Nintendo Entertainment System";
   ASSERT_STR_EQ(resolve("SNES"), snes);
   ASSERT_STR_EQ(resolve("Super Nintendo"), snes);
   ASSERT_STR_EQ(resolve("Super Nintendo Entertainment System"), snes);
   ASSERT_STR_EQ(resolve("Super Famicom"), snes);
   ASSERT_STR_EQ(resolve("SFC"), snes);

   {
      const char *genesis = "Sega - Mega Drive - Genesis";
      ASSERT_STR_EQ(resolve("Genesis"),       genesis);
      ASSERT_STR_EQ(resolve("Sega Genesis"),  genesis);
      ASSERT_STR_EQ(resolve("Mega Drive"),    genesis);
      ASSERT_STR_EQ(resolve("Sega Mega Drive"), genesis);
      ASSERT_STR_EQ(resolve("MD"),            genesis);
   }

   {
      const char *nes = "Nintendo - Nintendo Entertainment System";
      ASSERT_STR_EQ(resolve("NES"),     nes);
      ASSERT_STR_EQ(resolve("Famicom"), nes);
      ASSERT_STR_EQ(resolve("FC"),      nes);
   }

   /* GB / GBC / GBA must NOT collapse — they're distinct repos with
    * distinct art.  Multi-system cores like mgba make this critical. */
   ASSERT_TRUE(resolve("Game Boy")
         != resolve("Game Boy Color"));
   ASSERT_TRUE(resolve("Game Boy")
         != resolve("Game Boy Advance"));
   ASSERT_TRUE(resolve("Game Boy Color")
         != resolve("Game Boy Advance"));
}

/* ---- case insensitivity ---- */

static void test_case_insensitive(void)
{
   const char *snes = "Nintendo - Super Nintendo Entertainment System";
   ASSERT_STR_EQ(resolve("snes"),    snes);
   ASSERT_STR_EQ(resolve("Snes"),    snes);
   ASSERT_STR_EQ(resolve("sNeS"),    snes);
   ASSERT_STR_EQ(resolve("SNES"),    snes);

   ASSERT_STR_EQ(resolve("game boy"),
         "Nintendo - Game Boy");
   ASSERT_STR_EQ(resolve("GAME BOY ADVANCE"),
         "Nintendo - Game Boy Advance");
   ASSERT_STR_EQ(resolve("playstation"),
         "Sony - PlayStation");
}

/* ---- exact-match-only: substrings and partials don't hit ---- */

static void test_no_substring_matches(void)
{
   /* "Super Nintendo" has 'Nintendo' as a substring — must not
    * accidentally resolve via the NES alias.  Same for the inverse. */
   ASSERT_TRUE(resolve("Nintendo") == NULL);
   ASSERT_TRUE(resolve("Super") == NULL);
   /* And the other direction — extending an alias must not match. */
   ASSERT_TRUE(resolve("SNES Mini") == NULL);
   ASSERT_TRUE(resolve("Game Boy Advance SP") == NULL);
}

/* ---- input edge cases ---- */

static void test_null_or_empty_input(void)
{
   /* Both NULL and "" should fall through to the core_info fallback,
    * which our stubbed core_info_find returns false for → NULL. */
   ASSERT_NULL(downplay_metadata_resolve_db_name(NULL, NULL));
   ASSERT_NULL(downplay_metadata_resolve_db_name("",   NULL));
   ASSERT_NULL(downplay_metadata_resolve_db_name(NULL, ""));
   ASSERT_NULL(downplay_metadata_resolve_db_name("",   ""));
}

static void test_unknown_display_returns_null(void)
{
   /* No alias matches; core_ident NULL → no fallback path either. */
   ASSERT_NULL(resolve("FloofyVision 9000"));
   ASSERT_NULL(resolve("CD-i"));        /* not in our table — Phase-2 wishlist */
   ASSERT_NULL(resolve("3DO"));         /* not in our table — Phase-2 wishlist */
}

static void test_unknown_display_with_unknown_core(void)
{
   /* Falls through to core_info_find which is stubbed to false. */
   ASSERT_NULL(downplay_metadata_resolve_db_name(
         "FloofyVision 9000", "totally_made_up_core"));
   ASSERT_NULL(downplay_metadata_resolve_db_name(
         "Random Folder", "snes9x"));   /* even with a real core ident */
}

static void test_table_path_short_circuits_core_info(void)
{
   /* When the display_name is in the table, core_ident is irrelevant.
    * Pass a deliberately wrong core_ident: the SNES table entry should
    * still win, the stubbed core_info_find should never matter. */
   ASSERT_STR_EQ(downplay_metadata_resolve_db_name(
         "SNES", "totally_wrong_core_ident"),
         "Nintendo - Super Nintendo Entertainment System");
}

/* ---- whitespace / punctuation are NOT normalized ---- */

static void test_no_whitespace_normalization(void)
{
   /* Trailing space must not match — keeps the table semantics
    * predictable.  If the user creates a folder named "SNES  "
    * (trailing space), they'll get an honest miss rather than a
    * silent match.  Folder convention strips trailing spaces during
    * parse so this rarely bites, but the behaviour is asserted. */
   ASSERT_NULL(resolve("SNES "));
   ASSERT_NULL(resolve(" SNES"));
   ASSERT_NULL(resolve("SNES\t"));
}

/* ---- table is exhaustive enough for common cores we ship ---- */

static void test_coverage_for_common_cores(void)
{
   /* Sanity check that every "common" system folder name a user is
    * likely to type lands in the table.  This catches accidental
    * deletions during refactors more reliably than counting entries. */
   ASSERT_TRUE(resolve("NES")        != NULL);
   ASSERT_TRUE(resolve("SNES")       != NULL);
   ASSERT_TRUE(resolve("N64")        != NULL);
   ASSERT_TRUE(resolve("GB")         != NULL);
   ASSERT_TRUE(resolve("GBC")        != NULL);
   ASSERT_TRUE(resolve("GBA")        != NULL);
   ASSERT_TRUE(resolve("DS")         != NULL);
   ASSERT_TRUE(resolve("Genesis")    != NULL);
   ASSERT_TRUE(resolve("Master System") != NULL);
   ASSERT_TRUE(resolve("Game Gear")  != NULL);
   ASSERT_TRUE(resolve("Saturn")     != NULL);
   ASSERT_TRUE(resolve("Dreamcast")  != NULL);
   ASSERT_TRUE(resolve("PSX")        != NULL);
   ASSERT_TRUE(resolve("PSP")        != NULL);
   ASSERT_TRUE(resolve("Lynx")       != NULL);
   ASSERT_TRUE(resolve("PC Engine")  != NULL);
}

/* ============================================================
 * main
 * ============================================================ */

int main(void)
{
   printf("=== downplay metadata disambiguation tests ===\n");

   RUN_TEST(test_canonical_full_names);
   RUN_TEST(test_short_form_abbreviations);
   RUN_TEST(test_regional_aliases_collapse);
   RUN_TEST(test_case_insensitive);
   RUN_TEST(test_no_substring_matches);
   RUN_TEST(test_null_or_empty_input);
   RUN_TEST(test_unknown_display_returns_null);
   RUN_TEST(test_unknown_display_with_unknown_core);
   RUN_TEST(test_table_path_short_circuits_core_info);
   RUN_TEST(test_no_whitespace_normalization);
   RUN_TEST(test_coverage_for_common_cores);

   printf("\n%d passed, %d failed\n", g_pass, g_fail);
   return g_fail > 0 ? 1 : 0;
}
