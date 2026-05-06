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
#endif

bool pastime_external_parse_marker(const char *inside_parens,
      const char **package_out)
{
   const char *pkg;
   bool        has_dot = false;
   size_t      i;
   size_t      len;

   if (!inside_parens)
      return false;
   /* Marker form: "ext-<package>".  Dash specifically because:
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
   pkg = inside_parens + 4;
   len = strlen(pkg);
   if (len == 0)
      return false;

   /* Android package names are dot-separated Java identifiers — the
    * conservative subset we need to accept here is [A-Za-z0-9._]+ with
    * at least one dot.  We don't enforce the per-segment "starts with
    * a letter" rule; preset lookup is the real guardrail. */
   for (i = 0; i < len; i++)
   {
      char c = pkg[i];
      if (c == '.')
         has_dot = true;
      else if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
              || (c >= '0' && c <= '9') || c == '_'))
         return false;
   }
   if (!has_dot)
      return false;

   if (package_out)
      *package_out = pkg;
   return true;
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
