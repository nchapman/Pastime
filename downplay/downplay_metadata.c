/*  Downplay - a fork of RetroArch.
 *  Copyright (C) 2026 - Downplay contributors.
 *
 *  Downplay is free software: you can redistribute it and/or modify it under
 *  the terms of the GNU General Public License as published by the Free
 *  Software Foundation, either version 3 of the License, or (at your option)
 *  any later version.
 *
 *  Downplay is distributed in the hope that it will be useful, but WITHOUT ANY
 *  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 *  FOR A PARTICULAR PURPOSE. See the GNU General Public License for more
 *  details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with Downplay. If not, see <http://www.gnu.org/licenses/>.
 */

/* Per-system art-state index.  Persists DP_ART_{UNKNOWN,OK,MISSING}
 * per (system, basename) so the row drawer can pre-truncate
 * known-art rows on system enter, before the thumbnail manager has
 * looked anything up.
 *
 * No matching, no RDB, no worker thread, no background HTTP — those
 * lived here in an earlier incarnation that synthesized canonical
 * labels by running CRC/serial queries against libretro-database;
 * they were removed once the pastime.gg-backed thumbnail pipeline
 * (downplay_thumbs.c) took over both label resolution (via filename
 * normalization) and art lookup.  Kept around purely as a tiny
 * persistence layer for the row drawer's pre-truncation hint.
 *
 * The disambiguation table + downplay_metadata_resolve_db_name live
 * in downplay_metadata_disambig.c so they stay testable in isolation;
 * see downplay/tests/test_metadata_disambig.c. */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <boolean.h>
#include <file/file_path.h>
#include <retro_assert.h>
#include <retro_miscellaneous.h>
#include <streams/file_stream.h>
#include <streams/interface_stream.h>
#include <string/stdstring.h>
#include <formats/rjson.h>

#include "downplay_metadata.h"
#include "downplay_defaults.h"

#include "../verbosity.h"

/* Bumped from the prior schema (which carried label/match_kind/
 * match_value/art_checked_at) so any pre-existing on-disk index is
 * discarded on first read; the new format is a strict subset. */
#define DP_INDEX_SCHEMA_VERSION 2

typedef struct
{
   char    *basename;     /* heap-owned dictionary key */
   int64_t  mtime;
   int64_t  size;
   enum downplay_art_state art_state;
   bool     present;      /* set by note_present, cleared at start of scan */
} dp_entry_t;

struct downplay_index
{
   char        *json_path;
   char        *system_folder;
   char        *db_name;          /* informational only; written to JSON */
   dp_entry_t  *entries;
   size_t       entries_count;
   size_t       entries_cap;

   /* Open-addressing basename → entry-index hash table.  Each slot
    * holds (entry_index + 1); 0 means empty.  Sized to keep load
    * factor below 0.5.  Replaces a per-call linear scan over
    * entries[] — system-enter previously did O(R*E) string compares
    * across the reconcile loop (note_present + lookup), bad enough
    * to hitch the menu transition on a 1500-ROM PSX folder. */
   uint32_t    *ht;
   size_t       htmask;            /* (htsize - 1); htsize is a power of 2 */

   bool         dirty;
};

/* fnv1a-32 over a NUL-terminated string.  Same constants as the
 * thumbnail index's hasher; cheap, no table, well-distributed for
 * filename-shaped keys. */
static uint32_t dp_idx_hash_basename(const char *s)
{
   uint32_t h = 0x811c9dc5u;
   while (*s)
   {
      h ^= (unsigned char)*s++;
      h *= 0x01000193u;
   }
   return h;
}

/* Grow (or create) the hash table to the next power-of-two size.
 * Re-inserts every existing slot.  Caller invokes this when load
 * factor is about to exceed 0.5 OR when the entries array has been
 * compacted and the table needs rebuilding from scratch. */
static bool dp_ht_grow(downplay_index_t *idx)
{
   size_t    new_size = idx->htmask ? (idx->htmask + 1) * 2 : 64;
   uint32_t *nht;
   size_t    i;
   nht = (uint32_t*)calloc(new_size, sizeof(*nht));
   if (!nht)
      return false;
   if (idx->ht)
   {
      for (i = 0; i <= idx->htmask; i++)
      {
         uint32_t v = idx->ht[i];
         if (!v)
            continue;
         {
            const char *bn  = idx->entries[v - 1].basename;
            size_t      pos = dp_idx_hash_basename(bn) & (new_size - 1);
            while (nht[pos])
               pos = (pos + 1) & (new_size - 1);
            nht[pos] = v;
         }
      }
      free(idx->ht);
   }
   idx->ht     = nht;
   idx->htmask = new_size - 1;
   return true;
}

/* Reserve enough hash-table capacity for `expected_count` entries
 * at < 0.5 load.  Idempotent + cheap once large enough. */
static bool dp_ht_reserve(downplay_index_t *idx, size_t expected_count)
{
   while (!idx->ht || expected_count * 2 > idx->htmask + 1)
      if (!dp_ht_grow(idx))
         return false;
   return true;
}

/* Insert entry index `e_idx` into the hash table.  Caller must have
 * reserved capacity via dp_ht_reserve; this does no growing. */
static void dp_ht_insert(downplay_index_t *idx, uint32_t e_idx)
{
   const char *bn  = idx->entries[e_idx].basename;
   size_t      pos = dp_idx_hash_basename(bn) & idx->htmask;
   while (idx->ht[pos])
      pos = (pos + 1) & idx->htmask;
   idx->ht[pos] = e_idx + 1;
}

/* Wipe the hash table and re-insert every live entry.  Used after
 * downplay_index_finish_scan compacts entries[] in place.
 *
 * On reserve failure we still wipe the existing table — leaving the
 * pre-compaction slots in place would have them pointing at moved
 * (or freed) entry indices, which is a memory-safety hazard, not a
 * graceful degradation.  After the wipe, lookups answer NULL until
 * a future create grows ht; that's the same effect the comment in
 * the caller describes. */
static bool dp_ht_rebuild(downplay_index_t *idx)
{
   size_t i;
   if (!dp_ht_reserve(idx, idx->entries_count))
   {
      if (idx->ht)
         memset(idx->ht, 0, (idx->htmask + 1) * sizeof(*idx->ht));
      return false;
   }
   memset(idx->ht, 0, (idx->htmask + 1) * sizeof(*idx->ht));
   for (i = 0; i < idx->entries_count; i++)
      dp_ht_insert(idx, (uint32_t)i);
   return true;
}

static dp_entry_t *dp_entries_find(downplay_index_t *idx,
      const char *basename)
{
   size_t pos;
   if (!idx || !basename || !idx->ht || idx->entries_count == 0)
      return NULL;
   pos = dp_idx_hash_basename(basename) & idx->htmask;
   while (idx->ht[pos])
   {
      uint32_t    v = idx->ht[pos];
      dp_entry_t *e = &idx->entries[v - 1];
      if (string_is_equal(e->basename, basename))
         return e;
      pos = (pos + 1) & idx->htmask;
   }
   return NULL;
}

static dp_entry_t *dp_entries_create(downplay_index_t *idx,
      const char *basename)
{
   dp_entry_t *e;
   if (idx->entries_count == idx->entries_cap)
   {
      size_t      new_cap = idx->entries_cap ? idx->entries_cap * 2 : 16;
      dp_entry_t *grown   = (dp_entry_t*)realloc(idx->entries,
            new_cap * sizeof(*idx->entries));
      if (!grown)
         return NULL;
      idx->entries     = grown;
      idx->entries_cap = new_cap;
   }
   /* Reserve hash-table capacity AFTER the entries realloc succeeds.
    * If we reserved first and then the entries realloc failed, the
    * table would be over-sized for entries_count and stay that way,
    * silently wasting memory after every OOM event. */
   if (!dp_ht_reserve(idx, idx->entries_count + 1))
      return NULL;
   e = &idx->entries[idx->entries_count];
   memset(e, 0, sizeof(*e));
   e->basename = strdup(basename);
   if (!e->basename)
      return NULL;
   dp_ht_insert(idx, (uint32_t)idx->entries_count);
   idx->entries_count++;
   return e;
}

static void dp_entry_free(dp_entry_t *e)
{
   if (!e)
      return;
   free(e->basename);
   e->basename = NULL;
}

/* ---------- JSON load/save ---------- */

static const char *dp_art_state_to_str(enum downplay_art_state a)
{
   switch (a)
   {
      case DP_ART_OK:      return "ok";
      case DP_ART_MISSING: return "missing";
      case DP_ART_UNKNOWN: break;
   }
   return "unknown";
}

static enum downplay_art_state dp_art_state_from_str(const char *s)
{
   if (s)
   {
      if (string_is_equal(s, "ok"))      return DP_ART_OK;
      if (string_is_equal(s, "missing")) return DP_ART_MISSING;
   }
   return DP_ART_UNKNOWN;
}

enum dp_parse_state
{
   DP_PS_ROOT = 0,
   DP_PS_ROOT_KEY,
   DP_PS_ROOT_VAL,
   DP_PS_ENTRIES_KEY,
   DP_PS_ENTRIES_VAL,
   DP_PS_ENTRY_KEY,
   DP_PS_ENTRY_VAL
};

typedef struct
{
   downplay_index_t   *idx;
   dp_entry_t         *cur_entry;
   enum dp_parse_state state;
   int                 file_version;
   char                last_key[64];
   char                cur_basename[NAME_MAX_LENGTH];
} dp_parse_ctx_t;

static bool dp_parse_start_object(void *user)
{
   dp_parse_ctx_t *ctx = (dp_parse_ctx_t*)user;
   switch (ctx->state)
   {
      case DP_PS_ROOT:
         ctx->state = DP_PS_ROOT_KEY;
         break;
      case DP_PS_ROOT_VAL:
         if (string_is_equal(ctx->last_key, "entries"))
            ctx->state = DP_PS_ENTRIES_KEY;
         else
            return false;
         break;
      case DP_PS_ENTRIES_VAL:
         ctx->state = DP_PS_ENTRY_KEY;
         break;
      default:
         return false;
   }
   return true;
}

static bool dp_parse_end_object(void *user)
{
   dp_parse_ctx_t *ctx = (dp_parse_ctx_t*)user;
   switch (ctx->state)
   {
      case DP_PS_ROOT_KEY:
         ctx->state = DP_PS_ROOT;
         break;
      case DP_PS_ENTRIES_KEY:
         ctx->state = DP_PS_ROOT_KEY;
         break;
      case DP_PS_ENTRY_KEY:
         ctx->cur_entry = NULL;
         ctx->state     = DP_PS_ENTRIES_KEY;
         break;
      default:
         return false;
   }
   return true;
}

static bool dp_parse_object_member(void *user, const char *str, size_t len)
{
   dp_parse_ctx_t *ctx = (dp_parse_ctx_t*)user;
   (void)len;
   switch (ctx->state)
   {
      case DP_PS_ROOT_KEY:
         strlcpy(ctx->last_key, str, sizeof(ctx->last_key));
         ctx->state = DP_PS_ROOT_VAL;
         break;
      case DP_PS_ENTRIES_KEY:
         strlcpy(ctx->cur_basename, str, sizeof(ctx->cur_basename));
         ctx->cur_entry = dp_entries_create(ctx->idx, ctx->cur_basename);
         ctx->state     = DP_PS_ENTRIES_VAL;
         break;
      case DP_PS_ENTRY_KEY:
         strlcpy(ctx->last_key, str, sizeof(ctx->last_key));
         ctx->state = DP_PS_ENTRY_VAL;
         break;
      default:
         return false;
   }
   return true;
}

static bool dp_parse_string(void *user, const char *str, size_t len)
{
   dp_parse_ctx_t *ctx = (dp_parse_ctx_t*)user;
   (void)len;
   if (ctx->state == DP_PS_ROOT_VAL)
   {
      if (string_is_equal(ctx->last_key, "db_name") && !ctx->idx->db_name)
         ctx->idx->db_name = strdup(str);
      ctx->state = DP_PS_ROOT_KEY;
      return true;
   }
   if (ctx->state == DP_PS_ENTRY_VAL)
   {
      if (ctx->cur_entry && string_is_equal(ctx->last_key, "art"))
         ctx->cur_entry->art_state = dp_art_state_from_str(str);
      ctx->state = DP_PS_ENTRY_KEY;
      return true;
   }
   return false;
}

static bool dp_parse_number(void *user, const char *str, size_t len)
{
   dp_parse_ctx_t *ctx = (dp_parse_ctx_t*)user;
   long long       n   = atoll(str);
   (void)len;
   if (ctx->state == DP_PS_ROOT_VAL)
   {
      if (string_is_equal(ctx->last_key, "version"))
         ctx->file_version = (int)n;
      ctx->state = DP_PS_ROOT_KEY;
      return true;
   }
   if (ctx->state == DP_PS_ENTRY_VAL && ctx->cur_entry)
   {
      if (string_is_equal(ctx->last_key, "mtime"))
         ctx->cur_entry->mtime = (int64_t)n;
      else if (string_is_equal(ctx->last_key, "size"))
         ctx->cur_entry->size = (int64_t)n;
      ctx->state = DP_PS_ENTRY_KEY;
      return true;
   }
   return false;
}

static bool dp_parse_null(void *user)
{
   dp_parse_ctx_t *ctx = (dp_parse_ctx_t*)user;
   if (ctx->state == DP_PS_ENTRY_VAL)
   {
      ctx->state = DP_PS_ENTRY_KEY;
      return true;
   }
   if (ctx->state == DP_PS_ROOT_VAL)
   {
      ctx->state = DP_PS_ROOT_KEY;
      return true;
   }
   return false;
}

static bool dp_index_load_json(downplay_index_t *idx)
{
   intfstream_t   *file;
   rjson_t        *json;
   dp_parse_ctx_t  ctx;
   enum rjson_type result;

   if (!path_is_valid(idx->json_path))
      return true; /* fresh index — not an error */

   file = intfstream_open_file(idx->json_path,
         RETRO_VFS_FILE_ACCESS_READ, RETRO_VFS_FILE_ACCESS_HINT_NONE);
   if (!file)
      return false;
   json = rjson_open_stream((struct intfstream_internal*)file);
   if (!json)
   {
      intfstream_close(file);
      free(file);
      return false;
   }

   memset(&ctx, 0, sizeof(ctx));
   ctx.idx   = idx;
   ctx.state = DP_PS_ROOT;

   result = rjson_parse(json, &ctx,
         dp_parse_object_member,
         dp_parse_string,
         dp_parse_number,
         dp_parse_start_object,
         dp_parse_end_object,
         NULL, NULL, NULL,
         dp_parse_null);

   rjson_free(json);
   intfstream_close(file);
   free(file);

   if (result != RJSON_DONE)
   {
      RARCH_WARN("[Downplay] index parse failed for %s; rebuilding\n",
            idx->json_path);
      return false;
   }
   if (ctx.file_version != DP_INDEX_SCHEMA_VERSION)
   {
      RARCH_LOG("[Downplay] index %s is schema v%d (current v%d); "
            "rebuilding\n",
            idx->json_path, ctx.file_version, DP_INDEX_SCHEMA_VERSION);
      return false;
   }
   return true;
}

/* atoll-safe int64 emit: rjsonwriter_add_double formats via "%G" and
 * switches to scientific notation past ~6 sig figs, which corrupts
 * unix mtimes on round-trip.  Bypass with raw printf. */
static void dp_write_int64(rjsonwriter_t *w, int64_t v)
{
   rjsonwriter_rawf(w, "%lld", (long long)v);
}

static bool dp_index_save_json(downplay_index_t *idx)
{
   char           tmp_path[PATH_MAX_LENGTH];
   intfstream_t  *file;
   rjsonwriter_t *writer;
   size_t         i;
   bool           first_entry = true;

   if (!idx->json_path)
      return false;

   snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", idx->json_path);

   file = intfstream_open_file(tmp_path,
         RETRO_VFS_FILE_ACCESS_WRITE, RETRO_VFS_FILE_ACCESS_HINT_NONE);
   if (!file)
   {
      RARCH_ERR("[Downplay] index save: cannot open %s\n", tmp_path);
      return false;
   }
   writer = rjsonwriter_open_stream((struct intfstream_internal*)file);
   if (!writer)
   {
      intfstream_close(file);
      free(file);
      return false;
   }

   rjsonwriter_raw(writer, "{\n", 2);

   rjsonwriter_add_spaces(writer, 2);
   rjsonwriter_add_string(writer, "version");
   rjsonwriter_raw(writer, ": ", 2);
   rjsonwriter_rawf(writer, "%d", DP_INDEX_SCHEMA_VERSION);
   rjsonwriter_raw(writer, ",\n", 2);

   rjsonwriter_add_spaces(writer, 2);
   rjsonwriter_add_string(writer, "db_name");
   rjsonwriter_raw(writer, ": ", 2);
   rjsonwriter_add_string(writer, idx->db_name ? idx->db_name : "");
   rjsonwriter_raw(writer, ",\n", 2);

   rjsonwriter_add_spaces(writer, 2);
   rjsonwriter_add_string(writer, "entries");
   rjsonwriter_raw(writer, ": {\n", 4);

   for (i = 0; i < idx->entries_count; i++)
   {
      const dp_entry_t *e = &idx->entries[i];
      if (!e->basename)
         continue;
      if (!first_entry)
         rjsonwriter_raw(writer, ",\n", 2);
      first_entry = false;

      rjsonwriter_add_spaces(writer, 4);
      rjsonwriter_add_string(writer, e->basename);
      rjsonwriter_raw(writer, ": {\n", 4);

      rjsonwriter_add_spaces(writer, 6);
      rjsonwriter_add_string(writer, "mtime");
      rjsonwriter_raw(writer, ": ", 2);
      dp_write_int64(writer, e->mtime);
      rjsonwriter_raw(writer, ",\n", 2);

      rjsonwriter_add_spaces(writer, 6);
      rjsonwriter_add_string(writer, "size");
      rjsonwriter_raw(writer, ": ", 2);
      dp_write_int64(writer, e->size);
      rjsonwriter_raw(writer, ",\n", 2);

      rjsonwriter_add_spaces(writer, 6);
      rjsonwriter_add_string(writer, "art");
      rjsonwriter_raw(writer, ": ", 2);
      rjsonwriter_add_string(writer, dp_art_state_to_str(e->art_state));
      rjsonwriter_raw(writer, "\n", 1);

      rjsonwriter_add_spaces(writer, 4);
      rjsonwriter_raw(writer, "}", 1);
   }

   rjsonwriter_raw(writer, "\n", 1);
   rjsonwriter_add_spaces(writer, 2);
   rjsonwriter_raw(writer, "}\n", 2);
   rjsonwriter_raw(writer, "}\n", 2);

   rjsonwriter_free(writer);
   intfstream_close(file);
   free(file);

   if (rename(tmp_path, idx->json_path) != 0)
   {
      RARCH_ERR("[Downplay] index save: rename %s -> %s failed\n",
            tmp_path, idx->json_path);
      return false;
   }
   return true;
}

/* ---------- public API ---------- */

downplay_index_t *downplay_index_open(const char *system_folder_name,
      const char *system_root_path, const char *db_name)
{
   downplay_index_t *idx;
   char              index_root[PATH_MAX_LENGTH];
   char              json_path[PATH_MAX_LENGTH];
   char              file_name[NAME_MAX_LENGTH];

   (void)system_root_path; /* historical — no longer needed for matching */

   if (!system_folder_name || !*system_folder_name)
      return NULL;
   if (!downplay_paths_get_index_root(index_root, sizeof(index_root)))
      return NULL;

   snprintf(file_name, sizeof(file_name), "%s.json", system_folder_name);
   fill_pathname_join_special(json_path, index_root, file_name,
         sizeof(json_path));

   idx = (downplay_index_t*)calloc(1, sizeof(*idx));
   if (!idx)
      return NULL;
   idx->json_path     = strdup(json_path);
   idx->system_folder = strdup(system_folder_name);
   if (db_name && *db_name)
      idx->db_name    = strdup(db_name);
   if (!idx->json_path || !idx->system_folder)
   {
      downplay_index_close(idx);
      return NULL;
   }

   if (!dp_index_load_json(idx))
   {
      size_t i;
      for (i = 0; i < idx->entries_count; i++)
         dp_entry_free(&idx->entries[i]);
      idx->entries_count = 0;
      /* Wipe ht in lockstep so a fresh rebuild starts from empty. */
      if (idx->ht)
         memset(idx->ht, 0, (idx->htmask + 1) * sizeof(*idx->ht));
   }

   return idx;
}

void downplay_index_close(downplay_index_t *idx)
{
   size_t i;
   if (!idx)
      return;
   downplay_index_flush(idx);
   for (i = 0; i < idx->entries_count; i++)
      dp_entry_free(&idx->entries[i]);
   free(idx->entries);
   free(idx->ht);
   free(idx->json_path);
   free(idx->system_folder);
   free(idx->db_name);
   free(idx);
}

/* The mtime/size parameters are vestigial — kept on the API surface
 * so callers don't have to change, but no longer drive validity
 * checks.  We used to invalidate the cached art_state on a (basename,
 * mtime, size) mismatch, but the per-file stat() that produced those
 * numbers cost ~150 ms per system enter on Android FUSE.  art_state
 * is layout-only (truncation gate) and self-heals on first hover via
 * drive_system_thumbnails → downplay_index_set_art_state, so a stale
 * value is at worst a one-frame layout miss, never a wrong launch. */
bool downplay_index_lookup(downplay_index_t *idx,
      const char *basename, time_t mtime, int64_t size,
      downplay_index_record_t *out)
{
   dp_entry_t *e;
   (void)mtime; (void)size;
   if (!out)
      return false;
   memset(out, 0, sizeof(*out));
   if (!idx || !basename)
      return false;
   e = dp_entries_find(idx, basename);
   if (!e)
      return false;
   out->art_state = e->art_state;
   return true;
}

void downplay_index_note_present(downplay_index_t *idx,
      const char *basename, time_t mtime, int64_t size)
{
   dp_entry_t *e;
   (void)mtime; (void)size;
   if (!idx || !basename || !*basename)
      return;
   e = dp_entries_find(idx, basename);
   if (!e)
   {
      e = dp_entries_create(idx, basename);
      if (!e)
         return;
      idx->dirty = true; /* new entry */
   }
   e->present = true;
}

void downplay_index_finish_scan(downplay_index_t *idx)
{
   size_t read_i;
   size_t write_i = 0;
   if (!idx)
      return;
   for (read_i = 0; read_i < idx->entries_count; read_i++)
   {
      if (idx->entries[read_i].present)
      {
         idx->entries[read_i].present = false;
         if (write_i != read_i)
            idx->entries[write_i] = idx->entries[read_i];
         write_i++;
      }
      else
      {
         dp_entry_free(&idx->entries[read_i]);
         idx->dirty = true;
      }
   }
   idx->entries_count = write_i;
   /* Compaction shifted entries[] indices around; the hash table
    * holds stale offsets pointing at moved (or freed) slots.  A
    * full rebuild is the simplest correct recovery — entries_count
    * is the new total ROM count, well under hash-grow thresholds.
    * The only failure mode here is calloc OOM during a grow (rare;
    * the working set is on the order of kilobytes); on failure the
    * rebuild leaves a wiped ht so lookups answer NULL until create
    * eventually grows it again. */
   (void)dp_ht_rebuild(idx);
}

void downplay_index_set_art_state(downplay_index_t *idx,
      const char *basename, enum downplay_art_state state)
{
   dp_entry_t *e;
   if (!idx || !basename)
      return;
   e = dp_entries_find(idx, basename);
   if (e && e->art_state != state)
   {
      e->art_state = state;
      idx->dirty   = true;
   }
}

void downplay_index_flush(downplay_index_t *idx)
{
   if (!idx || !idx->dirty || !idx->json_path)
      return;
   if (dp_index_save_json(idx))
      idx->dirty = false;
}
