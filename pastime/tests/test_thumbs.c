/* Unit tests for pastime/pastime_thumbs_index.c — the pure parse +
 * tier-cascade matcher.  The HTTP/IO manager (pastime_thumbs.c) is
 * a separate translation unit and is intentionally NOT linked into
 * this test binary; see run_tests.sh. */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "../pastime_thumbs.h"
#include "../pastime_thumbs_internal.h"

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
 * cascade tests.  The legacy form (no per-entry width/height/
 * thumbhash) — covers the forward-compat path where the parser
 * accepts entries without the v2 dim fields and yields 0/NULL. */
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

/* v2 builder: every entry carries width/height/thumbhash sourced
 * from a parallel array.  Used to exercise the dim + thumbhash
 * round-trip through the binary format.  Pass NULL for a per-entry
 * thumbhash slot to omit just that field. */
typedef struct
{
   const char *title;
   uint16_t    width;
   uint16_t    height;
   const char *thumbhash_b64;   /* NULL → field omitted entirely */
} v2_entry_t;

static char *build_idx_json_v2(const v2_entry_t *entries, size_t n)
{
   size_t i;
   size_t cap = 256;
   size_t pos;
   char  *buf;
   for (i = 0; i < n; i++)
      cap += strlen(entries[i].title) + 128
           + (entries[i].thumbhash_b64
                ? strlen(entries[i].thumbhash_b64) : 0);
   buf = (char*)malloc(cap);
   if (!buf) return NULL;
   pos = (size_t)snprintf(buf, cap,
         "{\"system\":\"Test\",\"image_type\":\"boxart\",\"files\":{");
   for (i = 0; i < n; i++)
   {
      pos += (size_t)snprintf(buf + pos, cap - pos,
            "%s\"%s\":{\"formats\":{\"webp\":500}"
            ",\"width\":%u,\"height\":%u",
            i ? "," : "", entries[i].title,
            (unsigned)entries[i].width,
            (unsigned)entries[i].height);
      if (entries[i].thumbhash_b64)
         pos += (size_t)snprintf(buf + pos, cap - pos,
               ",\"thumbhash\":\"%s\"", entries[i].thumbhash_b64);
      pos += (size_t)snprintf(buf + pos, cap - pos, "}");
   }
   pos += (size_t)snprintf(buf + pos, cap - pos, "}}");
   (void)pos;
   return buf;
}

static pastime_thumbs_index_t *make_idx(const char * const *titles, size_t n)
{
   char *json = build_idx_json(titles, n);
   pastime_thumbs_index_t *idx;
   if (!json) return NULL;
   idx = pastime_thumbs_index_parse(json, strlen(json));
   free(json);
   return idx;
}

static void test_parse_basic(void)
{
   const char *titles[] = {
      "Pokemon Red (USA, Europe)",
      "Tetris (World)"
   };
   pastime_thumbs_index_t *idx = make_idx(titles, 2);
   ASSERT_NONNULL(idx);
   if (idx)
   {
      ASSERT_TRUE(pastime_thumbs_index_count(idx) == 2);
      pastime_thumbs_index_free(idx);
   }
}

static void test_t0_exact(void)
{
   const char *titles[] = {
      "Pokemon Red (USA, Europe)",
      "Tetris (World)"
   };
   pastime_thumbs_index_t *idx = make_idx(titles, 2);
   /* Exact filename match (extension stripped). */
   ASSERT_STR_EQ(pastime_thumbs_index_match(idx,
         "Pokemon Red (USA, Europe).gb"),
         "Pokemon Red (USA, Europe)");
   pastime_thumbs_index_free(idx);
}

static void test_t1_strips_flags(void)
{
   const char *titles[] = {
      "Pokemon Red (USA, Europe) (SGB Enhanced)",
      "Tetris (World)"
   };
   pastime_thumbs_index_t *idx = make_idx(titles, 2);
   /* User has bare-titled ROM; canonical key has extra parens. */
   ASSERT_STR_EQ(pastime_thumbs_index_match(idx, "Pokemon Red.gb"),
         "Pokemon Red (USA, Europe) (SGB Enhanced)");
   pastime_thumbs_index_free(idx);
}

static void test_t1_rotates_article(void)
{
   /* Canonical (No-Intro form): "Legend of Zelda, The (USA)".  Both
    * sides clean to "The Legend of Zelda" which is the match. */
   const char *titles[] = {
      "Legend of Zelda, The (USA)"
   };
   pastime_thumbs_index_t *idx = make_idx(titles, 1);
   ASSERT_STR_EQ(pastime_thumbs_index_match(idx,
         "The Legend of Zelda.nes"),
         "Legend of Zelda, The (USA)");
   /* Reverse direction also works (user's ROM uses No-Intro form). */
   ASSERT_STR_EQ(pastime_thumbs_index_match(idx,
         "Legend of Zelda, The (USA).nes"),
         "Legend of Zelda, The (USA)");
   pastime_thumbs_index_free(idx);
}

static void test_t2_case_insensitive(void)
{
   const char *titles[] = {
      "Pokemon Red (USA, Europe)"
   };
   pastime_thumbs_index_t *idx = make_idx(titles, 1);
   /* All lowercase — T2's sort_key normalization lowercases both
    * sides, so a hit is still possible. */
   ASSERT_STR_EQ(pastime_thumbs_index_match(idx, "pokemon red.gb"),
         "Pokemon Red (USA, Europe)");
   pastime_thumbs_index_free(idx);
}

static void test_t3_region_preference_usa_over_japan(void)
{
   const char *titles[] = {
      "Sonic The Hedgehog (Japan)",
      "Sonic The Hedgehog (USA, Europe)"
   };
   pastime_thumbs_index_t *idx = make_idx(titles, 2);
   /* Bare "Sonic The Hedgehog" matches both via T1; USA wins T3. */
   ASSERT_STR_EQ(pastime_thumbs_index_match(idx,
         "Sonic The Hedgehog.md"),
         "Sonic The Hedgehog (USA, Europe)");
   pastime_thumbs_index_free(idx);
}

static void test_t3_world_over_japan(void)
{
   const char *titles[] = {
      "Tetris (Japan)",
      "Tetris (World)"
   };
   pastime_thumbs_index_t *idx = make_idx(titles, 2);
   ASSERT_STR_EQ(pastime_thumbs_index_match(idx, "Tetris.gb"),
         "Tetris (World)");
   pastime_thumbs_index_free(idx);
}

static void test_compact_spacing_variants(void)
{
   /* "Mega Man X" and "Megaman X" tokenize identically under heavy
    * normalize (alphanumeric runs only, spaces are separators) — no
    * alias table needed. */
   const char *titles[] = { "Mega Man X (USA)" };
   pastime_thumbs_index_t *idx = make_idx(titles, 1);
   ASSERT_STR_EQ(pastime_thumbs_index_match(idx, "Megaman X.smc"),
         "Mega Man X (USA)");
   ASSERT_STR_EQ(pastime_thumbs_index_match(idx, "MegaManX.smc"),
         "Mega Man X (USA)");
   pastime_thumbs_index_free(idx);
}

static void test_roman_arabic_equivalence(void)
{
   /* "VI" and "6" both reduce to "6" — no alias table needed; the
    * roman→arabic conversion in normalize handles it. */
   const char *titles[] = { "Final Fantasy VI (USA)" };
   pastime_thumbs_index_t *idx = make_idx(titles, 1);
   ASSERT_STR_EQ(pastime_thumbs_index_match(idx,
         "Final Fantasy 6.smc"),
         "Final Fantasy VI (USA)");
   pastime_thumbs_index_free(idx);
}

static void test_miss_homebrew(void)
{
   const char *titles[] = {
      "Pokemon Red (USA, Europe)",
      "Tetris (World)"
   };
   pastime_thumbs_index_t *idx = make_idx(titles, 2);
   /* Bogus homebrew filename — no match anywhere. */
   ASSERT_NULL(pastime_thumbs_index_match(idx,
         "My Awesome Homebrew Game (homebrew).gb"));
   pastime_thumbs_index_free(idx);
}

static void test_empty_index(void)
{
   /* An empty `files` object is a valid index with zero entries. */
   const char *json = "{\"system\":\"X\",\"image_type\":\"boxart\","
                      "\"files\":{}}";
   pastime_thumbs_index_t *idx = pastime_thumbs_index_parse(json,
         strlen(json));
   ASSERT_NONNULL(idx);
   if (idx)
   {
      ASSERT_TRUE(pastime_thumbs_index_count(idx) == 0);
      ASSERT_NULL(pastime_thumbs_index_match(idx, "Anything.nes"));
      pastime_thumbs_index_free(idx);
   }
}

static void test_idempotent_repeated_lookups(void)
{
   const char *titles[] = { "Pokemon Red (USA, Europe)" };
   pastime_thumbs_index_t *idx = make_idx(titles, 1);
   const char *a = pastime_thumbs_index_match(idx, "Pokemon Red.gb");
   const char *b = pastime_thumbs_index_match(idx, "Pokemon Red.gb");
   /* Same internal pointer (not just same value) — confirms no
    * per-call allocation that the caller might forget to free. */
   ASSERT_TRUE(a == b);
   pastime_thumbs_index_free(idx);
}

static void test_multi_paren_world_beats_japan(void)
{
   /* Compound paren tokens still scored: "(World) (Rev 1)" → World. */
   const char *titles[] = {
      "Game (Japan) (Rev A)",
      "Game (World) (Rev 1)"
   };
   pastime_thumbs_index_t *idx = make_idx(titles, 2);
   ASSERT_STR_EQ(pastime_thumbs_index_match(idx, "Game.gb"),
         "Game (World) (Rev 1)");
   pastime_thumbs_index_free(idx);
}

static void test_no_false_positive_substring(void)
{
   /* Subtitle ROM should NOT match a parent title via T1/T2 — once
    * cleaned both sides retain the subtitle, so they remain distinct. */
   const char *titles[] = {
      "Castlevania (USA)",
      "Castlevania - Symphony of the Night (USA)"
   };
   pastime_thumbs_index_t *idx = make_idx(titles, 2);
   /* User has the symphony subtitle; expects symphony, not bare. */
   ASSERT_STR_EQ(pastime_thumbs_index_match(idx,
         "Castlevania - Symphony of the Night.iso"),
         "Castlevania - Symphony of the Night (USA)");
   /* User has the bare title; expects bare, not symphony. */
   ASSERT_STR_EQ(pastime_thumbs_index_match(idx, "Castlevania.nes"),
         "Castlevania (USA)");
   pastime_thumbs_index_free(idx);
}

static void test_multi_disc_disambiguation(void)
{
   const char *titles[] = {
      "Final Fantasy VII (USA) (Disc 1)",
      "Final Fantasy VII (USA) (Disc 2)",
      "Final Fantasy VII (USA) (Disc 3)"
   };
   pastime_thumbs_index_t *idx = make_idx(titles, 3);
   /* User's filename names the disc — must pick the matching disc,
    * not whichever disc has the best region score. */
   ASSERT_STR_EQ(pastime_thumbs_index_match(idx,
         "Final Fantasy VII (Disc 2).bin"),
         "Final Fantasy VII (USA) (Disc 2)");
   ASSERT_STR_EQ(pastime_thumbs_index_match(idx,
         "Final Fantasy VII (Disc 3).bin"),
         "Final Fantasy VII (USA) (Disc 3)");
   /* Bare filename (no disc) — falls back to first/best region. */
   ASSERT_NONNULL(pastime_thumbs_index_match(idx,
         "Final Fantasy VII.bin"));
   pastime_thumbs_index_free(idx);
}

static void test_region_prefix_not_a_false_match(void)
{
   /* "(USA Proto)" must NOT score as USA — proto is a different
    * release.  Regression for the bare-prefix bug: if the region
    * scorer matched on "USA" prefix without checking what follows,
    * USA Proto would beat clean USA.  The dp_is_region_terminator
    * check guards this. */
   const char *titles[] = {
      "Game (USA Proto)",
      "Game (USA)"
   };
   pastime_thumbs_index_t *idx = make_idx(titles, 2);
   ASSERT_STR_EQ(pastime_thumbs_index_match(idx, "Game.gb"),
         "Game (USA)");
   pastime_thumbs_index_free(idx);
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
   pastime_thumbs_index_t *idx = make_idx(titles, 5);
   ASSERT_NONNULL(idx);
   if (idx)
   {
      /* Only the safe one survives. */
      ASSERT_TRUE(pastime_thumbs_index_count(idx) == 1);
      ASSERT_STR_EQ(pastime_thumbs_index_match(idx, "Good Title.nes"),
            "Good Title (USA)");
      ASSERT_NULL(pastime_thumbs_index_match(idx, "../../etc/passwd"));
      ASSERT_NULL(pastime_thumbs_index_match(idx, "subdir/Pokemon Red"));
      pastime_thumbs_index_free(idx);
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
   pastime_thumbs_index_t *idx = make_idx(titles, 3);
   ASSERT_STR_EQ(pastime_thumbs_index_match(idx, "Sonic the Hedgehog.md"),
         "Sonic the Hedgehog (USA, Europe)");
   pastime_thumbs_index_free(idx);
}

static void test_bad_dump_only_falls_through(void)
{
   /* If only bad dumps exist, return one rather than nothing. */
   const char *titles[] = {
      "Foo (Beta)",
      "Foo (Prototype)"
   };
   pastime_thumbs_index_t *idx = make_idx(titles, 2);
   ASSERT_NONNULL(pastime_thumbs_index_match(idx, "Foo.bin"));
   pastime_thumbs_index_free(idx);
}

static void test_rev_highest_wins(void)
{
   /* Higher Rev N must win when region is the same. */
   const char *titles[] = {
      "Foo (USA)",
      "Foo (USA) (Rev 1)",
      "Foo (USA) (Rev 2)"
   };
   pastime_thumbs_index_t *idx = make_idx(titles, 3);
   ASSERT_STR_EQ(pastime_thumbs_index_match(idx, "Foo.bin"),
         "Foo (USA) (Rev 2)");
   pastime_thumbs_index_free(idx);
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
   pastime_thumbs_index_t *idx = make_idx(titles, 3);
   /* User file with (Disc 1) must not be penalised against the
    * (CD ROM) entry — the (CD ROM) tag should NOT have a disc_token. */
   ASSERT_STR_EQ(pastime_thumbs_index_match(idx, "Foo (Disc 1).bin"),
         "Foo (USA) (CD ROM)");
   ASSERT_STR_EQ(pastime_thumbs_index_match(idx, "Real Disc Game (Disc 1).bin"),
         "Real Disc Game (USA) (Disc 1)");
   pastime_thumbs_index_free(idx);
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
   pastime_thumbs_index_t *idx = make_idx(titles, 2);
   ASSERT_STR_EQ(pastime_thumbs_index_match(idx, "Foo.bin"),
         "Foo (USA)");
   pastime_thumbs_index_free(idx);
}

static void test_roman_to_arabic(void)
{
   /* User's filename uses arabic; canonical uses roman.  Both
    * normalize to "finalfantasy7" via the II-IX table. */
   const char *titles[] = {
      "Final Fantasy VII (USA)"
   };
   pastime_thumbs_index_t *idx = make_idx(titles, 1);
   ASSERT_STR_EQ(pastime_thumbs_index_match(idx, "Final Fantasy 7.bin"),
         "Final Fantasy VII (USA)");
   ASSERT_STR_EQ(pastime_thumbs_index_match(idx, "Final Fantasy VII.bin"),
         "Final Fantasy VII (USA)");
   pastime_thumbs_index_free(idx);
}

static void test_roman_x_not_converted(void)
{
   /* "Mega Man X" must NOT collide with "Mega Man 10" — X is excluded
    * from the roman table specifically to avoid this. */
   const char *titles[] = {
      "Mega Man X (USA)",
      "Mega Man 10 (USA)"
   };
   pastime_thumbs_index_t *idx = make_idx(titles, 2);
   ASSERT_STR_EQ(pastime_thumbs_index_match(idx, "Mega Man X.smc"),
         "Mega Man X (USA)");
   ASSERT_STR_EQ(pastime_thumbs_index_match(idx, "Mega Man 10.smc"),
         "Mega Man 10 (USA)");
   pastime_thumbs_index_free(idx);
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
   pastime_thumbs_index_t *idx = make_idx(titles, 1);
   ASSERT_STR_EQ(pastime_thumbs_index_match(idx,
         "Mario & Luigi - Superstar Saga (USA).zip"),
         "Mario _ Luigi - Superstar Saga (USA)");
   ASSERT_STR_EQ(pastime_thumbs_index_match(idx,
         "Mario and Luigi - Superstar Saga.zip"),
         "Mario _ Luigi - Superstar Saga (USA)");
   ASSERT_STR_EQ(pastime_thumbs_index_match(idx,
         "Mario _ Luigi - Superstar Saga.zip"),
         "Mario _ Luigi - Superstar Saga (USA)");
   pastime_thumbs_index_free(idx);

   /* Also: "Tom & Jerry" ↔ "Tom and Jerry" (no underscore on either
    * side) still works via the "and" connective drop. */
   {
      const char *titles2[] = { "Tom & Jerry (USA)" };
      pastime_thumbs_index_t *idx2 = make_idx(titles2, 1);
      ASSERT_STR_EQ(pastime_thumbs_index_match(idx2, "Tom and Jerry.gen"),
            "Tom & Jerry (USA)");
      ASSERT_STR_EQ(pastime_thumbs_index_match(idx2, "Tom & Jerry.gen"),
            "Tom & Jerry (USA)");
      pastime_thumbs_index_free(idx2);
   }
}

static void test_latin_fold(void)
{
   /* Accented canonical must match unaccented user filename and
    * vice versa.  é/ñ/ç/etc fold to ASCII before tokenization. */
   const char *titles[] = {
      "Pokémon Red (USA, Europe)"
   };
   pastime_thumbs_index_t *idx = make_idx(titles, 1);
   ASSERT_STR_EQ(pastime_thumbs_index_match(idx, "Pokemon Red.gb"),
         "Pokémon Red (USA, Europe)");
   ASSERT_STR_EQ(pastime_thumbs_index_match(idx, "Pokémon Red.gb"),
         "Pokémon Red (USA, Europe)");
   pastime_thumbs_index_free(idx);

   {
      const char *titles2[] = { "Doña Bárbara (Spain)" };
      pastime_thumbs_index_t *idx2 = make_idx(titles2, 1);
      ASSERT_STR_EQ(pastime_thumbs_index_match(idx2, "Dona Barbara.bin"),
            "Doña Bárbara (Spain)");
      pastime_thumbs_index_free(idx2);
   }
}

static void test_punctuation_and_spacing_irrelevant(void)
{
   const char *titles[] = {
      "Mike Tyson's Punch-Out!! (USA)"
   };
   pastime_thumbs_index_t *idx = make_idx(titles, 1);
   /* All these are equivalent under heavy normalize. */
   ASSERT_STR_EQ(pastime_thumbs_index_match(idx, "Mike.Tysons.Punch.Out.nes"),
         "Mike Tyson's Punch-Out!! (USA)");
   ASSERT_STR_EQ(pastime_thumbs_index_match(idx, "Mike_Tysons_PunchOut.nes"),
         "Mike Tyson's Punch-Out!! (USA)");
   pastime_thumbs_index_free(idx);
}

static void test_rev_letter_ordering(void)
{
   /* Letter revs: A=1, B=2, C=3.  Higher letter wins, same as numeric. */
   const char *titles[] = {
      "Foo (USA) (Rev A)",
      "Foo (USA) (Rev C)",
      "Foo (USA) (Rev B)"
   };
   pastime_thumbs_index_t *idx = make_idx(titles, 3);
   ASSERT_STR_EQ(pastime_thumbs_index_match(idx, "Foo.bin"),
         "Foo (USA) (Rev C)");
   pastime_thumbs_index_free(idx);
}

static void test_multiple_bad_dump_tags(void)
{
   /* Entry with several flags is still bad-dump. */
   const char *titles[] = {
      "Foo (USA)",
      "Foo (Beta) (Proto)"
   };
   pastime_thumbs_index_t *idx = make_idx(titles, 2);
   ASSERT_STR_EQ(pastime_thumbs_index_match(idx, "Foo.bin"),
         "Foo (USA)");
   pastime_thumbs_index_free(idx);
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
   pastime_thumbs_index_t *idx = make_idx(titles, 1);
   ASSERT_STR_EQ(pastime_thumbs_index_match(idx,
         "F-16 Fighter (USA, Europe, Brazil) (En).zip"),
         "F-16 Fighting Falcon _ F-16 Fighter _ F16 Falcon Fighter (USA, Europe, Brazil) (En)");
   ASSERT_STR_EQ(pastime_thumbs_index_match(idx,
         "F-16 Fighting Falcon.zip"),
         "F-16 Fighting Falcon _ F-16 Fighter _ F16 Falcon Fighter (USA, Europe, Brazil) (En)");
   ASSERT_STR_EQ(pastime_thumbs_index_match(idx,
         "F16 Falcon Fighter.zip"),
         "F-16 Fighting Falcon _ F-16 Fighter _ F16 Falcon Fighter (USA, Europe, Brazil) (En)");
   pastime_thumbs_index_free(idx);
}

static void test_unbalanced_paren_in_user_input(void)
{
   /* User filename has a stray '(': normalize tokenizes whatever it
    * can.  Don't crash; either match or miss is fine. */
   const char *titles[] = {
      "Bar Game (USA)"
   };
   pastime_thumbs_index_t *idx = make_idx(titles, 1);
   /* "Bar Game" → "bargame" matches "Bar Game" → "bargame".  The
    * trailing "(extra" tokenizes as "extra" and is appended → no
    * match.  This documents the behavior; the important property is
    * "no crash". */
   (void)pastime_thumbs_index_match(idx, "Bar Game (extra.gb");
   pastime_thumbs_index_free(idx);
}

static void test_null_and_empty_inputs(void)
{
   /* The parse + match + count + free APIs all promise NULL/empty
    * safety in the header.  These exercise those guards directly
    * — without them, a bug introduced in the early-return paths
    * would only surface in the manager and never in tests. */
   const char *titles[] = { "Sonic the Hedgehog (USA)" };
   pastime_thumbs_index_t *idx;

   /* _parse: bad inputs return NULL, never deref. */
   ASSERT_NULL(pastime_thumbs_index_parse(NULL, 0));
   ASSERT_NULL(pastime_thumbs_index_parse(NULL, 16));
   ASSERT_NULL(pastime_thumbs_index_parse("ignored", 0));
   /* Outright malformed JSON. */
   ASSERT_NULL(pastime_thumbs_index_parse("garbage", 7));
   ASSERT_NULL(pastime_thumbs_index_parse("{", 1));
   /* JSON without a "files" object — saw_files_obj guard. */
   ASSERT_NULL(pastime_thumbs_index_parse("{}", 2));
   ASSERT_NULL(pastime_thumbs_index_parse(
         "{\"system\":\"Test\"}", 17));

   /* _count: NULL returns 0. */
   ASSERT_TRUE(pastime_thumbs_index_count(NULL) == 0);

   /* _match: NULL idx and NULL/empty rom basename short-circuit. */
   idx = make_idx(titles, 1);
   ASSERT_NONNULL(idx);
   ASSERT_NULL(pastime_thumbs_index_match(NULL,  "Sonic.md"));
   ASSERT_NULL(pastime_thumbs_index_match(idx,   NULL));
   ASSERT_NULL(pastime_thumbs_index_match(idx,   ""));
   /* Sanity: a real query against the same idx still works. */
   ASSERT_STR_EQ(pastime_thumbs_index_match(idx,
         "Sonic the Hedgehog.md"),
         "Sonic the Hedgehog (USA)");
   pastime_thumbs_index_free(idx);

   /* _free: NULL is a no-op. */
   pastime_thumbs_index_free(NULL);
}

static void test_filename_extension_edge_cases(void)
{
   /* dp_strip_ext skips files starting with '.' — a bare ".gb"
    * input is treated literally as ".gb", not stripped to "".
    * A no-extension input is passed through unchanged. */
   const char *titles[] = { "Sonic the Hedgehog (USA)" };
   pastime_thumbs_index_t *idx = make_idx(titles, 1);
   /* No-extension query: "Sonic the Hedgehog" → matches. */
   ASSERT_STR_EQ(pastime_thumbs_index_match(idx,
         "Sonic the Hedgehog"),
         "Sonic the Hedgehog (USA)");
   /* Bare extension only: dp_strip_ext leaves ".gb" intact (skips
    * names starting with '.'); heavy normalize then yields "gb",
    * which doesn't match the canonical's "sonichedgehog" → miss. */
   ASSERT_NULL(pastime_thumbs_index_match(idx, ".gb"));
   pastime_thumbs_index_free(idx);
}

/* Round-trip width/height/thumbhash through the v2 binary format and
 * confirm the cascade returns the per-entry metadata.  The
 * "41kCFgZi..." string is a real thumbhash from the GB index (32
 * b64 chars → 24 bytes binary). */
static void test_v2_dims_and_thumbhash_round_trip(void)
{
   v2_entry_t es[] = {
      { "Pokemon Red (USA, Europe)", 440, 512,
            "41kCFgZiSQq2lwd3qFSXo7YrqI2P2fk=" },
      { "Tetris (World)",            512, 512, NULL /* no thumbhash */ },
   };
   char *json = build_idx_json_v2(es, 2);
   pastime_thumbs_index_t *idx;
   ASSERT_NONNULL(json);
   idx = pastime_thumbs_index_parse(json, strlen(json));
   free(json);
   ASSERT_NONNULL(idx);
   if (idx)
   {
      size_t mi = dp_idx_match(idx, "Pokemon Red (USA, Europe)");
      ASSERT_TRUE(mi != (size_t)-1);
      if (mi != (size_t)-1)
      {
         uint16_t w = 0, h = 0;
         const uint8_t *th = NULL;
         size_t thlen = 0;
         dp_idx_dims(idx, (uint32_t)mi, &w, &h);
         ASSERT_TRUE(w == 440);
         ASSERT_TRUE(h == 512);
         dp_idx_thumbhash(idx, (uint32_t)mi, &th, &thlen);
         ASSERT_NONNULL(th);
         ASSERT_TRUE(thlen > 0 && thlen <= 32);
         /* First byte of "41kCFgZi..." → b64('4')=56, b64('1')=53 →
          * (56<<2 | 53>>4) = 224 | 3 = 0xe3.  Spot-check the bytes
          * came back intact. */
         ASSERT_TRUE(th && thlen > 0 && th[0] == 0xe3);
      }
      /* Tetris omitted thumbhash → must read back as absent. */
      mi = dp_idx_match(idx, "Tetris (World)");
      ASSERT_TRUE(mi != (size_t)-1);
      if (mi != (size_t)-1)
      {
         uint16_t w = 0, h = 0;
         const uint8_t *th = (const uint8_t*)0x1; /* sentinel */
         size_t thlen = 999;
         dp_idx_dims(idx, (uint32_t)mi, &w, &h);
         ASSERT_TRUE(w == 512);
         ASSERT_TRUE(h == 512);
         dp_idx_thumbhash(idx, (uint32_t)mi, &th, &thlen);
         ASSERT_TRUE(th == NULL);
         ASSERT_TRUE(thlen == 0);
      }
      pastime_thumbs_index_free(idx);
   }
}

/* Forward-compat: parsing legacy JSON (no width/height/thumbhash
 * keys) succeeds and the missing fields read back as 0/NULL. */
static void test_v2_missing_fields_default_zero(void)
{
   const char *titles[] = { "Tetris (World)" };
   pastime_thumbs_index_t *idx = make_idx(titles, 1);
   ASSERT_NONNULL(idx);
   if (idx)
   {
      size_t mi = dp_idx_match(idx, "Tetris (World)");
      ASSERT_TRUE(mi != (size_t)-1);
      if (mi != (size_t)-1)
      {
         uint16_t w = 9, h = 9;
         const uint8_t *th = (const uint8_t*)0x1;
         size_t thlen = 999;
         dp_idx_dims(idx, (uint32_t)mi, &w, &h);
         ASSERT_TRUE(w == 0);
         ASSERT_TRUE(h == 0);
         dp_idx_thumbhash(idx, (uint32_t)mi, &th, &thlen);
         ASSERT_TRUE(th == NULL);
         ASSERT_TRUE(thlen == 0);
      }
      pastime_thumbs_index_free(idx);
   }
}

/* Malformed thumbhash base64 must not abort the parse — the entry
 * should still land in the index, with thumbhash absent. */
static void test_v2_malformed_thumbhash_tolerated(void)
{
   v2_entry_t es[] = {
      /* '!' isn't a valid b64 char → decoder returns 0, parser drops
       * the field. */
      { "Tetris (World)", 200, 200, "not!valid!base64!" },
   };
   char *json = build_idx_json_v2(es, 1);
   pastime_thumbs_index_t *idx;
   ASSERT_NONNULL(json);
   idx = pastime_thumbs_index_parse(json, strlen(json));
   free(json);
   ASSERT_NONNULL(idx);
   if (idx)
   {
      size_t mi = dp_idx_match(idx, "Tetris (World)");
      ASSERT_TRUE(mi != (size_t)-1);
      if (mi != (size_t)-1)
      {
         uint16_t w = 0, h = 0;
         const uint8_t *th = (const uint8_t*)0x1;
         size_t thlen = 999;
         dp_idx_dims(idx, (uint32_t)mi, &w, &h);
         ASSERT_TRUE(w == 200);
         ASSERT_TRUE(h == 200);
         dp_idx_thumbhash(idx, (uint32_t)mi, &th, &thlen);
         ASSERT_TRUE(th == NULL);
         ASSERT_TRUE(thlen == 0);
      }
      pastime_thumbs_index_free(idx);
   }
}

/* Helpers for the format-validation tests below. */
static void put_u32_le(uint8_t *p, uint32_t v)
{
   p[0] = (uint8_t)(v & 0xff);
   p[1] = (uint8_t)((v >>  8) & 0xff);
   p[2] = (uint8_t)((v >> 16) & 0xff);
   p[3] = (uint8_t)((v >> 24) & 0xff);
}

/* dp_idx_open consumes its buffer regardless of outcome — frees on
 * failure, transfers ownership on success.  Build a fresh-by-malloc
 * buffer so the open call can take it. */
static uint8_t *dup_buf(const uint8_t *src, size_t n)
{
   uint8_t *p = (uint8_t*)malloc(n);
   if (p)
      memcpy(p, src, n);
   return p;
}

/* Synthesize a valid v2 binary buffer with a single dim-less +
 * thumbhash-less entry, then perturb it for the validation tests.
 * Returns malloc'd buffer; caller frees. */
static uint8_t *build_minimal_v2_buffer(size_t *out_len)
{
   /* Layout: 40 header + 1*16 entries + 1*4 bcanon + 1*8 bheavy +
    *         strings("a\0a\0") + 0 thumbhash + 12 footer
    *       = 40 + 16 + 4 + 8 + 4 + 0 + 12 = 84.
    * Strings pool must be NUL-terminated; we put a single string
    * "a" (NUL-terminated) at offset 0 and reuse it for both
    * canonical and heavy. */
   const uint32_t magic = 0x48545044u;   /* DP_IDX_MAGIC */
   const uint32_t version = 2u;
   const uint32_t n = 1;
   const uint32_t strings_size = 2;     /* "a" + NUL */
   const uint32_t thumbhash_size = 0;
   const uint32_t entries_off = 40;
   const uint32_t bcanon_off  = entries_off + 1 * 16;
   const uint32_t bheavy_off  = bcanon_off + 1 * 4;
   const uint32_t strings_off = bheavy_off + 1 * 8;
   const uint32_t thumbhash_off = strings_off + strings_size;
   const uint32_t footer_off  = thumbhash_off + thumbhash_size;
   const size_t   total       = footer_off + 12;
   uint8_t       *buf = (uint8_t*)calloc(total, 1);
   if (!buf) return NULL;
   put_u32_le(buf +  0, magic);
   put_u32_le(buf +  4, version);
   put_u32_le(buf +  8, n);
   put_u32_le(buf + 12, strings_size);
   put_u32_le(buf + 16, thumbhash_size);
   put_u32_le(buf + 20, entries_off);
   put_u32_le(buf + 24, bcanon_off);
   put_u32_le(buf + 28, bheavy_off);
   put_u32_le(buf + 32, strings_off);
   put_u32_le(buf + 36, thumbhash_off);
   /* Entry 0: canon_off=0, heavy_off=0, w=0, h=0, th_off=0 */
   put_u32_le(buf + entries_off + 0, 0);
   put_u32_le(buf + entries_off + 4, 0);
   /* w/h u16 already zero from calloc; th_off u32 already zero */
   /* BY_CANONICAL[0] = 0 (already zero from calloc) */
   /* BY_HEAVY[0] = {hash=0, idx=0} (already zero) */
   buf[strings_off + 0] = 'a';
   buf[strings_off + 1] = '\0';
   /* Footer: magic + version + count */
   put_u32_le(buf + footer_off + 0, magic);
   put_u32_le(buf + footer_off + 4, version);
   put_u32_le(buf + footer_off + 8, n);
   *out_len = total;
   return buf;
}

/* Sanity: the synthetic minimal buffer opens cleanly.  Anchor for
 * the perturbation tests below — if THIS fails, every reject-case
 * assertion is meaningless. */
static void test_v2_minimal_synthetic_buffer_opens(void)
{
   size_t   n;
   uint8_t *buf = build_minimal_v2_buffer(&n);
   pastime_thumbs_index_t *idx;
   ASSERT_NONNULL(buf);
   if (!buf) return;
   /* Need a malloc'd copy because dp_idx_open frees on either path. */
   idx = dp_idx_open(dup_buf(buf, n), n);
   ASSERT_NONNULL(idx);
   if (idx)
   {
      ASSERT_TRUE(pastime_thumbs_index_count(idx) == 1);
      pastime_thumbs_index_free(idx);
   }
   free(buf);
}

/* Hand-edit the version field to 1 (was-v2-buffer pretending to be
 * v1).  Header reads version=1 → mismatch with DP_IDX_VERSION (2)
 * → reject.  Layered defense: even if the version check were
 * bypassed, the layout would mismatch (v1 records were 8 bytes, v2
 * are 16) — but the explicit check fails first. */
static void test_v2_rejects_version_mismatch(void)
{
   size_t   n;
   uint8_t *buf = build_minimal_v2_buffer(&n);
   pastime_thumbs_index_t *idx;
   if (!buf) { ASSERT_NONNULL(buf); return; }
   put_u32_le(buf + 4, 1u);  /* header version → 1 */
   idx = dp_idx_open(dup_buf(buf, n), n);
   ASSERT_NULL(idx);
   free(buf);
}

/* Footer carries a version mirror.  If header.version is 2 but
 * footer.version is anything else, reject — guards against partial
 * writes mid-flush as well as hand-edited headers that left the
 * footer stale. */
static void test_v2_rejects_footer_version_mismatch(void)
{
   size_t   n;
   uint8_t *buf = build_minimal_v2_buffer(&n);
   pastime_thumbs_index_t *idx;
   uint32_t footer_off;
   if (!buf) { ASSERT_NONNULL(buf); return; }
   /* footer_off = total - 12 */
   footer_off = (uint32_t)(n - 12);
   put_u32_le(buf + footer_off + 4, 99u); /* footer version */
   idx = dp_idx_open(dup_buf(buf, n), n);
   ASSERT_NULL(idx);
   free(buf);
}

/* Truncate the buffer mid-thumbhash-pool.  The validator's exact-
 * file-size check (`buf_len != expected`) must catch this — and the
 * thumbhash accessor's bounds check is the second line of defense
 * for the in-memory case. */
static void test_v2_rejects_truncated_buffer(void)
{
   size_t   n;
   uint8_t *buf = build_minimal_v2_buffer(&n);
   pastime_thumbs_index_t *idx;
   if (!buf) { ASSERT_NONNULL(buf); return; }
   /* Pass `n - 1` for a 1-byte-short buffer — buf_len mismatch
    * fails the layout check immediately. */
   idx = dp_idx_open(dup_buf(buf, n - 1), n - 1);
   ASSERT_NULL(idx);
   free(buf);
}

/* dp_idx_thumbhash is the centralised pool accessor.  Its bounds
 * check guarantees out-of-range thumbhash_off values can't trigger
 * an OOB read even on a corrupted-after-validation buffer.  Confirm
 * directly by handing it an entry whose th_off field exceeds the
 * declared pool size. */
static void test_v2_thumbhash_bounds_clamp(void)
{
   /* Build a single-entry buffer with thumbhash_size=4 (one valid
    * entry "len=1; bytes=0xAA"), but rewrite the entry's th_off to
    * 999 — way past the pool end.  Accessor must return NULL. */
   const uint32_t magic = 0x48545044u, version = 2u;
   const uint32_t n = 1, strings_size = 2, thumbhash_size = 2;
   const uint32_t entries_off = 40;
   const uint32_t bcanon_off  = entries_off + 16;
   const uint32_t bheavy_off  = bcanon_off + 4;
   const uint32_t strings_off = bheavy_off + 8;
   const uint32_t thumbhash_off = strings_off + strings_size;
   const uint32_t footer_off  = thumbhash_off + thumbhash_size;
   const size_t   total       = footer_off + 12;
   pastime_thumbs_index_t *idx;
   uint8_t       *buf = (uint8_t*)calloc(total, 1);
   if (!buf) { ASSERT_NONNULL(buf); return; }
   put_u32_le(buf +  0, magic);
   put_u32_le(buf +  4, version);
   put_u32_le(buf +  8, n);
   put_u32_le(buf + 12, strings_size);
   put_u32_le(buf + 16, thumbhash_size);
   put_u32_le(buf + 20, entries_off);
   put_u32_le(buf + 24, bcanon_off);
   put_u32_le(buf + 28, bheavy_off);
   put_u32_le(buf + 32, strings_off);
   put_u32_le(buf + 36, thumbhash_off);
   /* Entry 0: th_off = 999 (out of range) */
   put_u32_le(buf + entries_off + 12, 999u);
   buf[strings_off + 0] = 'a';
   buf[strings_off + 1] = '\0';
   buf[thumbhash_off + 0] = 0x01;  /* one-byte payload */
   buf[thumbhash_off + 1] = 0xAA;
   put_u32_le(buf + footer_off + 0, magic);
   put_u32_le(buf + footer_off + 4, version);
   put_u32_le(buf + footer_off + 8, n);
   idx = dp_idx_open(buf, total);
   ASSERT_NONNULL(idx);
   if (idx)
   {
      const uint8_t *th = (const uint8_t*)0x1;
      size_t thlen = 999;
      dp_idx_thumbhash(idx, 0, &th, &thlen);
      ASSERT_TRUE(th == NULL);
      ASSERT_TRUE(thlen == 0);
      pastime_thumbs_index_free(idx);
   }
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
   test_null_and_empty_inputs();
   test_filename_extension_edge_cases();

   test_v2_dims_and_thumbhash_round_trip();
   test_v2_missing_fields_default_zero();
   test_v2_malformed_thumbhash_tolerated();
   test_v2_minimal_synthetic_buffer_opens();
   test_v2_rejects_version_mismatch();
   test_v2_rejects_footer_version_mismatch();
   test_v2_rejects_truncated_buffer();
   test_v2_thumbhash_bounds_clamp();

   printf("test_thumbs: %d passed, %d failed\n", g_pass, g_fail);
   return g_fail ? 1 : 0;
}
