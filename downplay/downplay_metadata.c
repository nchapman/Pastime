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

/* Per-system metadata index for Downplay's launcher.  Owns the
 * libretrodb handle for one system's RDB, the on-disk JSON cache file,
 * and a small queue of files pending CRC/serial match.
 *
 * The matching logic mirrors RetroArch's own scanner (see
 * tasks/task_database.c:716 task_database_iterate_playlist) — every
 * helper we call is extern with a public header (tasks/task_database_cue.h).
 * The dispatcher itself is `static` upstream so we replicate its 30-odd-
 * line extension switch here, but the per-extension helpers do all the
 * actual file reading. */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <boolean.h>
#include <encodings/utf.h>
#include <file/archive_file.h>
#include <file/file_path.h>
#include <retro_assert.h>
#include <retro_miscellaneous.h>
#include <streams/file_stream.h>
#include <streams/interface_stream.h>
#include <string/stdstring.h>
#include <lists/string_list.h>
#include <formats/rjson.h>
#ifdef HAVE_THREADS
#include <rthreads/rthreads.h>
#endif

#include "downplay_metadata.h"
#include "downplay_defaults.h"

#include "../configuration.h"
#include "../defaults.h"
#include "../file_path_special.h"
#include "../gfx/gfx_thumbnail.h"
#include "../verbosity.h"
#include "../libretro-db/libretrodb.h"
#include "../libretro-db/rmsgpack_dom.h"
#include "../msg_hash.h"
#include "../tasks/task_database_cue.h"
#include "../tasks/task_file_transfer.h"

#include <net/net_http.h>

/* The disambiguation table + downplay_metadata_resolve_db_name live in
 * downplay_metadata_disambig.c so they're testable in isolation —
 * see downplay/tests/test_metadata_disambig.c for the unit tests. */

/* ---------- internal: per-extension match strategy ---------- */

/* Replicates the static function task_database_iterate_playlist
 * (tasks/task_database.c:716) — extension-switch dispatch.  Only the
 * dispatch is replicated; every helper called below is extern in
 * tasks/task_database_cue.h. */
enum dp_match_strategy
{
   DP_STRAT_SKIP = 0,
   DP_STRAT_CRC,            /* CRC32 of file (cart-likes) */
   DP_STRAT_CRC_ARCHIVE,    /* CRC of archive (zip/7z) */
   DP_STRAT_SERIAL_CUE,
   DP_STRAT_SERIAL_GDI,
   DP_STRAT_SERIAL_CHD,
   DP_STRAT_SERIAL_ISO      /* iso/wbfs/rvz/wia */
};

static enum dp_match_strategy dp_strategy_for_path(const char *path)
{
   const char *ext;
   char        ext_lower[8];

   if (!path || !*path)
      return DP_STRAT_SKIP;
   ext = path_get_extension(path);
   if (!ext || !*ext)
      return DP_STRAT_SKIP;
   strlcpy(ext_lower, ext, sizeof(ext_lower));
   string_to_lower(ext_lower);

   /* Mirror tasks/task_database.c:684 extension_to_file_type byte-for-
    * byte so we make the same matching choices as stock RA. */
   if (   string_is_equal(ext_lower, "zip")
       || string_is_equal(ext_lower, "7z")
       || string_is_equal(ext_lower, "apk")
       || string_is_equal(ext_lower, "zst"))
      return DP_STRAT_CRC_ARCHIVE;
   if (string_is_equal(ext_lower, "cue"))
      return DP_STRAT_SERIAL_CUE;
   if (string_is_equal(ext_lower, "gdi"))
      return DP_STRAT_SERIAL_GDI;
   if (string_is_equal(ext_lower, "chd"))
      return DP_STRAT_SERIAL_CHD;
   if (   string_is_equal(ext_lower, "iso")
       || string_is_equal(ext_lower, "wbfs")
       || string_is_equal(ext_lower, "rvz")
       || string_is_equal(ext_lower, "wia"))
      return DP_STRAT_SERIAL_ISO;
   return DP_STRAT_CRC;
}

/* Run the chosen strategy against `path`.  On success fills out_kind /
 * out_value and returns true.  out_value's content depends on kind:
 * for CRC, uppercase hex (caller normalises if needed); for serial,
 * the raw extracted string.  Falls back from serial→CRC for CUE/GDI/
 * CHD if serial extraction fails — same fallback policy as RA.
 *
 * out_archive_crc (may be NULL) is set to the *secondary* CRC for
 * archive content: the whole-archive CRC, so the caller can build the
 * dual-CRC `{crc:or(b"<inner>",b"<archive>")}` query that mirrors
 * RA's scanner (tasks/task_database.c:1199-1201).  Set to 0 when the
 * primary IS the archive CRC (inner-file extraction failed) or for
 * non-archive strategies. */
static bool dp_extract_match(const char *path,
      enum downplay_match_kind *out_kind,
      char *out_value, size_t out_value_len,
      uint32_t *out_archive_crc)
{
   uint32_t              crc        = 0;
   uint64_t              size       = 0;
   char                  serial[64];
   enum dp_match_strategy strat;

   if (!out_kind || !out_value || out_value_len == 0)
      return false;
   *out_kind     = DP_MATCH_NONE;
   *out_value    = '\0';
   serial[0]     = '\0';
   if (out_archive_crc)
      *out_archive_crc = 0;

   strat = dp_strategy_for_path(path);
   switch (strat)
   {
      case DP_STRAT_SKIP:
         return false;

      case DP_STRAT_CRC:
         /* Raw cart file — CRC the whole thing. */
         if (!intfstream_file_get_crc_and_size(path, 0, INT64_MAX,
                  &crc, &size))
            return false;
         snprintf(out_value, out_value_len, "%08X", (unsigned)crc);
         *out_kind = DP_MATCH_CRC;
         return true;

      case DP_STRAT_CRC_ARCHIVE:
         /* Zips/7z hold *one ROM each* in the No-Intro/Redump
          * conventions — the DAT files describe ROMs, not zips, so
          * the RDB primarily keys on the *inner file's* CRC.  But
          * libretro-database also has a small set of entries indexed
          * by *archive* CRC.  RA's own scanner builds a dual-CRC
          * `or(b"<inner>",b"<archive>")` query to cover both
          * (tasks/task_database.c:1199-1201); we mirror that here.
          *
          * Compute archive CRC unconditionally since archives are
          * generally small (the inner ROM is what's big after
          * decompression), then try inner-file CRC; if both succeed,
          * the caller emits the dual form.  If inner fails (corrupt
          * zip, unsupported member format), the archive CRC alone
          * becomes the primary. */
         {
            uint32_t inner   = file_archive_get_file_crc32_and_size(path, &size);
            uint32_t archive = 0;
            uint64_t archive_size = 0;
            if (intfstream_file_get_crc_and_size(path, 0, INT64_MAX,
                     &archive, &archive_size))
            {
               /* archive holds the whole-file CRC; size already
                * holds the inner-file size if `inner` succeeded,
                * otherwise it's untouched and we fall back to the
                * archive's size. */
               if (inner == 0)
                  size = archive_size;
            }
            if (inner != 0)
            {
               snprintf(out_value, out_value_len, "%08X", (unsigned)inner);
               if (out_archive_crc)
                  *out_archive_crc = archive;  /* may be 0 if the file
                                                * read failed; that's
                                                * fine — query builder
                                                * collapses to single
                                                * form. */
               *out_kind = DP_MATCH_CRC;
               return true;
            }
            if (archive != 0)
            {
               snprintf(out_value, out_value_len, "%08X", (unsigned)archive);
               /* Don't double-include the archive CRC as both primary
                * and secondary. */
               *out_kind = DP_MATCH_CRC;
               return true;
            }
            return false;
         }

      case DP_STRAT_SERIAL_CUE:
         if (task_database_cue_get_serial(path, serial, sizeof(serial), &size)
               && *serial)
         {
            strlcpy(out_value, serial, out_value_len);
            *out_kind = DP_MATCH_SERIAL;
            return true;
         }
         /* CRC fallback per task_database.c:744. */
         if (!task_database_cue_get_crc_and_size(path, &crc, &size))
            return false;
         snprintf(out_value, out_value_len, "%08X", (unsigned)crc);
         *out_kind = DP_MATCH_CRC;
         return true;

      case DP_STRAT_SERIAL_GDI:
         if (task_database_gdi_get_serial(path, serial, sizeof(serial), &size)
               && *serial)
         {
            strlcpy(out_value, serial, out_value_len);
            *out_kind = DP_MATCH_SERIAL;
            return true;
         }
         if (!task_database_gdi_get_crc_and_size(path, &crc, &size))
            return false;
         snprintf(out_value, out_value_len, "%08X", (unsigned)crc);
         *out_kind = DP_MATCH_CRC;
         return true;

      case DP_STRAT_SERIAL_CHD:
         if (task_database_chd_get_serial(path, serial, sizeof(serial), &size)
               && *serial)
         {
            strlcpy(out_value, serial, out_value_len);
            *out_kind = DP_MATCH_SERIAL;
            return true;
         }
         if (!task_database_chd_get_crc_and_size(path, &crc, &size))
            return false;
         snprintf(out_value, out_value_len, "%08X", (unsigned)crc);
         *out_kind = DP_MATCH_CRC;
         return true;

      case DP_STRAT_SERIAL_ISO:
         if (intfstream_file_get_serial(path, 0, INT64_MAX,
                  serial, sizeof(serial), &size) && *serial)
         {
            strlcpy(out_value, serial, out_value_len);
            *out_kind = DP_MATCH_SERIAL;
            return true;
         }
         return false;
   }

   return false;
}

/* ---------- internal: libretrodb wrapper ---------- */

/* Open a libretrodb_t* against rdb_path.  Returns NULL on failure
 * (file missing, format mismatch).  Holds the handle for the index's
 * lifetime — the per-query cost drops from ~50ms (cold open) to ~10ms
 * (cached open + linear-scan query). */
static libretrodb_t *dp_open_rdb(const char *rdb_path)
{
   libretrodb_t *db;
   if (!rdb_path || !*rdb_path)
      return NULL;
   if (!path_is_valid(rdb_path))
      return NULL;
   db = libretrodb_new();
   if (!db)
      return NULL;
   if (libretrodb_open(rdb_path, db, false) != 0)
   {
      libretrodb_free(db);
      return NULL;
   }
   return db;
}

static void dp_close_rdb(libretrodb_t *db)
{
   if (!db)
      return;
   libretrodb_close(db);
   libretrodb_free(db);
}

/* Run a query against the open RDB.  On hit, copies the matched
 * record's `name` field into out_label and returns true.  Mirrors the
 * cursor + DOM walk pattern at database_info.c:558-637. */
static bool dp_run_query(libretrodb_t *db, const char *query,
      char *out_label, size_t out_label_len)
{
   libretrodb_cursor_t       *cursor;
   libretrodb_query_t        *q;
   const char                *err = NULL;
   struct rmsgpack_dom_value  item;
   size_t                     i;
   bool                       found = false;

   if (!db || !query || !*query || !out_label || out_label_len == 0)
      return false;
   *out_label = '\0';

   q = (libretrodb_query_t*)libretrodb_query_compile(db, query,
         strlen(query), &err);
   if (err || !q)
   {
      if (q)
         libretrodb_query_free(q);
      return false;
   }

   cursor = libretrodb_cursor_new();
   if (!cursor)
   {
      libretrodb_query_free(q);
      return false;
   }
   if (libretrodb_cursor_open(db, cursor, q) != 0)
   {
      libretrodb_cursor_free(cursor);
      libretrodb_query_free(q);
      return false;
   }

   /* First match wins — RDB entries are unique-by-CRC/serial in the
    * libretro-database publishing rules. */
   if (libretrodb_cursor_read_item(cursor, &item) == 0)
   {
      if (item.type == RDT_MAP)
      {
         for (i = 0; i < item.val.map.len; i++)
         {
            const struct rmsgpack_dom_value *key   = &item.val.map.items[i].key;
            const struct rmsgpack_dom_value *value = &item.val.map.items[i].value;
            if (!key || !value || key->type != RDT_STRING)
               continue;
            if (key->val.string.len == 4
                  && memcmp(key->val.string.buff, "name", 4) == 0
                  && value->type == RDT_STRING
                  && value->val.string.buff
                  && *value->val.string.buff)
            {
               strlcpy(out_label, value->val.string.buff, out_label_len);
               found = true;
               break;
            }
         }
      }
      rmsgpack_dom_value_free(&item);
   }

   libretrodb_cursor_close(cursor);
   libretrodb_cursor_free(cursor);
   libretrodb_query_free(q);
   return found;
}

/* Build query string verbatim from RA (tasks/task_database.c:1199-1201
 * and :1374-1378).  The byte-level wire format must match for the
 * cursor's inline field-evaluator to fast-path the same way. */
static bool dp_build_crc_query(const char *crc_hex, uint32_t archive_crc,
      char *out, size_t out_len)
{
   /* Mirrors RA's `{crc:or(b"<a>",b"<b>")}` dual-CRC form
    * (tasks/task_database.c:1199-1201) when the caller supplies an
    * archive CRC alongside the inner-file CRC.  When archive_crc is 0
    * (cart-likes, or the inner-file CRC failed and the archive CRC is
    * the primary), emit the single-CRC short form. */
   if (!crc_hex || !*crc_hex)
      return false;
   if (archive_crc == 0)
      snprintf(out, out_len, "{crc:b\"%s\"}", crc_hex);
   else
      snprintf(out, out_len, "{crc:or(b\"%s\",b\"%08X\")}",
            crc_hex, (unsigned)archive_crc);
   return true;
}

static bool dp_build_serial_query(const char *serial, char *out, size_t out_len)
{
   /* RA serializes via bin_to_hex_alloc (task_database.c:1367-1378):
    *   {'serial': b'<hex-of-ASCII-serial>'}
    * We reproduce that exactly: hex-encode the serial string's bytes. */
   size_t len;
   size_t i;
   size_t pos;

   if (!serial || !*serial)
      return false;
   len = strlen(serial);
   if (out_len < 16 + len * 2)
      return false;
   pos = (size_t)snprintf(out, out_len, "{'serial': b'");
   for (i = 0; i < len; i++)
   {
      if (pos + 2 >= out_len)
         return false;
      pos += (size_t)snprintf(out + pos, out_len - pos,
            "%02X", (unsigned)(unsigned char)serial[i]);
   }
   if (pos + 3 >= out_len)
      return false;
   out[pos++] = '\'';
   out[pos++] = '}';
   out[pos]   = '\0';
   return true;
}

/* ---------- internal: entry + queue + result storage ----------
 *
 * Threading model (see THREADING_REFACTOR.md for the full design):
 *
 *   Main thread owns: json_path, system_folder, dirty, plus all reads
 *   and writes of dp_entry_t fields.  Worker thread NEVER touches
 *   entries[] — it operates on snapshotted dp_work_t / dp_result_t
 *   structures by value.
 *
 *   Shared (mutex-protected): entries[] (the array itself, against
 *   realloc; field reads outside the lock are main-thread-only and
 *   therefore race-free), queue[], results[], shutdown.
 *
 *   Worker-only after sthread_create publishes the worker (read-only
 *   from there): db, system_root, db_name.  No lock needed because
 *   thread create/join are full memory barriers (POSIX guarantee).
 *
 *   `lookup` and `set_art_state` take the lock briefly to walk
 *   entries[] — that's the only contention with `note_present` /
 *   `finish_scan` / `pump` (all main-thread, so the contention is
 *   really with the worker's queue/results pushes happening
 *   underneath, which is also serialised). */

/* One entry in the index.  basename is the dictionary key (heap-owned).
 * mtime + size are the validity stamps; mismatch invalidates everything
 * else and re-queues for matching.  art_checked_at is reserved for a
 * future "refresh missing art every N days" feature. */
typedef struct
{
   char                    *basename;     /* owned */
   char                    *label;        /* owned, may be NULL */
   char                    *match_value;  /* owned, may be NULL */
   int64_t                  mtime;
   int64_t                  size;
   int64_t                  art_checked_at;
   enum downplay_match_kind match_kind;
   enum downplay_art_state  art_state;
   bool                     present;      /* set by note_present, cleared at start of scan */
   bool                     pending;      /* in the work queue */
} dp_entry_t;

/* Work item passed from main thread to worker.  POD by design — no heap
 * pointers — so copying across threads under the queue mutex is safe
 * with no separate lifetime concerns.  The (mtime, size) stamp is
 * carried alongside basename so the apply path can validate that the
 * file hasn't been replaced under the same name during the worker's
 * (potentially seconds-long) processing window. */
typedef struct
{
   char    basename[NAME_MAX_LENGTH];
   int64_t mtime;
   int64_t size;
} dp_work_t;

/* Result pushed back from worker to main thread.  POD; same lifetime
 * rationale as dp_work_t.  When kind == DP_MATCH_NONE the file's
 * extraction failed (unreadable, unsupported); when label[0] == '\0'
 * the extraction worked but the libretrodb query missed (record what
 * we tried so we don't re-fingerprint on every visit).  The
 * basename + (mtime, size) tuple is the validity key for apply. */
typedef struct
{
   char                      basename[NAME_MAX_LENGTH];
   int64_t                   mtime;
   int64_t                   size;
   enum downplay_match_kind  kind;
   char                      match_value[64];
   char                      label[NAME_MAX_LENGTH];
} dp_result_t;

/* Sanity cap on the work queue.  With per-entry pending-flag dedup, the
 * queue can never exceed entries_count, so this is just a runaway-bug
 * canary (we'd notice 10k SNES ROMs in one folder long before this fires
 * organically). */
#define DP_QUEUE_CAP_LIMIT 10000

struct downplay_index
{
   /* === main-thread-only === */
   char         *json_path;     /* <RA_config>/downplay/index/<system>.json */
   char         *system_folder;
   bool          dirty;

   /* === shared (mutex-protected) ===
    * Mutate or grow only under idx->mutex.  Reads of dp_entry_t fields
    * outside the array spine (label/match_value/match_kind/art_state)
    * are main-thread-only and therefore unlocked. */
   dp_entry_t   *entries;       size_t entries_count, entries_cap;
   dp_work_t    *queue;         size_t queue_count, queue_cap, queue_head;
   dp_result_t  *results;       size_t results_count, results_cap, results_head;

   /* === worker-only after sthread_create publishes the worker ===
    * Initialised on main thread before sthread_create (full barrier).
    * Closed on main thread after sthread_join (full barrier).  Read-
    * only by the worker.  No lock required. */
   libretrodb_t *db;
   char         *system_root;   /* absolute path of the ROM folder */
   char         *db_name;

   /* === lifecycle === */
#ifdef HAVE_THREADS
   slock_t      *mutex;
   scond_t      *cond;
   sthread_t    *worker;        /* NULL when threads disabled OR spawn failed */
   bool          shutdown;
#endif
};

#ifdef HAVE_THREADS
#define DP_LOCK(idx)   slock_lock((idx)->mutex)
#define DP_UNLOCK(idx) slock_unlock((idx)->mutex)
/* Debug guard: any function that mutates main-thread-owned state must
 * assert it's not running on the worker.  No-op in release builds. */
#define DP_ASSERT_MAIN_THREAD(idx) do { \
   if ((idx)->worker) \
      retro_assert(!sthread_isself((idx)->worker)); \
} while (0)
#else
#define DP_LOCK(idx)   ((void)0)
#define DP_UNLOCK(idx) ((void)0)
#define DP_ASSERT_MAIN_THREAD(idx) ((void)0)
#endif

static dp_entry_t *dp_entries_find(downplay_index_t *idx, const char *basename)
{
   size_t i;
   if (!idx || !basename)
      return NULL;
   for (i = 0; i < idx->entries_count; i++)
   {
      if (string_is_equal(idx->entries[i].basename, basename))
         return &idx->entries[i];
   }
   return NULL;
}

static dp_entry_t *dp_entries_create(downplay_index_t *idx, const char *basename)
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
   e = &idx->entries[idx->entries_count];
   memset(e, 0, sizeof(*e));
   e->basename = strdup(basename);
   if (!e->basename)
      return NULL;
   idx->entries_count++;
   return e;
}

static void dp_entry_free_strings(dp_entry_t *e)
{
   if (!e)
      return;
   free(e->basename);
   free(e->label);
   free(e->match_value);
   e->basename    = NULL;
   e->label       = NULL;
   e->match_value = NULL;
}

static void dp_entry_clear_match(dp_entry_t *e)
{
   if (!e)
      return;
   free(e->label);
   free(e->match_value);
   e->label       = NULL;
   e->match_value = NULL;
   e->match_kind  = DP_MATCH_NONE;
}

/* Push a work item.  Caller holds idx->mutex.  Returns false if the
 * queue is at the sanity cap or realloc fails (caller leaves
 * entry->pending unchanged so a future scan can retry). */
static bool dp_queue_push_locked(downplay_index_t *idx,
      const dp_work_t *work)
{
   size_t tail;
   if (idx->queue_count >= DP_QUEUE_CAP_LIMIT)
   {
      RARCH_WARN("[Downplay] index queue cap (%zu) reached; dropping %s\n",
            (size_t)DP_QUEUE_CAP_LIMIT, work->basename);
      return false;
   }
   tail = idx->queue_head + idx->queue_count;
   if (tail >= idx->queue_cap)
   {
      /* No space at tail.  First try compacting the head; that's
       * cheaper than realloc and reclaims any space the worker has
       * already drained. */
      if (idx->queue_head > 0)
      {
         memmove(idx->queue, idx->queue + idx->queue_head,
               idx->queue_count * sizeof(*idx->queue));
         idx->queue_head = 0;
         tail            = idx->queue_count;
      }
      if (tail >= idx->queue_cap)
      {
         size_t     new_cap = idx->queue_cap ? idx->queue_cap * 2 : 16;
         dp_work_t *grown   = (dp_work_t*)realloc(idx->queue,
               new_cap * sizeof(*grown));
         if (!grown)
            return false;
         idx->queue     = grown;
         idx->queue_cap = new_cap;
      }
   }
   idx->queue[tail] = *work;
   idx->queue_count++;
   return true;
}

/* Pop the head work item.  Caller holds idx->mutex.  FIFO so background
 * enrichment progresses in scan order (alphabetical), which matches
 * what the user is most likely scrolling through. */
static bool dp_queue_pop_locked(downplay_index_t *idx, dp_work_t *out)
{
   if (idx->queue_count == 0)
      return false;
   *out = idx->queue[idx->queue_head];
   idx->queue_head++;
   idx->queue_count--;
   /* Lazy compact: when we've consumed more than half the buffer's
    * capacity from the front, slide remaining items down so the next
    * push has room without an alloc.  This avoids the O(N²) cost of
    * memmove-on-every-pop while keeping the buffer bounded. */
   if (idx->queue_head > 0 && idx->queue_head > idx->queue_cap / 2)
   {
      if (idx->queue_count > 0)
         memmove(idx->queue, idx->queue + idx->queue_head,
               idx->queue_count * sizeof(*idx->queue));
      idx->queue_head = 0;
   }
   return true;
}

/* Build a snapshot from `e` and push it.  Caller holds idx->mutex.
 * No-op if the entry is already in the queue (pending flag).  Sets
 * pending on success so subsequent calls dedup. */
static void dp_enqueue_match_locked(downplay_index_t *idx, dp_entry_t *e)
{
   dp_work_t work;
   if (!e || e->pending || !e->basename)
      return;
   memset(&work, 0, sizeof(work));
   strlcpy(work.basename, e->basename, sizeof(work.basename));
   work.mtime = e->mtime;
   work.size  = e->size;
   if (dp_queue_push_locked(idx, &work))
      e->pending = true;
}

/* ---------- internal: match-one (worker body) ---------- */

/* Run the full match pipeline on one work snapshot.  Pure: takes a
 * snapshot in, fills a result out, never touches entries[].  Called
 * from the worker loop AND from the synchronous fallback pump (when
 * HAVE_THREADS=0 or worker spawn failed).  No locking required —
 * the only shared state read is idx->db / idx->system_root, both
 * worker-only-after-publish (or main-thread-only when no worker
 * exists). */
static void dp_match_one(downplay_index_t *idx,
      const dp_work_t *work, dp_result_t *out)
{
   char                     full_path[PATH_MAX_LENGTH];
   char                     match_val[64];
   char                     query[256];
   char                     label[NAME_MAX_LENGTH];
   enum downplay_match_kind kind;
   uint32_t                 archive_crc = 0;

   memset(out, 0, sizeof(*out));
   strlcpy(out->basename, work->basename, sizeof(out->basename));
   out->mtime = work->mtime;
   out->size  = work->size;

   fill_pathname_join_special(full_path, idx->system_root, work->basename,
         sizeof(full_path));
   if (!path_is_valid(full_path))
      return;

   if (!dp_extract_match(full_path, &kind, match_val, sizeof(match_val),
            &archive_crc))
      return;
   if (kind == DP_MATCH_NONE)
      return;

   if (kind == DP_MATCH_CRC)
   {
      if (!dp_build_crc_query(match_val, archive_crc, query, sizeof(query)))
         return;
   }
   else
   {
      if (!dp_build_serial_query(match_val, query, sizeof(query)))
         return;
   }

   /* Successful extraction is itself useful provenance even when the
    * RDB query misses — record what we tried so a future visit doesn't
    * re-CRC the file. */
   out->kind = kind;
   strlcpy(out->match_value, match_val, sizeof(out->match_value));
   if (dp_run_query(idx->db, query, label, sizeof(label)))
      strlcpy(out->label, label, sizeof(out->label));
}

/* Apply a result to the entries[] table.  Main-thread only.  Validates
 * that the entry still exists with the same (mtime, size) — if the
 * file was replaced or removed during the worker's processing window,
 * discard the result silently and a future scan will re-enqueue. */
static void dp_apply_result(downplay_index_t *idx, const dp_result_t *r)
{
   dp_entry_t *e;
   bool        applied = false;

   DP_ASSERT_MAIN_THREAD(idx);
   DP_LOCK(idx);
   e = dp_entries_find(idx, r->basename);
   if (!e)
   {
      DP_UNLOCK(idx);
      return; /* entry pruned by finish_scan; orphan result */
   }
   if (e->mtime != r->mtime || e->size != r->size)
   {
      /* This result is for an old snapshot — the file was replaced
       * after we enqueued.  Discard it.  Don't touch `pending`: a new
       * snapshot may already be in the queue (note_present resets
       * pending and re-enqueues on stale detection), and clearing it
       * here would let a subsequent note_present enqueue a duplicate. */
      DP_UNLOCK(idx);
      return;
   }

   if (r->kind != DP_MATCH_NONE)
   {
      e->match_kind = r->kind;
      free(e->match_value);
      e->match_value = strdup(r->match_value);
      applied        = true;
   }
   if (*r->label)
   {
      free(e->label);
      e->label = strdup(r->label);
      RARCH_LOG("[Downplay] match hit: %s -> \"%s\" (%s=%s)\n",
            e->basename, e->label,
            r->kind == DP_MATCH_CRC ? "crc" : "serial", r->match_value);
   }
   else if (r->kind != DP_MATCH_NONE)
      RARCH_LOG("[Downplay] match miss: %s (%s=%s)\n",
            e->basename,
            r->kind == DP_MATCH_CRC ? "crc" : "serial", r->match_value);

   e->pending = false;
   DP_UNLOCK(idx);

   /* `dirty` is main-thread-only (see struct ownership comment); set
    * after unlock so the read in `flush`'s early-return guard doesn't
    * race a write inside someone else's locked region. */
   if (applied)
      idx->dirty = true;
}

#ifdef HAVE_THREADS
/* Push a result.  Caller holds idx->mutex.  On realloc failure we drop
 * the result and clear the entry's `pending` flag so a future
 * note_present can re-enqueue — without that step the entry would be
 * stuck pending forever (dp_enqueue_match_locked short-circuits on
 * pending=true, and note_present only force-clears it on (mtime, size)
 * mismatch).
 *
 * Mirrors dp_queue_push/pop_locked's head-cursor lazy-compact so the
 * results buffer doesn't degenerate to O(N²) when the worker is
 * producing faster than the pump drains. */
static void dp_results_push_locked(downplay_index_t *idx,
      const dp_result_t *r)
{
   size_t tail = idx->results_head + idx->results_count;
   if (tail >= idx->results_cap)
   {
      if (idx->results_head > 0)
      {
         memmove(idx->results, idx->results + idx->results_head,
               idx->results_count * sizeof(*idx->results));
         idx->results_head = 0;
         tail              = idx->results_count;
      }
      if (tail >= idx->results_cap)
      {
         size_t       new_cap = idx->results_cap ? idx->results_cap * 2 : 16;
         dp_result_t *grown   = (dp_result_t*)realloc(idx->results,
               new_cap * sizeof(*grown));
         if (!grown)
         {
            dp_entry_t *e = dp_entries_find(idx, r->basename);
            if (e && e->mtime == r->mtime && e->size == r->size)
               e->pending = false;
            return;
         }
         idx->results     = grown;
         idx->results_cap = new_cap;
      }
   }
   idx->results[tail] = *r;
   idx->results_count++;
}

/* Worker thread entry point.  Loops on the queue until shutdown is
 * signalled.  All heavy work (CRC32 of multi-MB ROMs, libretrodb
 * cursor scans) happens OUTSIDE the lock so the main thread is never
 * blocked on a per-frame pump call.
 *
 * Cancellation latency: an in-flight CRC of a very large ROM (~700MB
 * ISO would cart-likes route to CRC; in practice flat-binary homebrew
 * is the worst case) takes seconds.  The shutdown flag is checked at
 * loop entry, not mid-CRC, so close() can block for the duration of
 * one in-flight match.  See THREADING_REFACTOR.md "Non-goals" for the
 * rationale on not threading cancellation through the intfstream
 * helpers. */
static void dp_worker_loop(void *data)
{
   downplay_index_t *idx = (downplay_index_t*)data;

   for (;;)
   {
      dp_work_t   work;
      dp_result_t result;

      slock_lock(idx->mutex);
      while (!idx->shutdown && idx->queue_count == 0)
         scond_wait(idx->cond, idx->mutex);
      if (idx->shutdown)
      {
         slock_unlock(idx->mutex);
         break;
      }
      /* Predicate above guarantees queue_count > 0, so pop never fails. */
      dp_queue_pop_locked(idx, &work);
      slock_unlock(idx->mutex);

      dp_match_one(idx, &work, &result);

      slock_lock(idx->mutex);
      /* Push regardless of shutdown.  close() doesn't drain results
       * before sthread_join — the entry is about to be freed — so a
       * push here that lands during shutdown is silently dropped on
       * the subsequent free(idx->results).  Pushing under the lock
       * (rather than checking shutdown first) keeps the pre-shutdown
       * path simple at the cost of one wasted memcpy on the very last
       * result during a close. */
      dp_results_push_locked(idx, &result);
      slock_unlock(idx->mutex);
   }
}
#endif

/* ---------- internal: JSON ---------- */

/* Bump on incompatible schema changes; also bump when fixing matching
 * logic so stale-but-syntactically-valid caches get rebuilt rather than
 * loaded with bad data.
 *
 *   v1 → v2 (May 2026): switched archive CRC strategy from "CRC of the
 *     zip itself" to "CRC of the inner ROM" via
 *     file_archive_get_file_crc32_and_size.  v1 caches contained the
 *     wrong (archive) CRC, so every label resolved to NULL and every
 *     `art` slot was sentinel-stuck at "missing".  v2 forces a full
 *     re-match on first load. */
#define DP_INDEX_SCHEMA_VERSION 2

static const char *dp_match_kind_to_str(enum downplay_match_kind k)
{
   switch (k)
   {
      case DP_MATCH_CRC:    return "crc";
      case DP_MATCH_SERIAL: return "serial";
      case DP_MATCH_NONE:   break;
   }
   return "none";
}

static enum downplay_match_kind dp_match_kind_from_str(const char *s)
{
   if (s)
   {
      if (string_is_equal(s, "crc"))    return DP_MATCH_CRC;
      if (string_is_equal(s, "serial")) return DP_MATCH_SERIAL;
   }
   return DP_MATCH_NONE;
}

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

/* SAX-style parse context.  The index file has fixed shape:
 *   { version: <int>, db_name: <string>, entries: { <basename>: { ... } } }
 * The walker tracks where we are via a small enum stack. */
enum dp_parse_state
{
   DP_PS_ROOT = 0,        /* awaiting root object */
   DP_PS_ROOT_KEY,        /* inside root, awaiting member name */
   DP_PS_ROOT_VAL,        /* inside root, awaiting value of last_key */
   DP_PS_ENTRIES_KEY,     /* inside entries{}, awaiting basename key */
   DP_PS_ENTRIES_VAL,     /* inside an entry-value object */
   DP_PS_ENTRY_KEY,       /* inside entry obj, awaiting field name */
   DP_PS_ENTRY_VAL        /* inside entry obj, awaiting value */
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
         /* entries: { ... } */
         if (string_is_equal(ctx->last_key, "entries"))
            ctx->state = DP_PS_ENTRIES_KEY;
         else
            return false; /* unknown nested object at root */
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
         ctx->state = DP_PS_ROOT; /* done */
         break;
      case DP_PS_ENTRIES_KEY:
         ctx->state = DP_PS_ROOT_KEY;
         break;
      case DP_PS_ENTRY_KEY:
         /* Done with this entry; entries that loaded with NONE/UNKNOWN
          * should also be enqueued so a partial cache resumes matching
          * after restart.  But only if we have enough info to validate
          * later (mtime/size ok). */
         if (ctx->cur_entry && ctx->cur_entry->match_kind == DP_MATCH_NONE
               && ctx->cur_entry->basename
               && ctx->cur_entry->mtime > 0)
         {
            /* Defer enqueue to note_present() — it'll reconcile against
             * the current filesystem state and only enqueue if the file
             * still exists.  Loading the JSON alone never triggers I/O. */
         }
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
         /* Create entry stub.  If alloc fails we silently skip the row. */
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
      if (ctx->cur_entry)
      {
         if (string_is_equal(ctx->last_key, "label"))
         {
            free(ctx->cur_entry->label);
            ctx->cur_entry->label = strdup(str);
         }
         else if (string_is_equal(ctx->last_key, "match_kind"))
            ctx->cur_entry->match_kind = dp_match_kind_from_str(str);
         else if (string_is_equal(ctx->last_key, "match_value"))
         {
            free(ctx->cur_entry->match_value);
            ctx->cur_entry->match_value = strdup(str);
         }
         else if (string_is_equal(ctx->last_key, "art"))
            ctx->cur_entry->art_state = dp_art_state_from_str(str);
      }
      ctx->state = DP_PS_ENTRY_KEY;
      return true;
   }
   return false;
}

static bool dp_parse_number(void *user, const char *str, size_t len)
{
   dp_parse_ctx_t *ctx = (dp_parse_ctx_t*)user;
   long long       n   = 0;
   (void)len;
   /* atoll on a bounded string — rjson zero-terminates the buffer it
    * hands us.  Negative or out-of-range values clamp to 0 because
    * mtime/size of 0 is treated as "unknown" everywhere else. */
   n = atoll(str);

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
      else if (string_is_equal(ctx->last_key, "art_checked_at"))
         ctx->cur_entry->art_checked_at = (int64_t)n;
      ctx->state = DP_PS_ENTRY_KEY;
      return true;
   }
   return false;
}

static bool dp_parse_null(void *user)
{
   dp_parse_ctx_t *ctx = (dp_parse_ctx_t*)user;
   /* We accept null for label / match_value (unmatched entries). */
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
   intfstream_t  *file;
   rjson_t       *json;
   dp_parse_ctx_t ctx;
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
         NULL, /* start_array — schema has none */
         NULL, /* end_array   — schema has none */
         NULL, /* boolean     — schema has none */
         dp_parse_null);

   rjson_free(json);
   intfstream_close(file);
   free(file);

   if (result != RJSON_DONE)
   {
      /* Corrupt or unrecognized; drop in-memory state and start fresh.
       * Atomic write on next flush will replace the bad file. */
      RARCH_WARN("[Downplay] index parse failed for %s; rebuilding\n",
            idx->json_path);
      return false;
   }
   if (ctx.file_version != DP_INDEX_SCHEMA_VERSION)
   {
      /* Older schema — discard in-memory state so the matching pass
       * reruns with the current logic.  See the version comment above
       * for rationale on each bump. */
      RARCH_LOG("[Downplay] index %s is schema v%d (current v%d); "
            "rebuilding\n",
            idx->json_path, ctx.file_version, DP_INDEX_SCHEMA_VERSION);
      return false;
   }
   return true;
}

static void dp_write_int64(rjsonwriter_t *w, int64_t v)
{
   /* rjsonwriter has no int64 path (only double); roundtripping mtime
    * through double is fine for values < 2^53 (we're nowhere close). */
   rjsonwriter_add_double(w, (double)v);
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

   /* Atomic write: emit to <path>.tmp, then rename(2).  rename is atomic
    * on a single filesystem — a crash mid-write leaves the previous
    * good copy intact. */
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
      rjsonwriter_add_string(writer, "match_kind");
      rjsonwriter_raw(writer, ": ", 2);
      rjsonwriter_add_string(writer, dp_match_kind_to_str(e->match_kind));
      rjsonwriter_raw(writer, ",\n", 2);

      rjsonwriter_add_spaces(writer, 6);
      rjsonwriter_add_string(writer, "match_value");
      rjsonwriter_raw(writer, ": ", 2);
      if (e->match_value)
         rjsonwriter_add_string(writer, e->match_value);
      else
         rjsonwriter_raw(writer, "null", 4);
      rjsonwriter_raw(writer, ",\n", 2);

      rjsonwriter_add_spaces(writer, 6);
      rjsonwriter_add_string(writer, "label");
      rjsonwriter_raw(writer, ": ", 2);
      if (e->label)
         rjsonwriter_add_string(writer, e->label);
      else
         rjsonwriter_raw(writer, "null", 4);
      rjsonwriter_raw(writer, ",\n", 2);

      rjsonwriter_add_spaces(writer, 6);
      rjsonwriter_add_string(writer, "art");
      rjsonwriter_raw(writer, ": ", 2);
      rjsonwriter_add_string(writer, dp_art_state_to_str(e->art_state));
      rjsonwriter_raw(writer, ",\n", 2);

      rjsonwriter_add_spaces(writer, 6);
      rjsonwriter_add_string(writer, "art_checked_at");
      rjsonwriter_raw(writer, ": ", 2);
      dp_write_int64(writer, e->art_checked_at);
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

   /* Atomically replace.  POSIX rename overwrites the destination on
    * the same filesystem; on Windows we'd need ReplaceFile, but
    * downplay's targets are POSIX-only today. */
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

   if (!system_folder_name || !*system_folder_name)
      return NULL;
   if (!system_root_path || !*system_root_path)
      return NULL;
   if (!downplay_paths_get_index_root(index_root, sizeof(index_root)))
      return NULL;

   /* Filename: "<system_folder>.json".  No path-sanitization here
    * because system_folder_name is already a real on-disk directory
    * name (filesystem already vetted it).  Any slash characters would
    * have been impossible. */
   snprintf(file_name, sizeof(file_name), "%s.json", system_folder_name);
   fill_pathname_join_special(json_path, index_root, file_name,
         sizeof(json_path));

   idx = (downplay_index_t*)calloc(1, sizeof(*idx));
   if (!idx)
      return NULL;
   idx->json_path     = strdup(json_path);
   idx->system_folder = strdup(system_folder_name);
   idx->system_root   = strdup(system_root_path);
   if (db_name && *db_name)
      idx->db_name    = strdup(db_name);
   if (!idx->json_path || !idx->system_folder || !idx->system_root)
   {
      downplay_index_close(idx);
      return NULL;
   }

   /* Best-effort load.  Failure → start with empty in-memory state and
    * the bad on-disk file gets atomically replaced on next flush. */
   if (!dp_index_load_json(idx))
   {
      size_t i;
      for (i = 0; i < idx->entries_count; i++)
         dp_entry_free_strings(&idx->entries[i]);
      idx->entries_count = 0;
   }

   /* Open the RDB lazily — do it now so subsequent pump ops are cheap.
    * directory_database is a settings_t->paths field but we keep this
    * module free of configuration.h by accepting a pre-resolved
    * db_name and looking up its rdb path through core_info if the
    * direct settings path isn't available.  Simplest: g_defaults exposes
    * DEFAULT_DIR_DATABASE which holds the rdb path. */
   if (idx->db_name && *idx->db_name)
   {
      char rdb_path[PATH_MAX_LENGTH];
      const char *db_dir = g_defaults.dirs[DEFAULT_DIR_DATABASE];
      if (db_dir && *db_dir)
      {
         char db_file[NAME_MAX_LENGTH];
         /* libretro-database stores the RDBs under a `rdb/` subdir.
          * Stock RA's directory_database setting points one level above
          * (the parent of `rdb/`); but DEFAULT_DIR_DATABASE varies by
          * platform — sometimes it's the rdb/ dir, sometimes its parent.
          * Try both. */
         snprintf(db_file, sizeof(db_file), "%s.rdb", idx->db_name);
         fill_pathname_join_special(rdb_path, db_dir, db_file,
               sizeof(rdb_path));
         if (!path_is_valid(rdb_path))
         {
            char nested[PATH_MAX_LENGTH];
            fill_pathname_join_special(nested, db_dir, "rdb",
                  sizeof(nested));
            fill_pathname_join_special(rdb_path, nested, db_file,
                  sizeof(rdb_path));
         }
         idx->db = dp_open_rdb(rdb_path);
         if (!idx->db)
            RARCH_WARN("[Downplay] index: rdb not found for %s "
                  "(tried %s); matching disabled\n",
                  idx->db_name, rdb_path);
         else
            RARCH_LOG("[Downplay] index: opened rdb %s\n", rdb_path);
      }
   }

#ifdef HAVE_THREADS
   /* Thread primitives + worker.  Spawn only when matching is actually
    * available (db opened) — otherwise the worker would idle forever.
    * sthread_create is a full memory barrier (POSIX), so all the
    * worker-zone fields above are safely visible to the worker on its
    * first instruction. */
   idx->mutex = slock_new();
   idx->cond  = scond_new();
   if (idx->db && idx->mutex && idx->cond)
   {
      idx->worker = sthread_create(dp_worker_loop, idx);
      if (!idx->worker)
         RARCH_WARN("[Downplay] index: worker spawn failed; "
               "falling back to inline matching\n");
   }
#endif

   return idx;
}

void downplay_index_close(downplay_index_t *idx)
{
   size_t i;
   if (!idx)
      return;
   DP_ASSERT_MAIN_THREAD(idx);

#ifdef HAVE_THREADS
   /* Shutdown sequence per THREADING_REFACTOR.md "Important: shutdown
    * without a final result drain":
    *   1. Set shutdown under lock and signal so a waiting worker
    *      observes both atomically.
    *   2. Join — may block for up to one large-file CRC.
    * After join: flush dirty entries, free entries[] / queue / results
    * (any final result pushed by the worker before observing shutdown
    * is silently dropped — entries[] is about to be torn down anyway,
    * applying it would be wasted work). */
   if (idx->worker)
   {
      slock_lock(idx->mutex);
      idx->shutdown = true;
      scond_signal(idx->cond);
      slock_unlock(idx->mutex);
      sthread_join(idx->worker);
      idx->worker = NULL;
   }
#endif

   downplay_index_flush(idx);
   dp_close_rdb(idx->db);
   for (i = 0; i < idx->entries_count; i++)
      dp_entry_free_strings(&idx->entries[i]);
   free(idx->entries);
   free(idx->queue);
   free(idx->results);

#ifdef HAVE_THREADS
   if (idx->mutex)
      slock_free(idx->mutex);
   if (idx->cond)
      scond_free(idx->cond);
#endif

   free(idx->json_path);
   free(idx->system_folder);
   free(idx->system_root);
   free(idx->db_name);
   free(idx);
}

bool downplay_index_lookup(downplay_index_t *idx,
      const char *basename, time_t mtime, int64_t size,
      downplay_index_record_t *out)
{
   dp_entry_t *e;
   bool        found = false;
   if (!out)
      return false;
   memset(out, 0, sizeof(*out));
   if (!idx || !basename)
      return false;

   /* Lock briefly to walk entries[] safely against a concurrent realloc
    * by note_present.  The string pointers we hand back are owned by
    * the entry and only freed by main-thread paths (note_present's
    * stale invalidation, apply_result's replace).  Caller MUST consume
    * the pointers within the same frame — see the contract on
    * downplay_index_record_t in the header.  Storing across a
    * subsequent pump or note_present is a use-after-free hazard. */
   DP_LOCK(idx);
   e = dp_entries_find(idx, basename);
   if (e && e->mtime == (int64_t)mtime && e->size == size)
   {
      out->label       = e->label;
      out->match_kind  = e->match_kind;
      out->match_value = e->match_value;
      out->art_state   = e->art_state;
      found = true;
   }
   DP_UNLOCK(idx);
   return found;
}

void downplay_index_note_present(downplay_index_t *idx,
      const char *basename, time_t mtime, int64_t size)
{
   dp_entry_t *e;
   bool        is_new      = false;
   bool        wake_worker = false;

   if (!idx || !basename || !*basename)
      return;
   DP_ASSERT_MAIN_THREAD(idx);

   DP_LOCK(idx);
   e = dp_entries_find(idx, basename);
   if (!e)
   {
      e = dp_entries_create(idx, basename);
      if (!e)
      {
         DP_UNLOCK(idx);
         return;
      }
      is_new = true;
   }
   e->present = true;

   if (is_new || e->mtime != (int64_t)mtime || e->size != size)
   {
      e->mtime = (int64_t)mtime;
      e->size  = size;
      /* Stale or new — invalidate match and art state, requeue.  Note
       * that pending may be true here from a prior queue snapshot for
       * the old (mtime, size); apply_result will discard that result
       * via its validity check.  Force-clear pending so we re-enqueue
       * with the new stamp. */
      dp_entry_clear_match(e);
      e->art_state      = DP_ART_UNKNOWN;
      e->art_checked_at = 0;
      e->pending        = false;
      idx->dirty        = true;
      if (idx->db)
      {
         dp_enqueue_match_locked(idx, e);
         wake_worker = true;
      }
   }
   else if (e->match_kind == DP_MATCH_NONE && idx->db)
   {
      /* Unmatched entry from a prior session — retry now we're up. */
      dp_enqueue_match_locked(idx, e);
      wake_worker = true;
   }

#ifdef HAVE_THREADS
   /* Signal under the lock per THREADING_REFACTOR.md "Lock discipline":
    * the simpler model with no lost-wakeup edge cases.  scond_signal
    * is cheap. */
   if (wake_worker && idx->cond && idx->worker)
      scond_signal(idx->cond);
#else
   (void)wake_worker;
#endif
   DP_UNLOCK(idx);
}

void downplay_index_finish_scan(downplay_index_t *idx)
{
   size_t read_i;
   size_t write_i;
   if (!idx)
      return;
   DP_ASSERT_MAIN_THREAD(idx);

   /* Compact under the lock: entries[] is shared, and even though the
    * worker doesn't touch the array directly today, the queue holds
    * basename-keyed snapshots so apply_result doesn't care if our
    * indices shift.  Pending flags survive compaction in place — work
    * items already in flight retain their validity via basename +
    * (mtime, size) lookup on apply. */
   DP_LOCK(idx);
   write_i = 0;
   for (read_i = 0; read_i < idx->entries_count; read_i++)
   {
      if (idx->entries[read_i].present)
      {
         idx->entries[read_i].present = false; /* reset for next scan */
         if (write_i != read_i)
            idx->entries[write_i] = idx->entries[read_i];
         write_i++;
      }
      else
      {
         dp_entry_free_strings(&idx->entries[read_i]);
         idx->dirty = true;
      }
   }
   idx->entries_count = write_i;
   DP_UNLOCK(idx);
}

void downplay_index_set_art_state(downplay_index_t *idx,
      const char *basename, enum downplay_art_state state)
{
   dp_entry_t *e;
   if (!idx || !basename)
      return;
   DP_ASSERT_MAIN_THREAD(idx);
   DP_LOCK(idx);
   e = dp_entries_find(idx, basename);
   if (e && e->art_state != state)
   {
      e->art_state      = state;
      e->art_checked_at = (int64_t)time(NULL);
      idx->dirty        = true;
   }
   DP_UNLOCK(idx);
}

int downplay_index_pump(downplay_index_t *idx, int max_ops)
{
   int ops = 0;

   if (!idx || max_ops <= 0)
      return 0;
   if (!idx->db)
      return 0; /* matching disabled when RDB not available */
   DP_ASSERT_MAIN_THREAD(idx);

#ifdef HAVE_THREADS
   if (idx->worker)
   {
      /* Drain results produced by the worker.  Each drain hop takes the
       * lock once: pop the head, release, apply (which retakes the lock
       * briefly to mutate entries[]).  Bounded by max_ops so per-frame
       * cost is predictable.  The worker keeps producing concurrently
       * — we don't need to drain everything in one frame. */
      while (ops < max_ops)
      {
         dp_result_t r;
         slock_lock(idx->mutex);
         if (idx->results_count == 0)
         {
            slock_unlock(idx->mutex);
            break;
         }
         r = idx->results[idx->results_head];
         idx->results_head++;
         idx->results_count--;
         /* Lazy compact, mirroring dp_queue_pop_locked. */
         if (idx->results_head > idx->results_cap / 2)
         {
            if (idx->results_count > 0)
               memmove(idx->results, idx->results + idx->results_head,
                     idx->results_count * sizeof(*idx->results));
            idx->results_head = 0;
         }
         slock_unlock(idx->mutex);

         dp_apply_result(idx, &r);
         ops++;
      }
      return ops;
   }
#endif

   /* Synchronous fallback: HAVE_THREADS=0 OR worker spawn failed at
    * open.  Same dp_match_one helper the worker calls, just invoked
    * inline from the pump.  Will block the menu thread for as long as
    * one CRC takes — historical behaviour pre-threading; acceptable on
    * desktop console targets where HAVE_THREADS=0 is rare and ROMs
    * tend to be small. */
   while (ops < max_ops)
   {
      dp_work_t   work;
      dp_result_t result;
      DP_LOCK(idx);
      if (!dp_queue_pop_locked(idx, &work))
      {
         DP_UNLOCK(idx);
         break;
      }
      DP_UNLOCK(idx);

      dp_match_one(idx, &work, &result);
      dp_apply_result(idx, &result);
      ops++;
   }
   return ops;
}

void downplay_index_flush(downplay_index_t *idx)
{
   if (!idx || !idx->dirty)
      return;
   if (!idx->json_path)
      return;
   DP_ASSERT_MAIN_THREAD(idx);
   /* No lock: serializing entries[] is safe because the worker never
    * touches entries[] (it only reads/writes queue and results), and
    * every main-thread caller that mutates entries[] is mutually
    * exclusive with flush by virtue of running on the same thread.
    * Holding the mutex here would block the worker for the full JSON
    * write — pointless on storage that can stutter for tens of ms. */
   if (dp_index_save_json(idx))
      idx->dirty = false;
}

/* ---------- boxart download ---------- */

/* HTTP transfer callback.  task_data is http_transfer_data_t (the
 * fetched bytes); user_data is our file_transfer_t with the destination
 * path.  Mirrors cb_http_task_download_pl_thumbnail
 * (task_pl_thumbnail_download.c:259) but without the playlist housekeep-
 * ing — we just need the file on disk. */
static void dp_cb_boxart_download(retro_task_t *task, void *task_data,
      void *user_data, const char *err)
{
   http_transfer_data_t *data  = (http_transfer_data_t*)task_data;
   file_transfer_t      *transf = (file_transfer_t*)user_data;
   char                  output_dir[PATH_MAX_LENGTH];
   (void)task;

   if (!transf)
      return;
   if (!data || !data->data || !*transf->path)
      goto finish;
   if (data->status != 200)
   {
      err = "404 / non-200";
      goto finish;
   }

   /* Recreate <dir_thumbnails>/<system>/Named_Boxarts/ if needed. */
   strlcpy(output_dir, transf->path, sizeof(output_dir));
   path_basedir_wrapper(output_dir);
   if (!path_mkdir(output_dir))
   {
      err = "mkdir failed";
      goto finish;
   }

   if (!filestream_write_file(transf->path, data->data, data->len))
   {
      err = "write failed";
      goto finish;
   }

finish:
   if (err && *err)
      RARCH_WARN("[Downplay] boxart \"%s\" failed: %s\n",
            transf->path, err);
   else
      RARCH_LOG("[Downplay] boxart -> %s\n", transf->path);
   free(transf);
}

void downplay_metadata_request_boxart(const char *system, const char *label)
{
   settings_t      *settings;
   const char      *dir_thumbs;
   file_transfer_t *transf;
   char             img_name[PATH_MAX_LENGTH];
   char             local_path[PATH_MAX_LENGTH];
   char             local_subdir[PATH_MAX_LENGTH];
   char             raw_url[2048];
   char             url[2048];

   if (!system || !*system || !label || !*label)
      return;

   settings = config_get_ptr();
   if (!settings)
      return;
   dir_thumbs = settings->paths.directory_thumbnails;
   if (!dir_thumbs || !*dir_thumbs)
      return;

   /* Sanitize the label into a PNG basename using RA's own helper —
    * this performs the same character-substitution rules the resolver
    * uses for lookup, so the file we write lands where the resolver
    * will subsequently find it. */
   gfx_thumbnail_fill_content_img(img_name, sizeof(img_name), label, false);
   if (!*img_name)
      return;

   /* Scrub '#' to '_'.  RA's `gfx_thumbnail_fill_content_img` doesn't
    * touch it, but `#` is the URL-fragment delimiter — every byte
    * after it gets stripped by the HTTP client before the request is
    * sent (RFC 3986 §3.5).  A label like "Game #2 (USA)" otherwise
    * 404s silently because the GET hits ".../Game " on the server.
    * Mutating img_name here keeps the local filename and the URL
    * filename in sync, so a successful download lands at a path the
    * resolver will subsequently find. */
   {
      char *hash;
      while ((hash = strchr(img_name, '#')))
         *hash = '_';
   }

   /* Local destination: <dir_thumbnails>/<system>/Named_Boxarts/<label>.png */
   fill_pathname_join_special(local_subdir, dir_thumbs, system,
         sizeof(local_subdir));
   {
      char tmp[PATH_MAX_LENGTH];
      strlcpy(tmp, local_subdir, sizeof(tmp));
      fill_pathname_join_special(local_subdir, tmp, "Named_Boxarts",
            sizeof(local_subdir));
   }
   fill_pathname_join_special(local_path, local_subdir, img_name,
         sizeof(local_path));

   /* Skip if already on disk. */
   if (path_is_valid(local_path))
      return;

   /* Build the remote URL — same byte format RA's downloader uses
    * (task_pl_thumbnail_download.c:241-243), then URL-encode. */
   snprintf(raw_url, sizeof(raw_url), "%s/%s/%s/%s",
         FILE_PATH_CORE_THUMBNAILS_URL, system, "Named_Boxarts", img_name);
   net_http_urlencode_full(url, raw_url, sizeof(url));
   if (!*url)
      return;

   transf = (file_transfer_t*)calloc(1, sizeof(*transf));
   if (!transf)
      return;
   transf->enum_idx = MSG_UNKNOWN;
   strlcpy(transf->path, local_path, sizeof(transf->path));

   RARCH_LOG("[Downplay] boxart fetch: %s\n", url);
   if (!task_push_http_transfer_file(url, true /* mute */, NULL,
            dp_cb_boxart_download, transf))
   {
      RARCH_WARN("[Downplay] boxart task push failed for %s\n", url);
      free(transf);
   }
}
