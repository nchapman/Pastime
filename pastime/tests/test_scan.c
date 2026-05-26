/* Unit tests for pastime/pastime_scan.c.
 *
 * Tests the baked map cache, ROM name resolution pipeline, and
 * the .disabled predicate.  Compiles standalone with
 * PASTIME_SCAN_TEST_BUILD + PASTIME_ROMMAP_TEST_BUILD to stub
 * file I/O and link against real display_name + rommap logic.
 */

#define PASTIME_SCAN_TEST_BUILD
#define PASTIME_ROMMAP_TEST_BUILD

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../pastime_rommap.h"
#include "../pastime_rommap.c"
#include "../pastime_scan.h"
#include "../pastime_scan.c"
#include "../pastime_display_name.h"
#include "../pastime_display_name.c"

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

/* Helper: create a rommap from a string literal */
static pastime_rommap_t *map_from_str(const char *s)
{
   size_t len = strlen(s);
   char *buf  = (char *)malloc(len + 1);
   memcpy(buf, s, len + 1);
   return pastime_rommap_load_buf(buf, len);
}

/* ================================================================== */
/* .disabled predicate tests                                           */
/* ================================================================== */

static int test_disabled_basic(void)
{
   ASSERT_TRUE(pastime_name_is_disabled("Game Boy.disabled", 17));
   ASSERT_TRUE(pastime_name_is_disabled("x.disabled", 10));
   return 1;
}

static int test_disabled_too_short(void)
{
   ASSERT_FALSE(pastime_name_is_disabled(".disabled", 9));
   ASSERT_FALSE(pastime_name_is_disabled("disabled", 8));
   ASSERT_FALSE(pastime_name_is_disabled("", 0));
   return 1;
}

static int test_disabled_not_at_end(void)
{
   ASSERT_FALSE(pastime_name_is_disabled(".disabled.txt", 13));
   ASSERT_FALSE(pastime_name_is_disabled("foo.disabledx", 13));
   return 1;
}

static int test_disabled_exact_boundary(void)
{
   /* "a.disabled" = 10 chars, len > 9 is true */
   ASSERT_TRUE(pastime_name_is_disabled("a.disabled", 10));
   return 1;
}

/* ================================================================== */
/* Baked cache tests                                                    */
/* ================================================================== */

static int test_baked_cache_null_core(void)
{
   pastime_baked_cache_t c;
   pastime_baked_cache_init(&c);
   ASSERT_NULL(pastime_baked_cache_get(&c, NULL, "/assets"));
   ASSERT_NULL(pastime_baked_cache_get(&c, "", "/assets"));
   pastime_baked_cache_free(&c);
   return 1;
}

static int test_baked_cache_no_route(void)
{
   pastime_baked_cache_t c;
   pastime_baked_cache_init(&c);
   /* "gambatte" has no baked map route */
   ASSERT_NULL(pastime_baked_cache_get(&c, "gambatte", "/assets"));
   pastime_baked_cache_free(&c);
   return 1;
}

static int test_baked_cache_null_assets(void)
{
   pastime_baked_cache_t c;
   pastime_baked_cache_init(&c);
   /* fbneo routes to "arcade.txt" but assets_dir is NULL */
   ASSERT_NULL(pastime_baked_cache_get(&c, "fbneo", NULL));
   ASSERT_NULL(pastime_baked_cache_get(&c, "fbneo", ""));
   pastime_baked_cache_free(&c);
   return 1;
}

/* ================================================================== */
/* ROM name resolution tests                                           */
/* ================================================================== */

static int test_resolve_user_map_hit(void)
{
   pastime_baked_cache_t baked;
   pastime_rom_resolved_t res;
   pastime_rommap_t *umap;
   char display[256], sort[256], tag[256];

   pastime_baked_cache_init(&baked);
   umap = map_from_str("mslug.zip\tMetal Slug\n");
   ASSERT_NOT_NULL(umap);

   res = pastime_resolve_rom_name("mslug.zip", umap, "fbneo",
         &baked, NULL, 0,
         display, sizeof(display),
         sort, sizeof(sort),
         tag, sizeof(tag));

   ASSERT_FALSE(res.hidden);
   ASSERT_FALSE(res.skip);
   ASSERT_TRUE(res.mapped);
   ASSERT_STR_EQ(display, "Metal Slug");
   ASSERT_TRUE(sort[0] != '\0');

   pastime_rommap_free(umap);
   pastime_baked_cache_free(&baked);
   return 1;
}

static int test_resolve_hidden_rom(void)
{
   pastime_baked_cache_t baked;
   pastime_rom_resolved_t res;
   pastime_rommap_t *umap;
   char display[256], sort[256], tag[256];

   pastime_baked_cache_init(&baked);
   umap = map_from_str("neogeo.zip\t.hidden\n");

   res = pastime_resolve_rom_name("neogeo.zip", umap, "fbneo",
         &baked, NULL, 0,
         display, sizeof(display),
         sort, sizeof(sort),
         tag, sizeof(tag));

   ASSERT_TRUE(res.hidden);
   ASSERT_FALSE(res.skip);

   pastime_rommap_free(umap);
   pastime_baked_cache_free(&baked);
   return 1;
}

static int test_resolve_unmapped_clean(void)
{
   pastime_baked_cache_t baked;
   pastime_rom_resolved_t res;
   char display[256], sort[256], tag[256];

   pastime_baked_cache_init(&baked);

   res = pastime_resolve_rom_name(
         "Super Mario Bros. 3 (USA).nes", NULL, "mesen",
         &baked, NULL, 0,
         display, sizeof(display),
         sort, sizeof(sort),
         tag, sizeof(tag));

   ASSERT_FALSE(res.hidden);
   ASSERT_FALSE(res.skip);
   ASSERT_FALSE(res.mapped);
   ASSERT_STR_EQ(display, "Super Mario Bros. 3");
   ASSERT_TRUE(sort[0] != '\0');

   pastime_baked_cache_free(&baked);
   return 1;
}

static int test_resolve_unmapped_empty_after_clean(void)
{
   pastime_baked_cache_t baked;
   pastime_rom_resolved_t res;
   char display[256], sort[256], tag[256];

   pastime_baked_cache_init(&baked);

   /* A file that is entirely tags/brackets */
   res = pastime_resolve_rom_name(
         "(USA).nes", NULL, "mesen",
         &baked, NULL, 0,
         display, sizeof(display),
         sort, sizeof(sort),
         tag, sizeof(tag));

   ASSERT_TRUE(res.skip);

   pastime_baked_cache_free(&baked);
   return 1;
}

static int test_resolve_keep_tag(void)
{
   pastime_baked_cache_t baked;
   pastime_rom_resolved_t res;
   char display[256], sort[256], tag[256];

   pastime_baked_cache_init(&baked);

   res = pastime_resolve_rom_name(
         "Donkey Kong (USA) (Rev 1).gb", NULL, "gambatte",
         &baked, NULL, PASTIME_RESOLVE_KEEP_TAG,
         display, sizeof(display),
         sort, sizeof(sort),
         tag, sizeof(tag));

   ASSERT_FALSE(res.hidden);
   ASSERT_FALSE(res.skip);
   ASSERT_FALSE(res.mapped);
   ASSERT_STR_EQ(display, "Donkey Kong");
   ASSERT_TRUE(tag[0] != '\0');

   pastime_baked_cache_free(&baked);
   return 1;
}

static int test_resolve_null_basename(void)
{
   pastime_baked_cache_t baked;
   pastime_rom_resolved_t res;
   char display[256], sort[256], tag[256];

   pastime_baked_cache_init(&baked);

   res = pastime_resolve_rom_name(NULL, NULL, NULL,
         &baked, NULL, 0,
         display, sizeof(display),
         sort, sizeof(sort),
         tag, sizeof(tag));

   ASSERT_TRUE(res.skip);

   pastime_baked_cache_free(&baked);
   return 1;
}

static int test_resolve_empty_basename(void)
{
   pastime_baked_cache_t baked;
   pastime_rom_resolved_t res;
   char display[256], sort[256], tag[256];

   pastime_baked_cache_init(&baked);

   res = pastime_resolve_rom_name("", NULL, NULL,
         &baked, NULL, 0,
         display, sizeof(display),
         sort, sizeof(sort),
         tag, sizeof(tag));

   ASSERT_TRUE(res.skip);

   pastime_baked_cache_free(&baked);
   return 1;
}

static int test_resolve_mapped_sort_key(void)
{
   pastime_baked_cache_t baked;
   pastime_rom_resolved_t res;
   pastime_rommap_t *umap;
   char display[256], sort[256], tag[256];

   pastime_baked_cache_init(&baked);
   umap = map_from_str("kof98.zip\tThe King of Fighters '98\n");

   res = pastime_resolve_rom_name("kof98.zip", umap, "fbneo",
         &baked, NULL, 0,
         display, sizeof(display),
         sort, sizeof(sort),
         tag, sizeof(tag));

   ASSERT_TRUE(res.mapped);
   ASSERT_STR_EQ(display, "The King of Fighters '98");
   /* Sort key should strip leading "the " and lowercase */
   ASSERT_STR_EQ(sort, "king of fighters '98");

   pastime_rommap_free(umap);
   pastime_baked_cache_free(&baked);
   return 1;
}

static int test_resolve_extension_strip(void)
{
   pastime_baked_cache_t baked;
   pastime_rom_resolved_t res;
   char display[256], sort[256], tag[256];

   pastime_baked_cache_init(&baked);

   res = pastime_resolve_rom_name(
         "Celeste.p8.png", NULL, "pico8",
         &baked, NULL, 0,
         display, sizeof(display),
         sort, sizeof(sort),
         tag, sizeof(tag));

   ASSERT_FALSE(res.skip);
   ASSERT_STR_EQ(display, "Celeste");

   pastime_baked_cache_free(&baked);
   return 1;
}

/* ================================================================== */
/* .disabled predicate — additional cases                              */
/* ================================================================== */

static int test_disabled_case_sensitive(void)
{
   /* ".Disabled" and ".DISABLED" should NOT match */
   ASSERT_FALSE(pastime_name_is_disabled("Game.Disabled", 13));
   ASSERT_FALSE(pastime_name_is_disabled("Game.DISABLED", 13));
   return 1;
}

static int test_disabled_with_spaces(void)
{
   ASSERT_TRUE(pastime_name_is_disabled(
         "Nintendo DS (melonds).disabled", 30));
   return 1;
}

/* ================================================================== */
/* Baked cache — positive path tests                                    */
/* ================================================================== */

static int test_baked_cache_same_core_reuses(void)
{
   pastime_baked_cache_t c;
   pastime_rommap_t *m1, *m2;

   pastime_baked_cache_init(&c);
   /* fbneo routes to "arcade.txt" — but in test build,
    * pastime_rommap_load returns NULL (file not found).  The cache
    * should still store the fn so the second call doesn't re-attempt. */
   m1 = pastime_baked_cache_get(&c, "fbneo", "/fake/assets");
   m2 = pastime_baked_cache_get(&c, "fbneo", "/fake/assets");
   /* Both NULL (test build can't load), but fn should be cached */
   ASSERT_TRUE(m1 == m2);
   ASSERT_STR_EQ(c.fn, "arcade.txt");
   pastime_baked_cache_free(&c);
   return 1;
}

static int test_baked_cache_different_core_invalidates(void)
{
   pastime_baked_cache_t c;

   pastime_baked_cache_init(&c);
   pastime_baked_cache_get(&c, "fbneo", "/fake/assets");
   ASSERT_STR_EQ(c.fn, "arcade.txt");

   /* mame2003_plus also routes to "arcade.txt" — same file, no reload */
   pastime_baked_cache_get(&c, "mame2003_plus", "/fake/assets");
   ASSERT_STR_EQ(c.fn, "arcade.txt");

   /* A core with no route clears nothing — returns NULL */
   ASSERT_NULL(pastime_baked_cache_get(&c, "gambatte", "/fake/assets"));
   /* fn still cached from the arcade route */
   ASSERT_STR_EQ(c.fn, "arcade.txt");

   pastime_baked_cache_free(&c);
   return 1;
}

static int test_baked_cache_free_resets(void)
{
   pastime_baked_cache_t c;

   pastime_baked_cache_init(&c);
   pastime_baked_cache_get(&c, "fbneo", "/fake/assets");
   ASSERT_STR_EQ(c.fn, "arcade.txt");

   pastime_baked_cache_free(&c);
   ASSERT_TRUE(c.fn[0] == '\0');
   ASSERT_NULL(c.map);
   return 1;
}

/* ================================================================== */
/* ROM name resolution — additional cases                              */
/* ================================================================== */

static int test_resolve_user_map_miss_no_baked(void)
{
   /* User map exists but doesn't have this ROM; no baked route */
   pastime_baked_cache_t baked;
   pastime_rom_resolved_t res;
   pastime_rommap_t *umap;
   char display[256], sort[256], tag[256];

   pastime_baked_cache_init(&baked);
   umap = map_from_str("other.zip\tOther Game\n");

   res = pastime_resolve_rom_name("mygame (USA).smc", umap, "snes9x",
         &baked, NULL, 0,
         display, sizeof(display),
         sort, sizeof(sort),
         tag, sizeof(tag));

   ASSERT_FALSE(res.mapped);
   ASSERT_FALSE(res.hidden);
   ASSERT_FALSE(res.skip);
   ASSERT_STR_EQ(display, "mygame");

   pastime_rommap_free(umap);
   pastime_baked_cache_free(&baked);
   return 1;
}

static int test_resolve_hidden_via_dot_prefix(void)
{
   /* Map value starting with '.' means hidden, regardless of the char after */
   pastime_baked_cache_t baked;
   pastime_rom_resolved_t res;
   pastime_rommap_t *umap;
   char display[256], sort[256], tag[256];

   pastime_baked_cache_init(&baked);
   umap = map_from_str("bios.zip\t.bios\nsystem.zip\t.\n");

   res = pastime_resolve_rom_name("bios.zip", umap, NULL,
         &baked, NULL, 0,
         display, sizeof(display),
         sort, sizeof(sort),
         tag, sizeof(tag));
   ASSERT_TRUE(res.hidden);

   res = pastime_resolve_rom_name("system.zip", umap, NULL,
         &baked, NULL, 0,
         display, sizeof(display),
         sort, sizeof(sort),
         tag, sizeof(tag));
   ASSERT_TRUE(res.hidden);

   pastime_rommap_free(umap);
   pastime_baked_cache_free(&baked);
   return 1;
}

static int test_resolve_null_buffers(void)
{
   /* build_recents passes NULL for sort_buf and tag_buf */
   pastime_baked_cache_t baked;
   pastime_rom_resolved_t res;
   pastime_rommap_t *umap;
   char display[256];

   pastime_baked_cache_init(&baked);
   umap = map_from_str("mslug.zip\tMetal Slug\n");

   res = pastime_resolve_rom_name("mslug.zip", umap, "fbneo",
         &baked, NULL, 0,
         display, sizeof(display),
         NULL, 0, NULL, 0);

   ASSERT_TRUE(res.mapped);
   ASSERT_STR_EQ(display, "Metal Slug");
   /* sort_key and tag should be NULL since no buffers provided */
   ASSERT_NULL(res.sort_key);
   ASSERT_NULL(res.tag);

   pastime_rommap_free(umap);
   pastime_baked_cache_free(&baked);
   return 1;
}

static int test_resolve_null_buffers_unmapped(void)
{
   /* Unmapped path with NULL sort/tag buffers */
   pastime_baked_cache_t baked;
   pastime_rom_resolved_t res;
   char display[256];

   pastime_baked_cache_init(&baked);

   res = pastime_resolve_rom_name("Cool Game (USA).sfc", NULL, "snes9x",
         &baked, NULL, 0,
         display, sizeof(display),
         NULL, 0, NULL, 0);

   ASSERT_FALSE(res.skip);
   ASSERT_STR_EQ(display, "Cool Game");
   ASSERT_NULL(res.sort_key);

   pastime_baked_cache_free(&baked);
   return 1;
}

static int test_resolve_keep_tag_no_brackets(void)
{
   /* A ROM with no brackets/parens produces an empty tag */
   pastime_baked_cache_t baked;
   pastime_rom_resolved_t res;
   char display[256], sort[256], tag[256];

   pastime_baked_cache_init(&baked);

   res = pastime_resolve_rom_name("Tetris.gb", NULL, "gambatte",
         &baked, NULL, PASTIME_RESOLVE_KEEP_TAG,
         display, sizeof(display),
         sort, sizeof(sort),
         tag, sizeof(tag));

   ASSERT_FALSE(res.skip);
   ASSERT_STR_EQ(display, "Tetris");
   ASSERT_STR_EQ(tag, "");

   pastime_baked_cache_free(&baked);
   return 1;
}

static int test_resolve_extension_only_filename(void)
{
   /* ".nes" → strip ext → "" → skip */
   pastime_baked_cache_t baked;
   pastime_rom_resolved_t res;
   char display[256], sort[256], tag[256];

   pastime_baked_cache_init(&baked);

   res = pastime_resolve_rom_name(".nes", NULL, "mesen",
         &baked, NULL, 0,
         display, sizeof(display),
         sort, sizeof(sort),
         tag, sizeof(tag));

   ASSERT_TRUE(res.skip);

   pastime_baked_cache_free(&baked);
   return 1;
}

static int test_resolve_unmapped_sort_key_strips_article(void)
{
   /* "The Legend of Zelda (USA).sfc" → display "The Legend of Zelda",
    * sort key "legend of zelda" */
   pastime_baked_cache_t baked;
   pastime_rom_resolved_t res;
   char display[256], sort[256], tag[256];

   pastime_baked_cache_init(&baked);

   res = pastime_resolve_rom_name(
         "Legend of Zelda, The (USA).sfc", NULL, "snes9x",
         &baked, NULL, 0,
         display, sizeof(display),
         sort, sizeof(sort),
         tag, sizeof(tag));

   ASSERT_FALSE(res.skip);
   ASSERT_STR_EQ(display, "The Legend of Zelda");
   ASSERT_STR_EQ(sort, "legend of zelda");

   pastime_baked_cache_free(&baked);
   return 1;
}

static int test_resolve_mapped_bypasses_clean(void)
{
   /* Mapped names are used verbatim — brackets/parens are NOT stripped */
   pastime_baked_cache_t baked;
   pastime_rom_resolved_t res;
   pastime_rommap_t *umap;
   char display[256], sort[256], tag[256];

   pastime_baked_cache_init(&baked);
   umap = map_from_str("game.zip\tMetal Slug (Neo Geo)\n");

   res = pastime_resolve_rom_name("game.zip", umap, NULL,
         &baked, NULL, 0,
         display, sizeof(display),
         sort, sizeof(sort),
         tag, sizeof(tag));

   ASSERT_TRUE(res.mapped);
   /* Parens preserved — maps provide curated display names */
   ASSERT_STR_EQ(display, "Metal Slug (Neo Geo)");

   pastime_rommap_free(umap);
   pastime_baked_cache_free(&baked);
   return 1;
}

static int test_resolve_multiple_extensions(void)
{
   /* Various ROM extensions should all be stripped cleanly */
   pastime_baked_cache_t baked;
   pastime_rom_resolved_t res;
   char display[256], sort[256], tag[256];
   const char *files[] = {
      "Sonic.smc", "Sonic.zip", "Sonic.7z", "Sonic.bin", "Sonic.iso"
   };
   size_t i;

   pastime_baked_cache_init(&baked);

   for (i = 0; i < 5; i++)
   {
      res = pastime_resolve_rom_name(files[i], NULL, "genesis_plus_gx",
            &baked, NULL, 0,
            display, sizeof(display),
            sort, sizeof(sort),
            tag, sizeof(tag));
      ASSERT_FALSE(res.skip);
      ASSERT_STR_EQ(display, "Sonic");
   }

   pastime_baked_cache_free(&baked);
   return 1;
}

static int test_resolve_keep_tag_mapped_ignores_flag(void)
{
   /* When the map hits, KEEP_TAG has no effect — mapped names are verbatim */
   pastime_baked_cache_t baked;
   pastime_rom_resolved_t res;
   pastime_rommap_t *umap;
   char display[256], sort[256], tag[256];

   pastime_baked_cache_init(&baked);
   umap = map_from_str("mslug.zip\tMetal Slug\n");

   res = pastime_resolve_rom_name("mslug.zip", umap, "fbneo",
         &baked, NULL, PASTIME_RESOLVE_KEEP_TAG,
         display, sizeof(display),
         sort, sizeof(sort),
         tag, sizeof(tag));

   ASSERT_TRUE(res.mapped);
   ASSERT_STR_EQ(display, "Metal Slug");
   /* Tag should be empty/unchanged — not extracted from mapped name */
   ASSERT_STR_EQ(tag, "");

   pastime_rommap_free(umap);
   pastime_baked_cache_free(&baked);
   return 1;
}

static int test_resolve_null_baked_cache(void)
{
   /* Passing NULL for baked should not crash — just skip baked lookup */
   pastime_rom_resolved_t res;
   pastime_rommap_t *umap;
   char display[256], sort[256], tag[256];

   umap = map_from_str("x.zip\tGame X\n");

   res = pastime_resolve_rom_name("y.zip", umap, "fbneo",
         NULL, "/assets", 0,
         display, sizeof(display),
         sort, sizeof(sort),
         tag, sizeof(tag));

   /* User map misses, baked is NULL → unmapped path */
   ASSERT_FALSE(res.mapped);
   ASSERT_FALSE(res.skip);
   ASSERT_STR_EQ(display, "y");

   pastime_rommap_free(umap);
   return 1;
}

/* ================================================================== */
/* main                                                                */
/* ================================================================== */

int main(void)
{
   printf("== pastime_scan tests ==\n\n");

   printf("--- .disabled predicate ---\n");
   RUN_TEST(test_disabled_basic);
   RUN_TEST(test_disabled_too_short);
   RUN_TEST(test_disabled_not_at_end);
   RUN_TEST(test_disabled_exact_boundary);
   RUN_TEST(test_disabled_case_sensitive);
   RUN_TEST(test_disabled_with_spaces);

   printf("--- baked cache ---\n");
   RUN_TEST(test_baked_cache_null_core);
   RUN_TEST(test_baked_cache_no_route);
   RUN_TEST(test_baked_cache_null_assets);
   RUN_TEST(test_baked_cache_same_core_reuses);
   RUN_TEST(test_baked_cache_different_core_invalidates);
   RUN_TEST(test_baked_cache_free_resets);

   printf("--- ROM name resolution ---\n");
   RUN_TEST(test_resolve_user_map_hit);
   RUN_TEST(test_resolve_hidden_rom);
   RUN_TEST(test_resolve_unmapped_clean);
   RUN_TEST(test_resolve_unmapped_empty_after_clean);
   RUN_TEST(test_resolve_keep_tag);
   RUN_TEST(test_resolve_null_basename);
   RUN_TEST(test_resolve_empty_basename);
   RUN_TEST(test_resolve_mapped_sort_key);
   RUN_TEST(test_resolve_extension_strip);
   RUN_TEST(test_resolve_user_map_miss_no_baked);
   RUN_TEST(test_resolve_hidden_via_dot_prefix);
   RUN_TEST(test_resolve_null_buffers);
   RUN_TEST(test_resolve_null_buffers_unmapped);
   RUN_TEST(test_resolve_keep_tag_no_brackets);
   RUN_TEST(test_resolve_extension_only_filename);
   RUN_TEST(test_resolve_unmapped_sort_key_strips_article);
   RUN_TEST(test_resolve_mapped_bypasses_clean);
   RUN_TEST(test_resolve_multiple_extensions);
   RUN_TEST(test_resolve_keep_tag_mapped_ignores_flag);
   RUN_TEST(test_resolve_null_baked_cache);

   printf("\n%d/%d passed\n", g_tests_passed, g_tests_run);
   return g_tests_passed == g_tests_run ? 0 : 1;
}
