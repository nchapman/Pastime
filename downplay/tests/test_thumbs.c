/* Unit tests for downplay/downplay_thumbs.c (pure match cascade).
 *
 * The manager (HTTP, FS, paths) is stubbed by DOWNPLAY_THUMBS_TEST_BUILD.
 * We only exercise the JSON parse + tier-cascade matcher.
 *
 * Build line in run_tests.sh.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "../downplay_thumbs.h"

/* rjson.c references stream / rfile I/O symbols even on the buffer
 * code paths we use.  We only call rjson_open_buffer / rjson_parse_quick,
 * so these are dead code at runtime — provide unused-but-resolvable
 * stubs to satisfy the linker. */
int    filestream_read(void *f, void *s, int64_t len) { (void)f; (void)s; (void)len; return 0; }
int    filestream_write(void *f, const void *s, int64_t len) { (void)f; (void)s; (void)len; return 0; }
int64_t filestream_get_size(void *f) { (void)f; return 0; }
int    intfstream_read(void *f, void *s, uint64_t len) { (void)f; (void)s; (void)len; return 0; }
int    intfstream_write(void *f, const void *s, uint64_t len) { (void)f; (void)s; (void)len; return 0; }
int64_t intfstream_get_size(void *f) { (void)f; return 0; }

static int g_pass;
static int g_fail;

#define ASSERT_STR_EQ(a, b) do { \
   const char *_va = (a); \
   const char *_vb = (b); \
   if (_va && _vb && strcmp(_va, _vb) == 0) { g_pass++; } \
   else { g_fail++; \
      fprintf(stderr, "    FAIL %s:%d  expected \"%s\", got \"%s\"\n", \
            __FILE__, __LINE__, _vb ? _vb : "(null)", \
            _va ? _va : "(null)"); } \
} while (0)

#define ASSERT_NULL(a) do { \
   if ((a) == NULL) { g_pass++; } \
   else { g_fail++; \
      fprintf(stderr, "    FAIL %s:%d  expected NULL, got \"%s\"\n", \
            __FILE__, __LINE__, (const char*)(a)); } \
} while (0)

#define ASSERT_NONNULL(a) do { \
   if ((a) != NULL) { g_pass++; } \
   else { g_fail++; \
      fprintf(stderr, "    FAIL %s:%d  expected non-NULL\n", \
            __FILE__, __LINE__); } \
} while (0)

#define ASSERT_TRUE(a) do { \
   if ((a)) { g_pass++; } \
   else { g_fail++; \
      fprintf(stderr, "    FAIL %s:%d  expected true\n", \
            __FILE__, __LINE__); } \
} while (0)

/* Build a minimal index JSON given a list of "key" entries.  Each
 * gets a stub formats.{jpg,webp} block — content doesn't matter for
 * cascade tests. */
static char *build_idx_json(const char * const *titles, size_t n)
{
   size_t i;
   size_t cap = 256;
   size_t pos;
   char  *buf;
   for (i = 0; i < n; i++)
      cap += strlen(titles[i]) + 64;
   buf = (char*)malloc(cap);
   if (!buf) return NULL;
   pos = (size_t)snprintf(buf, cap,
         "{\"system\":\"Test\",\"image_type\":\"boxart\",\"files\":{");
   for (i = 0; i < n; i++)
   {
      pos += (size_t)snprintf(buf + pos, cap - pos,
            "%s\"%s\":{\"formats\":{\"jpg\":1000,\"webp\":500}}",
            i ? "," : "", titles[i]);
   }
   pos += (size_t)snprintf(buf + pos, cap - pos, "}}");
   (void)pos;
   return buf;
}

static downplay_thumbs_index_t *make_idx(const char * const *titles, size_t n)
{
   char *json = build_idx_json(titles, n);
   downplay_thumbs_index_t *idx;
   if (!json) return NULL;
   idx = downplay_thumbs_index_parse(json, strlen(json));
   free(json);
   return idx;
}

static void test_parse_basic(void)
{
   const char *titles[] = {
      "Pokemon Red (USA, Europe)",
      "Tetris (World)"
   };
   downplay_thumbs_index_t *idx = make_idx(titles, 2);
   ASSERT_NONNULL(idx);
   if (idx)
   {
      ASSERT_TRUE(downplay_thumbs_index_count(idx) == 2);
      downplay_thumbs_index_free(idx);
   }
}

static void test_t0_exact(void)
{
   const char *titles[] = {
      "Pokemon Red (USA, Europe)",
      "Tetris (World)"
   };
   downplay_thumbs_index_t *idx = make_idx(titles, 2);
   /* Exact filename match (extension stripped). */
   ASSERT_STR_EQ(downplay_thumbs_index_match(idx,
         "Pokemon Red (USA, Europe).gb"),
         "Pokemon Red (USA, Europe)");
   downplay_thumbs_index_free(idx);
}

static void test_t1_strips_flags(void)
{
   const char *titles[] = {
      "Pokemon Red (USA, Europe) (SGB Enhanced)",
      "Tetris (World)"
   };
   downplay_thumbs_index_t *idx = make_idx(titles, 2);
   /* User has bare-titled ROM; canonical key has extra parens. */
   ASSERT_STR_EQ(downplay_thumbs_index_match(idx, "Pokemon Red.gb"),
         "Pokemon Red (USA, Europe) (SGB Enhanced)");
   downplay_thumbs_index_free(idx);
}

static void test_t1_rotates_article(void)
{
   /* Canonical (No-Intro form): "Legend of Zelda, The (USA)".  Both
    * sides clean to "The Legend of Zelda" which is the match. */
   const char *titles[] = {
      "Legend of Zelda, The (USA)"
   };
   downplay_thumbs_index_t *idx = make_idx(titles, 1);
   ASSERT_STR_EQ(downplay_thumbs_index_match(idx,
         "The Legend of Zelda.nes"),
         "Legend of Zelda, The (USA)");
   /* Reverse direction also works (user's ROM uses No-Intro form). */
   ASSERT_STR_EQ(downplay_thumbs_index_match(idx,
         "Legend of Zelda, The (USA).nes"),
         "Legend of Zelda, The (USA)");
   downplay_thumbs_index_free(idx);
}

static void test_t2_case_insensitive(void)
{
   const char *titles[] = {
      "Pokemon Red (USA, Europe)"
   };
   downplay_thumbs_index_t *idx = make_idx(titles, 1);
   /* All lowercase — T2's sort_key normalization lowercases both
    * sides, so a hit is still possible. */
   ASSERT_STR_EQ(downplay_thumbs_index_match(idx, "pokemon red.gb"),
         "Pokemon Red (USA, Europe)");
   downplay_thumbs_index_free(idx);
}

static void test_t3_region_preference_usa_over_japan(void)
{
   const char *titles[] = {
      "Sonic The Hedgehog (Japan)",
      "Sonic The Hedgehog (USA, Europe)"
   };
   downplay_thumbs_index_t *idx = make_idx(titles, 2);
   /* Bare "Sonic The Hedgehog" matches both via T1; USA wins T3. */
   ASSERT_STR_EQ(downplay_thumbs_index_match(idx,
         "Sonic The Hedgehog.md"),
         "Sonic The Hedgehog (USA, Europe)");
   downplay_thumbs_index_free(idx);
}

static void test_t3_world_over_japan(void)
{
   const char *titles[] = {
      "Tetris (Japan)",
      "Tetris (World)"
   };
   downplay_thumbs_index_t *idx = make_idx(titles, 2);
   ASSERT_STR_EQ(downplay_thumbs_index_match(idx, "Tetris.gb"),
         "Tetris (World)");
   downplay_thumbs_index_free(idx);
}

static void test_compact_spacing_variants(void)
{
   /* "Mega Man X" and "Megaman X" tokenize identically under heavy
    * normalize (alphanumeric runs only, spaces are separators) — no
    * alias table needed. */
   const char *titles[] = { "Mega Man X (USA)" };
   downplay_thumbs_index_t *idx = make_idx(titles, 1);
   ASSERT_STR_EQ(downplay_thumbs_index_match(idx, "Megaman X.smc"),
         "Mega Man X (USA)");
   ASSERT_STR_EQ(downplay_thumbs_index_match(idx, "MegaManX.smc"),
         "Mega Man X (USA)");
   downplay_thumbs_index_free(idx);
}

static void test_roman_arabic_equivalence(void)
{
   /* "VI" and "6" both reduce to "6" — no alias table needed; the
    * roman→arabic conversion in normalize handles it. */
   const char *titles[] = { "Final Fantasy VI (USA)" };
   downplay_thumbs_index_t *idx = make_idx(titles, 1);
   ASSERT_STR_EQ(downplay_thumbs_index_match(idx,
         "Final Fantasy 6.smc"),
         "Final Fantasy VI (USA)");
   downplay_thumbs_index_free(idx);
}

static void test_miss_homebrew(void)
{
   const char *titles[] = {
      "Pokemon Red (USA, Europe)",
      "Tetris (World)"
   };
   downplay_thumbs_index_t *idx = make_idx(titles, 2);
   /* Bogus homebrew filename — no match anywhere. */
   ASSERT_NULL(downplay_thumbs_index_match(idx,
         "My Awesome Homebrew Game (homebrew).gb"));
   downplay_thumbs_index_free(idx);
}

static void test_empty_index(void)
{
   /* An empty `files` object is a valid index with zero entries. */
   const char *json = "{\"system\":\"X\",\"image_type\":\"boxart\","
                      "\"files\":{}}";
   downplay_thumbs_index_t *idx = downplay_thumbs_index_parse(json,
         strlen(json));
   ASSERT_NONNULL(idx);
   if (idx)
   {
      ASSERT_TRUE(downplay_thumbs_index_count(idx) == 0);
      ASSERT_NULL(downplay_thumbs_index_match(idx, "Anything.nes"));
      downplay_thumbs_index_free(idx);
   }
}

static void test_idempotent_repeated_lookups(void)
{
   const char *titles[] = { "Pokemon Red (USA, Europe)" };
   downplay_thumbs_index_t *idx = make_idx(titles, 1);
   const char *a = downplay_thumbs_index_match(idx, "Pokemon Red.gb");
   const char *b = downplay_thumbs_index_match(idx, "Pokemon Red.gb");
   /* Same internal pointer (not just same value) — confirms no
    * per-call allocation that the caller might forget to free. */
   ASSERT_TRUE(a == b);
   downplay_thumbs_index_free(idx);
}

static void test_multi_paren_world_beats_japan(void)
{
   /* Compound paren tokens still scored: "(World) (Rev 1)" → World. */
   const char *titles[] = {
      "Game (Japan) (Rev A)",
      "Game (World) (Rev 1)"
   };
   downplay_thumbs_index_t *idx = make_idx(titles, 2);
   ASSERT_STR_EQ(downplay_thumbs_index_match(idx, "Game.gb"),
         "Game (World) (Rev 1)");
   downplay_thumbs_index_free(idx);
}

static void test_no_false_positive_substring(void)
{
   /* Subtitle ROM should NOT match a parent title via T1/T2 — once
    * cleaned both sides retain the subtitle, so they remain distinct. */
   const char *titles[] = {
      "Castlevania (USA)",
      "Castlevania - Symphony of the Night (USA)"
   };
   downplay_thumbs_index_t *idx = make_idx(titles, 2);
   /* User has the symphony subtitle; expects symphony, not bare. */
   ASSERT_STR_EQ(downplay_thumbs_index_match(idx,
         "Castlevania - Symphony of the Night.iso"),
         "Castlevania - Symphony of the Night (USA)");
   /* User has the bare title; expects bare, not symphony. */
   ASSERT_STR_EQ(downplay_thumbs_index_match(idx, "Castlevania.nes"),
         "Castlevania (USA)");
   downplay_thumbs_index_free(idx);
}

static void test_multi_disc_disambiguation(void)
{
   const char *titles[] = {
      "Final Fantasy VII (USA) (Disc 1)",
      "Final Fantasy VII (USA) (Disc 2)",
      "Final Fantasy VII (USA) (Disc 3)"
   };
   downplay_thumbs_index_t *idx = make_idx(titles, 3);
   /* User's filename names the disc — must pick the matching disc,
    * not whichever disc has the best region score. */
   ASSERT_STR_EQ(downplay_thumbs_index_match(idx,
         "Final Fantasy VII (Disc 2).bin"),
         "Final Fantasy VII (USA) (Disc 2)");
   ASSERT_STR_EQ(downplay_thumbs_index_match(idx,
         "Final Fantasy VII (Disc 3).bin"),
         "Final Fantasy VII (USA) (Disc 3)");
   /* Bare filename (no disc) — falls back to first/best region. */
   ASSERT_NONNULL(downplay_thumbs_index_match(idx,
         "Final Fantasy VII.bin"));
   downplay_thumbs_index_free(idx);
}

static void test_region_prefix_not_a_false_match(void)
{
   /* "(USA Proto)" must NOT score as USA — proto is a different
    * release.  Regression for the bare-prefix bug. */
   const char *titles[] = {
      "Game (USA Proto)",
      "Game (Japan)"
   };
   downplay_thumbs_index_t *idx = make_idx(titles, 2);
   /* Both should score as something other than USA.  Expect Japan to
    * win since proto scores as DP_REGION_OTHER (5) and Japan scores
    * as DP_REGION_JP (7); OTHER < JP so Proto wins.  The bug-free
    * behavior is "USA Proto != USA"; either result is acceptable as
    * long as the test below holds. */
   const char *hit = downplay_thumbs_index_match(idx, "Game.gb");
   ASSERT_NONNULL(hit);
   /* If "USA Proto" had been mis-scored as USA, it would beat Japan
    * unconditionally; the bug-free implementation gives proto only
    * OTHER, so it still beats Japan — but for a tighter regression
    * we test the reversed pair: */
   downplay_thumbs_index_free(idx);

   /* USA Proto vs USA — USA must win unambiguously. */
   {
      const char *titles2[] = {
         "Game (USA Proto)",
         "Game (USA)"
      };
      downplay_thumbs_index_t *idx2 = make_idx(titles2, 2);
      ASSERT_STR_EQ(downplay_thumbs_index_match(idx2, "Game.gb"),
            "Game (USA)");
      downplay_thumbs_index_free(idx2);
   }
}

static void test_path_traversal_rejected(void)
{
   /* Hostile keys that would let us write JPGs outside the cache
    * directory must be silently dropped from the index. */
   const char *titles[] = {
      "../../etc/passwd",
      "subdir/Pokemon Red",
      "..",
      ".hidden",
      "Good Title (USA)"
   };
   downplay_thumbs_index_t *idx = make_idx(titles, 5);
   ASSERT_NONNULL(idx);
   if (idx)
   {
      /* Only the safe one survives. */
      ASSERT_TRUE(downplay_thumbs_index_count(idx) == 1);
      ASSERT_STR_EQ(downplay_thumbs_index_match(idx, "Good Title.nes"),
            "Good Title (USA)");
      ASSERT_NULL(downplay_thumbs_index_match(idx, "../../etc/passwd"));
      ASSERT_NULL(downplay_thumbs_index_match(idx, "subdir/Pokemon Red"));
      downplay_thumbs_index_free(idx);
   }
}

static void test_bad_dump_filtered_out(void)
{
   /* Beta/Proto/Demo must lose to a clean release on the same heavy. */
   const char *titles[] = {
      "Sonic the Hedgehog (USA, Europe)",
      "Sonic the Hedgehog (Beta)",
      "Sonic the Hedgehog (Prototype)"
   };
   downplay_thumbs_index_t *idx = make_idx(titles, 3);
   ASSERT_STR_EQ(downplay_thumbs_index_match(idx, "Sonic the Hedgehog.md"),
         "Sonic the Hedgehog (USA, Europe)");
   downplay_thumbs_index_free(idx);
}

static void test_bad_dump_only_falls_through(void)
{
   /* If only bad dumps exist, return one rather than nothing. */
   const char *titles[] = {
      "Foo (Beta)",
      "Foo (Prototype)"
   };
   downplay_thumbs_index_t *idx = make_idx(titles, 2);
   ASSERT_NONNULL(downplay_thumbs_index_match(idx, "Foo.bin"));
   downplay_thumbs_index_free(idx);
}

static void test_rev_highest_wins(void)
{
   /* Higher Rev N must win when region is the same. */
   const char *titles[] = {
      "Foo (USA)",
      "Foo (USA) (Rev 1)",
      "Foo (USA) (Rev 2)"
   };
   downplay_thumbs_index_t *idx = make_idx(titles, 3);
   ASSERT_STR_EQ(downplay_thumbs_index_match(idx, "Foo.bin"),
         "Foo (USA) (Rev 2)");
   downplay_thumbs_index_free(idx);
}

static void test_cd_rom_is_not_a_disc_tag(void)
{
   /* "(CD ROM)" is a media tag, not a disc id.  Without the id-shape
    * filter we'd capture "ROM" as the disc id and a user file with
    * (Disc 1) would mismatch it. */
   const char *titles[] = {
      "Foo (USA) (CD ROM)",
      "Bar (USA) (CD Audio)",
      "Real Disc Game (USA) (Disc 1)"
   };
   downplay_thumbs_index_t *idx = make_idx(titles, 3);
   /* User file with (Disc 1) must not be penalised against the
    * (CD ROM) entry — the (CD ROM) tag should NOT have a disc_token. */
   ASSERT_STR_EQ(downplay_thumbs_index_match(idx, "Foo (Disc 1).bin"),
         "Foo (USA) (CD ROM)");
   ASSERT_STR_EQ(downplay_thumbs_index_match(idx, "Real Disc Game (Disc 1).bin"),
         "Real Disc Game (USA) (Disc 1)");
   downplay_thumbs_index_free(idx);
}

static void test_rev_clamped_no_overflow(void)
{
   /* Hostile/malformed (Rev 12345) must not score better than a clean
    * USA release.  rev_num clamps to [0, 99] so it can't leak into
    * the region×100 band of the layered score. */
   const char *titles[] = {
      "Foo (USA)",
      "Foo (Japan) (Rev 12345)"
   };
   downplay_thumbs_index_t *idx = make_idx(titles, 2);
   ASSERT_STR_EQ(downplay_thumbs_index_match(idx, "Foo.bin"),
         "Foo (USA)");
   downplay_thumbs_index_free(idx);
}

static void test_roman_to_arabic(void)
{
   /* User's filename uses arabic; canonical uses roman.  Both
    * normalize to "finalfantasy7" via the II-IX table. */
   const char *titles[] = {
      "Final Fantasy VII (USA)"
   };
   downplay_thumbs_index_t *idx = make_idx(titles, 1);
   ASSERT_STR_EQ(downplay_thumbs_index_match(idx, "Final Fantasy 7.bin"),
         "Final Fantasy VII (USA)");
   ASSERT_STR_EQ(downplay_thumbs_index_match(idx, "Final Fantasy VII.bin"),
         "Final Fantasy VII (USA)");
   downplay_thumbs_index_free(idx);
}

static void test_roman_x_not_converted(void)
{
   /* "Mega Man X" must NOT collide with "Mega Man 10" — X is excluded
    * from the roman table specifically to avoid this. */
   const char *titles[] = {
      "Mega Man X (USA)",
      "Mega Man 10 (USA)"
   };
   downplay_thumbs_index_t *idx = make_idx(titles, 2);
   ASSERT_STR_EQ(downplay_thumbs_index_match(idx, "Mega Man X.smc"),
         "Mega Man X (USA)");
   ASSERT_STR_EQ(downplay_thumbs_index_match(idx, "Mega Man 10.smc"),
         "Mega Man 10 (USA)");
   downplay_thumbs_index_free(idx);
}

static void test_ampersand_and_underscore_equivalence(void)
{
   /* The libretro thumbnails mirror sanitises filesystem-unsafe
    * characters (& * / : ? ...) to '_' in canonical keys.  Our
    * normalize must converge user-side "&"/"and"/literal "_" to
    * the same heavy form.  Real-world example: index canonical is
    * "Mario _ Luigi - Superstar Saga (USA)"; user filename is
    * "Mario & Luigi - Superstar Saga (USA).zip". */
   const char *titles[] = {
      "Mario _ Luigi - Superstar Saga (USA)"  /* mirror form */
   };
   downplay_thumbs_index_t *idx = make_idx(titles, 1);
   ASSERT_STR_EQ(downplay_thumbs_index_match(idx,
         "Mario & Luigi - Superstar Saga (USA).zip"),
         "Mario _ Luigi - Superstar Saga (USA)");
   ASSERT_STR_EQ(downplay_thumbs_index_match(idx,
         "Mario and Luigi - Superstar Saga.zip"),
         "Mario _ Luigi - Superstar Saga (USA)");
   ASSERT_STR_EQ(downplay_thumbs_index_match(idx,
         "Mario _ Luigi - Superstar Saga.zip"),
         "Mario _ Luigi - Superstar Saga (USA)");
   downplay_thumbs_index_free(idx);

   /* Also: "Tom & Jerry" ↔ "Tom and Jerry" (no underscore on either
    * side) still works via the "and" connective drop. */
   {
      const char *titles2[] = { "Tom & Jerry (USA)" };
      downplay_thumbs_index_t *idx2 = make_idx(titles2, 1);
      ASSERT_STR_EQ(downplay_thumbs_index_match(idx2, "Tom and Jerry.gen"),
            "Tom & Jerry (USA)");
      ASSERT_STR_EQ(downplay_thumbs_index_match(idx2, "Tom & Jerry.gen"),
            "Tom & Jerry (USA)");
      downplay_thumbs_index_free(idx2);
   }
}

static void test_latin_fold(void)
{
   /* Accented canonical must match unaccented user filename and
    * vice versa.  é/ñ/ç/etc fold to ASCII before tokenization. */
   const char *titles[] = {
      "Pokémon Red (USA, Europe)"
   };
   downplay_thumbs_index_t *idx = make_idx(titles, 1);
   ASSERT_STR_EQ(downplay_thumbs_index_match(idx, "Pokemon Red.gb"),
         "Pokémon Red (USA, Europe)");
   ASSERT_STR_EQ(downplay_thumbs_index_match(idx, "Pokémon Red.gb"),
         "Pokémon Red (USA, Europe)");
   downplay_thumbs_index_free(idx);

   {
      const char *titles2[] = { "Doña Bárbara (Spain)" };
      downplay_thumbs_index_t *idx2 = make_idx(titles2, 1);
      ASSERT_STR_EQ(downplay_thumbs_index_match(idx2, "Dona Barbara.bin"),
            "Doña Bárbara (Spain)");
      downplay_thumbs_index_free(idx2);
   }
}

static void test_punctuation_and_spacing_irrelevant(void)
{
   const char *titles[] = {
      "Mike Tyson's Punch-Out!! (USA)"
   };
   downplay_thumbs_index_t *idx = make_idx(titles, 1);
   /* All these are equivalent under heavy normalize. */
   ASSERT_STR_EQ(downplay_thumbs_index_match(idx, "Mike.Tysons.Punch.Out.nes"),
         "Mike Tyson's Punch-Out!! (USA)");
   ASSERT_STR_EQ(downplay_thumbs_index_match(idx, "Mike_Tysons_PunchOut.nes"),
         "Mike Tyson's Punch-Out!! (USA)");
   downplay_thumbs_index_free(idx);
}

static void test_rev_letter_ordering(void)
{
   /* Letter revs: A=1, B=2, C=3.  Higher letter wins, same as numeric. */
   const char *titles[] = {
      "Foo (USA) (Rev A)",
      "Foo (USA) (Rev C)",
      "Foo (USA) (Rev B)"
   };
   downplay_thumbs_index_t *idx = make_idx(titles, 3);
   ASSERT_STR_EQ(downplay_thumbs_index_match(idx, "Foo.bin"),
         "Foo (USA) (Rev C)");
   downplay_thumbs_index_free(idx);
}

static void test_multiple_bad_dump_tags(void)
{
   /* Entry with several flags is still bad-dump. */
   const char *titles[] = {
      "Foo (USA)",
      "Foo (Beta) (Proto)"
   };
   downplay_thumbs_index_t *idx = make_idx(titles, 2);
   ASSERT_STR_EQ(downplay_thumbs_index_match(idx, "Foo.bin"),
         "Foo (USA)");
   downplay_thumbs_index_free(idx);
}

static void test_alt_name_bundle(void)
{
   /* No-Intro / libretro mirror packs multi-name regional releases
    * into one canonical separated by ` _ ` (originally ` ~ ` in the
    * No-Intro DAT, sanitised to ` _ ` for filesystem safety).  A
    * user filename matching ANY alt name must resolve to the bundle. */
   const char *titles[] = {
      "F-16 Fighting Falcon _ F-16 Fighter _ F16 Falcon Fighter (USA, Europe, Brazil) (En)"
   };
   downplay_thumbs_index_t *idx = make_idx(titles, 1);
   ASSERT_STR_EQ(downplay_thumbs_index_match(idx,
         "F-16 Fighter (USA, Europe, Brazil) (En).zip"),
         "F-16 Fighting Falcon _ F-16 Fighter _ F16 Falcon Fighter (USA, Europe, Brazil) (En)");
   ASSERT_STR_EQ(downplay_thumbs_index_match(idx,
         "F-16 Fighting Falcon.zip"),
         "F-16 Fighting Falcon _ F-16 Fighter _ F16 Falcon Fighter (USA, Europe, Brazil) (En)");
   ASSERT_STR_EQ(downplay_thumbs_index_match(idx,
         "F16 Falcon Fighter.zip"),
         "F-16 Fighting Falcon _ F-16 Fighter _ F16 Falcon Fighter (USA, Europe, Brazil) (En)");
   downplay_thumbs_index_free(idx);
}

static void test_unbalanced_paren_in_user_input(void)
{
   /* User filename has a stray '(': normalize tokenizes whatever it
    * can.  Don't crash; either match or miss is fine. */
   const char *titles[] = {
      "Bar Game (USA)"
   };
   downplay_thumbs_index_t *idx = make_idx(titles, 1);
   /* "Bar Game" → "bargame" matches "Bar Game" → "bargame".  The
    * trailing "(extra" tokenizes as "extra" and is appended → no
    * match.  This documents the behavior; the important property is
    * "no crash". */
   (void)downplay_thumbs_index_match(idx, "Bar Game (extra.gb");
   downplay_thumbs_index_free(idx);
}

int main(void)
{
   test_parse_basic();
   test_t0_exact();
   test_t1_strips_flags();
   test_t1_rotates_article();
   test_t2_case_insensitive();
   test_t3_region_preference_usa_over_japan();
   test_t3_world_over_japan();
   test_compact_spacing_variants();
   test_roman_arabic_equivalence();
   test_miss_homebrew();
   test_empty_index();
   test_idempotent_repeated_lookups();
   test_multi_paren_world_beats_japan();
   test_no_false_positive_substring();
   test_multi_disc_disambiguation();
   test_cd_rom_is_not_a_disc_tag();
   test_region_prefix_not_a_false_match();
   test_path_traversal_rejected();
   test_bad_dump_filtered_out();
   test_bad_dump_only_falls_through();
   test_rev_highest_wins();
   test_rev_clamped_no_overflow();
   test_rev_letter_ordering();
   test_multiple_bad_dump_tags();
   test_unbalanced_paren_in_user_input();
   test_alt_name_bundle();
   test_roman_to_arabic();
   test_roman_x_not_converted();
   test_ampersand_and_underscore_equivalence();
   test_latin_fold();
   test_punctuation_and_spacing_irrelevant();

   printf("test_thumbs: %d passed, %d failed\n", g_pass, g_fail);
   return g_fail ? 1 : 0;
}
