/* See pastime_external.h for intent. */

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "pastime_external.h"
#include "pastime_external_presets.h"

#ifdef PASTIME_EXTERNAL_TEST_BUILD
/* Test build: stub out the verbosity macros + the Android JNI bridge so
 * the test binary links without dragging RA's logger or the NDK glue. */
extern void pastime_external_test_log(const char *fmt, ...);
#define RARCH_LOG(...)  pastime_external_test_log(__VA_ARGS__)
#define RARCH_WARN(...) pastime_external_test_log(__VA_ARGS__)
#define RARCH_ERR(...)  pastime_external_test_log(__VA_ARGS__)
#else
#include "../verbosity.h"
#endif

#if defined(ANDROID) && !defined(PASTIME_EXTERNAL_TEST_BUILD)
/* Real device: dispatch to the JNI bridge in pastime_external_android.c. */
extern bool pastime_external_android_launch(
      const pastime_external_spec_t *spec, const char *rom_path);
extern bool pastime_external_android_is_installed(const char *package);
#endif

bool pastime_external_parse_marker(const char *inside_parens,
      const char **payload_out)
{
   const char *payload;
   bool        has_dot = false;
   bool        has_upper = false;
   size_t      i;
   size_t      len;

   if (!inside_parens)
      return false;
   /* Marker form: "ext-<payload>" where <payload> is either a full
    * Android package ("com.github.stenzek.duckstation") or a shortname
    * ("duckstation").  Dash specifically because:
    *   - colon is illegal on exFAT/FAT32 (the universal SD card formats
    *     and the way users will stage portable libraries from a desktop)
    *   - tilde is shell-expansion-prone and ugly in folder names
    *   - underscore and dot are legal *within* Android package names
    *     (e.g. "com.foo_bar.baz") so they aren't unambiguous separators
    *   - dash is filesystem-universal and is illegal in Java packages,
    *     making "ext-com.foo.bar" unambiguous from a libretro core
    *     ident (which is [a-z0-9_]+, no dashes either).
    * No back-compat for prior `ext:` / `ext~` forms — Pastime has no
    * deployed users yet; one separator is simpler and the migration is
    * a folder rename. */
   if (strncmp(inside_parens, "ext-", 4) != 0)
      return false;
   payload = inside_parens + 4;
   len = strlen(payload);
   if (len == 0)
      return false;

   /* Validate as the union of the two accepted alphabets:
    *   - full package: [A-Za-z0-9._]+ with at least one '.'
    *   - shortname:    [a-z0-9]+ with no '.'
    * Two-pass: first sweep classifies the chars and detects any '.',
    * then re-validates the body against the chosen alphabet.  Strict
    * shortname alphabet matches what the sync script actually emits
    * (`re.sub('[^a-z0-9]','', ...)`), so a folder like
    * `(ext-nodot_here)` fails parse rather than silently looking up an
    * impossible shortname. */
   for (i = 0; i < len; i++)
   {
      char c = payload[i];
      if (c == '.')
         has_dot = true;
      else if (c >= 'A' && c <= 'Z')
         has_upper = true;
      else if (!((c >= 'a' && c <= 'z')
              || (c >= '0' && c <= '9') || c == '_'))
         return false;
   }
   if (!has_dot)
   {
      /* Shortname form: strict [a-z0-9]+, no upper, no underscore. */
      if (has_upper)
         return false;
      for (i = 0; i < len; i++)
      {
         if (payload[i] == '_')
            return false;
      }
   }

   if (payload_out)
      *payload_out = payload;
   return true;
}

bool pastime_external_payload_is_shortname(const char *payload)
{
   if (!payload)
      return false;
   /* The parser already guarantees: dot iff full package, no dot iff
    * shortname.  This is a O(len) check called once per folder so it's
    * fine; would be worth caching if it ever showed up in a profile. */
   return strchr(payload, '.') == NULL;
}

const pastime_external_spec_t *pastime_external_lookup(const char *package)
{
   size_t i;

   if (!package)
      return NULL;
   for (i = 0; i < pastime_external_presets_count; i++)
   {
      if (strcmp(pastime_external_presets[i].package, package) == 0)
         return &pastime_external_presets[i];
   }
   return NULL;
}

const pastime_external_spec_t *pastime_external_lookup_shortname(
      const char *shortname)
{
   size_t i;

   if (!shortname)
      return NULL;
   for (i = 0; i < pastime_external_presets_count; i++)
   {
      const char *sn = pastime_external_presets[i].shortname;
      if (sn && strcmp(sn, shortname) == 0)
         return &pastime_external_presets[i];
   }
   return NULL;
}

bool pastime_external_is_installed(const char *package)
{
   if (!package || !*package)
      return false;
#if defined(ANDROID) && !defined(PASTIME_EXTERNAL_TEST_BUILD)
   return pastime_external_android_is_installed(package);
#else
   /* Desktop / test build: pretend everything's installed so external
    * folders stay visible for development.  Real launches are still
    * a no-op (handled in pastime_external_launch). */
   return true;
#endif
}

bool pastime_external_launch(const pastime_external_spec_t *spec,
      const char *rom_path)
{
   if (!spec || !rom_path || !*rom_path)
      return false;

#if defined(ANDROID) && !defined(PASTIME_EXTERNAL_TEST_BUILD)
   return pastime_external_android_launch(spec, rom_path);
#else
   RARCH_LOG("[Pastime] external launch requested for %s (no-op outside Android)\n",
         spec->package);
   return false;
#endif
}
