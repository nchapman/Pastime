/*  Pastime - a fork of RetroArch.
 *  Copyright (C) 2026 - Pastime contributors.
 *
 *  Pastime is free software: you can redistribute it and/or modify it under
 *  the terms of the GNU General Public License as published by the Free
 *  Software Foundation, either version 3 of the License, or (at your option)
 *  any later version.
 *
 *  Pastime is distributed in the hope that it will be useful, but WITHOUT ANY
 *  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 *  FOR A PARTICULAR PURPOSE. See the GNU General Public License for more
 *  details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with Pastime. If not, see <http://www.gnu.org/licenses/>.
 */

/* HTTP/IO manager for the thumbnail subsystem.  See
 * pastime_thumbs.h for the public API contract; the on-disk binary
 * index format and the pure match cascade live in
 * pastime_thumbs_index.c.  This file owns:
 *
 *   - on-disk index TTL refresh + atomic write
 *   - per-entry attempt-state tracking
 *   - HTTP task dispatch (index + image)
 *   - background prefetch of indexes for the system list
 *   - the recents-thumbnail companion (pastime_thumbs_recents_*)
 *
 * Single-threaded: every callback runs on the main thread via
 * task_queue_check.  Coding rules: C89-style declarations, Allman
 * braces, libretro-common helpers for any cross-platform path/IO. */

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <time.h>
#include <sys/stat.h>
#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#endif

#include <boolean.h>
#include <compat/strl.h>
#include <file/file_path.h>
#include <net/net_http.h>
#include <streams/file_stream.h>
#include <streams/trans_stream.h>
#include <string/stdstring.h>
#include <rthreads/rthreads.h>

#include <retro_assert.h>
#include <retro_miscellaneous.h>
#include <queues/task_queue.h>

#include "pastime_thumbs.h"
#include "pastime_thumbs_internal.h"
#include "pastime_display_name.h"
#include "pastime_defaults.h"
#include "../verbosity.h"
#include "../msg_hash.h"
#include "../tasks/task_file_transfer.h"
#include "../tasks/tasks_internal.h"

#define DP_THUMBS_BASE_URL       "https://thumbnails.pastime.gg"
#define DP_THUMBS_INDEX_TTL_SEC  (7 * 24 * 60 * 60)
#define DP_THUMBS_QUEUE_CAP      32
#define DP_THUMBS_INFLIGHT_MAX   3

enum
{
   DP_LOAD_IDLE = 0,    /* never tried */
   DP_LOAD_FETCHING,    /* HTTP task in flight, polling for file */
   DP_LOAD_READY,       /* parsed; queries answer authoritatively */
   DP_LOAD_FAILED       /* HTTP/parse failed; queries answer UNKNOWN */
};

struct pastime_thumbs
{
   char system[256];
   char idx_path[DP_THUMBS_PATH_MAX];
   char cache_dir[DP_THUMBS_PATH_MAX];

   int  load_state;
   bool index_task_pushed;

   pastime_thumbs_index_t *index;

   /* Per-canonical fetch tracking.  Parallel to the binary index's
    * ENTRIES section; an entry's status drives _request return values
    * + de-dups in-flight downloads.  Allocated on index ready. */
   uint8_t *attempt;        /* DP_ATT_* values, length entry_count */

   /* Image fetch ring buffer of entry indices into the binary index. */
   uint32_t queue[DP_THUMBS_QUEUE_CAP];
   uint8_t  queue_pri[DP_THUMBS_QUEUE_CAP]; /* 1=active, 0=prefetch */
   int      queue_head, queue_tail;

   /* Bounded set of currently-fetching transfer contexts.  Each entry
    * is a heap-allocated `dp_img_transfer_t` whose RA HTTP task is in
    * flight; on completion the callback writes attempt[e_idx] and
    * compacts itself out of this array.  On manager close, each ctx's
    * mgr pointer is nulled so any not-yet-fired callback short-circuits
    * cleanly without touching freed manager state. */
   struct dp_img_transfer *outstanding[DP_THUMBS_INFLIGHT_MAX];
   int                     inflight;

   /* Diagnostic miss log.  Appended on first definitive miss
    * (index loaded, no match) for a given basename; in-memory set
    * prevents per-frame spam.  Path:
    *   <root>/Pastime/Thumbs/misses.log
    * Pull off-device with `adb pull` to triage real-world misses. */
   char     log_path[DP_THUMBS_PATH_MAX];
   char   **logged_misses;
   size_t   logged_misses_count;
   size_t   logged_misses_cap;
};

enum
{
   DP_ATT_UNTRIED = 0,
   DP_ATT_FETCHING,
   DP_ATT_ON_DISK,
   DP_ATT_FAILED
};

/* Subtype of file_transfer_t for per-image fetches.  Carries enough
 * context for the HTTP completion callback to write the manager's
 * `attempt[e_idx]` directly, instead of relying on a polling
 * path_is_valid() sweep that never sees failed fetches.  `mgr` is
 * nulled by pastime_thumbs_close() before the manager is freed, so
 * a callback that fires after close drops its writes safely.
 *
 * Defined here (rather than alongside dp_pf_transfer_t below) so it's
 * in scope for both dp_drain_queue and dp_cb_image_download — the
 * struct field `outstanding[]` declared above also needs the tag. */
typedef struct dp_img_transfer
{
   file_transfer_t      base;     /* must be first; .path is the on-disk dest */
   pastime_thumbs_t   *mgr;      /* NULL after manager close — see above */
   uint32_t             e_idx;    /* index into mgr->index entries */
} dp_img_transfer_t;

/* ---- internal: paths ---- */

/* Compute <root>/Thumbs/index/<system>.idx into `out`.  Single source
 * of truth for the on-disk binary-format index location: both
 * `pastime_thumbs_open` and the boot-time prefetch share this so
 * they cannot disagree on where the file lives.  Returns false if
 * the filesystem root can't be resolved.
 *
 * Filename used to be `<system>.index.json` when the cache held
 * gzipped JSON.  The on-disk format is now a packed binary file
 * (see DP_IDX_* constants); old `.index.json` caches are abandoned
 * naturally — first open after upgrade sees no `.idx`, refetches,
 * writes the new format. */
static bool dp_thumbs_index_path(const char *system,
      char *out, size_t out_size)
{
   char root[DP_THUMBS_PATH_MAX];
   char base[DP_THUMBS_PATH_MAX];
   char idx_dir[DP_THUMBS_PATH_MAX];
   char fname[256];
   if (!system || !*system || !out || out_size == 0)
      return false;
   if (!pastime_paths_get_root(root, sizeof(root)))
      return false;
   fill_pathname_join_special(base, root, "Thumbs", sizeof(base));
   fill_pathname_join_special(idx_dir, base, "index", sizeof(idx_dir));
   snprintf(fname, sizeof(fname), "%s.idx", system);
   fill_pathname_join_special(out, idx_dir, fname, out_size);
   return true;
}

/* Build the on-disk path for an image of canonical key `canonical`.
 * The canonical key is used verbatim as the filename — it's already
 * the No-Intro safe form (no path separators, since it's the title
 * of a game).  We always use `.webp` since the server is guaranteed
 * to publish WebP for every entry. */
static void dp_build_image_path(pastime_thumbs_t *t,
      const char *canonical, char *out, size_t out_size)
{
   char tmp[DP_THUMBS_PATH_MAX];
   fill_pathname_join_special(tmp, t->cache_dir, canonical, sizeof(tmp));
   snprintf(out, out_size, "%s.webp", tmp);
}

/* Resolve the on-disk path for an entry.  Stat the cache; on hit,
 * write the path and return true.  We no longer need a webp→jpg
 * sibling fallback: the manager always writes `.webp` and the
 * server always publishes `.webp`.  Pre-WebP `.jpg` siblings from
 * old Pastime versions are simply ignored (they sit on disk
 * orphaned until manual cleanup; harmless). */
static bool dp_resolve_local_image(pastime_thumbs_t *t,
      uint32_t e_idx, char *out, size_t out_size)
{
   const char *canonical = dp_idx_canonical(t->index, e_idx);
   dp_build_image_path(t, canonical, out, out_size);
   return path_is_valid(out);
}

/* Build the remote URL for an image of canonical key `canonical`. */
static void dp_build_image_url(pastime_thumbs_t *t,
      const char *canonical, char *out, size_t out_size)
{
   char raw[2048];
   snprintf(raw, sizeof(raw), "%s/%s/Named_Boxarts/%s.webp",
         DP_THUMBS_BASE_URL, t->system, canonical);
   net_http_urlencode_full(out, raw, out_size);
}

/* Hostile / misconfigured server can't blow up our cache.  Real
 * indexes are tens-to-hundreds of KB; 1 MB leaves real headroom for
 * the largest No-Intro systems (PSX, NDS, etc.) with their alt-name
 * bundles, while still capping a runaway response at a safe size.
 * Applied to the *compressed* (gzipped) payload before decompression. */
#define DP_THUMBS_PF_INDEX_MAX_BYTES (1024u * 1024u)

/* ---- internal: gzip helper ----
 *
 * Indexes are served as `index.json.gz`.  We decompress once at
 * fetch-completion time, parse the JSON straight into the binary
 * `.idx` form, and write that to disk — JSON never lands on the
 * filesystem.  See dp_cb_index_download.
 *
 * gzip wraps a deflate stream with a 10-byte header (magic 1F 8B + a
 * tiny metadata block) and an 8-byte footer carrying CRC32 + ISIZE
 * (uncompressed size mod 2^32).  Reading ISIZE up front lets us
 * allocate exactly the output buffer needed and reject anything that
 * would exceed our hard cap before we even hand bytes to zlib.
 *
 * Returns a malloc'd buffer (caller frees) on success; NULL on bad
 * magic, oversize ISIZE, alloc failure, or inflate error.  The cap
 * is the same DP_THUMBS_PF_INDEX_MAX_BYTES applied to compressed
 * input scaled by a max ratio; tune via DP_THUMBS_INDEX_MAX_RATIO
 * if the server ever switches to a different compressor. */
#define DP_THUMBS_INDEX_MAX_RATIO 32  /* gzip-of-JSON typical ratio is 6-12; 32 leaves headroom */

static uint8_t *dp_gunzip(const uint8_t *in, size_t in_len, size_t *out_len)
{
   const struct trans_stream_backend *backend;
   void                              *stream  = NULL;
   uint8_t                           *out     = NULL;
   uint32_t                           isize;
   uint32_t                           rd      = 0;
   uint32_t                           wn      = 0;
   enum trans_stream_error            xerr    = TRANS_STREAM_ERROR_NONE;
   const size_t                       max_out =
         (size_t)DP_THUMBS_PF_INDEX_MAX_BYTES * DP_THUMBS_INDEX_MAX_RATIO;

   if (out_len)
      *out_len = 0;
   if (!in || in_len < 18) /* 10-byte header + min payload + 8-byte footer */
      return NULL;
   /* gzip magic: 1F 8B.  Reject anything else (CDN error pages, raw
    * deflate, unknown compressor) before allocating. */
   if (in[0] != 0x1F || in[1] != 0x8B)
      return NULL;

   /* ISIZE is the last 4 bytes, little-endian, mod 2^32.  For inputs
    * >4 GiB this lies; we cap well below that elsewhere so it's fine. */
   isize = (uint32_t)in[in_len - 4]
         | ((uint32_t)in[in_len - 3] << 8)
         | ((uint32_t)in[in_len - 2] << 16)
         | ((uint32_t)in[in_len - 1] << 24);
   if (isize == 0 || isize > max_out)
      return NULL;
   /* trans_stream's set_in/set_out take uint32_t lengths.  Our caps
    * keep both sides well under 4 GiB today, but make the narrowing
    * explicit so a future cap bump can't silently truncate. */
   if (in_len > UINT32_MAX || (size_t)isize > UINT32_MAX)
      return NULL;

   backend = trans_stream_get_zlib_inflate_backend();
   if (!backend)
      return NULL;
   stream = backend->stream_new();
   if (!stream)
      return NULL;
   /* window_bits = MAX_WBITS (15) + 16 = 31 ⇒ gzip-only.  Strict on
    * purpose: anything that isn't gzip should already have been
    * rejected by the magic check above, but we don't want zlib to
    * silently accept a raw-deflate stream either. */
   if (backend->define && !backend->define(stream, "window_bits", 31))
   {
      backend->stream_free(stream);
      return NULL;
   }

   out = (uint8_t*)malloc(isize);
   if (!out)
   {
      backend->stream_free(stream);
      return NULL;
   }
   backend->set_in (stream, in,  (uint32_t)in_len);
   backend->set_out(stream, out, isize);
   if (!backend->trans(stream, true /* flush */, &rd, &wn, &xerr)
       || xerr != TRANS_STREAM_ERROR_NONE
       || wn != isize)
   {
      free(out);
      backend->stream_free(stream);
      return NULL;
   }
   backend->stream_free(stream);
   if (out_len)
      *out_len = (size_t)wn;
   return out;
}

/* ---- internal: HTTP callbacks (detached from manager) ----
 *
 * Both callbacks run on the main thread (RA's task system dispatches
 * `t->callback` from `task_queue_check`).  They write the file then
 * free their context — they never touch the manager.  The manager
 * discovers completion by stat'ing the path on the next pump/request. */

/* Atomically publish a binary `.idx` payload at `path`.  Writes to a
 * sibling .tmp first, renames over.  fsync's the parent directory so
 * the rename is durable across power loss; this matters less for an
 * index cache (we'd just refetch) but the cost is one syscall.
 *
 * Returns NULL on success, or a short error string for the caller's
 * log line (caller frees nothing — the string is static). */
static const char *dp_atomic_write_idx(const char *path,
      const uint8_t *buf, size_t buf_len)
{
   char  output_dir[DP_THUMBS_PATH_MAX];
   char  tmp_path[DP_THUMBS_PATH_MAX];

   if (!path || !*path || !buf || buf_len == 0)
      return "bad args";

   strlcpy(output_dir, path, sizeof(output_dir));
   path_basedir_wrapper(output_dir);
   if (!path_mkdir(output_dir))
      return "mkdir failed";

   snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);
   if (!filestream_write_file(tmp_path, buf, (int64_t)buf_len))
      return "write failed";
#ifdef _WIN32
   /* rename() refuses to clobber on Windows; remove first. */
   filestream_delete(path);
#endif
   if (rename(tmp_path, path) != 0)
   {
      filestream_delete(tmp_path);
      return "rename failed";
   }
#ifndef _WIN32
   /* fsync the parent directory so the rename survives power loss.
    * Best-effort: ENOTDIR / ENOSYS / sandboxed paths just leave us
    * with the same durability as the previous code path. */
   {
      int dir_fd = open(output_dir, O_RDONLY
#ifdef O_DIRECTORY
            | O_DIRECTORY
#endif
            );
      if (dir_fd >= 0)
      {
         (void)fsync(dir_fd);
         close(dir_fd);
      }
   }
#endif
   return NULL;
}

/* Off-main-thread worker for the JSON-decompress + parse + binary-emit
 * + atomic-write pipeline.  RA dispatches HTTP completion callbacks on
 * the main thread; doing 10–40 ms of compute there during cold-start
 * fan-out (a dozen systems landing in quick succession) is enough to
 * stutter the menu.  The HTTP callback now copies the gzipped payload
 * into a heap buffer and hands the rest to a detached sthread.
 *
 * "Completion" is fire-and-forget: the worker writes the .idx via the
 * existing atomic .tmp+rename, and the per-system manager / boot
 * prefetch discover the file on the next pump frame via path_is_valid.
 * No main-thread completion callback needed beyond the HTTP one.
 *
 * Threading invariant: the worker function must touch ONLY its own
 * job struct + the file system.  It must not call any RA logger,
 * task-queue, or manager state — those are main-thread-only.  The
 * RARCH_LOG / RARCH_WARN macros in upstream are technically not
 * thread-safe (they go through the runloop's message queue) but
 * have been observed-safe in practice for low-frequency one-shot
 * messages; we use them anyway for diagnostic visibility.  If real-
 * world telemetry shows races, switch to a deferred main-thread log. */
typedef struct
{
   uint8_t *gz_data;        /* malloc'd copy of compressed payload */
   size_t   gz_len;
   char     idx_path[DP_THUMBS_PATH_MAX];
} dp_idx_emit_job_t;

static void dp_idx_emit_worker(void *userdata)
{
   dp_idx_emit_job_t *job      = (dp_idx_emit_job_t*)userdata;
   uint8_t           *json     = NULL;
   size_t             json_len = 0;
   uint8_t           *idx_buf  = NULL;
   size_t             idx_len  = 0;
   const char        *werr     = NULL;

   json = dp_gunzip(job->gz_data, job->gz_len, &json_len);
   if (!json || json_len == 0)
   {
      werr = "gunzip failed";
      goto out;
   }
   if (!dp_idx_parse_json_to_buffer((const char*)json, json_len,
            &idx_buf, &idx_len))
   {
      werr = "parse failed";
      goto out;
   }
   werr = dp_atomic_write_idx(job->idx_path, idx_buf, idx_len);

out:
   if (werr && *werr)
      RARCH_WARN("[Pastime] thumbs index worker \"%s\" failed: %s\n",
            job->idx_path, werr);
   else
      RARCH_LOG("[Pastime] thumbs index -> %s\n", job->idx_path);
   free(json);
   free(idx_buf);
   free(job->gz_data);
   free(job);
}

/* Hand off parse+emit+write to a detached worker thread.  Takes
 * ownership of `gz_data` on success (worker frees it).  On failure
 * the caller still owns `gz_data` and must free it. */
static bool dp_idx_dispatch_emit(uint8_t *gz_data, size_t gz_len,
      const char *idx_path)
{
   dp_idx_emit_job_t *job;
   sthread_t         *t;
   if (!gz_data || gz_len == 0 || !idx_path || !*idx_path)
      return false;
   job = (dp_idx_emit_job_t*)calloc(1, sizeof(*job));
   if (!job)
      return false;
   job->gz_data = gz_data;
   job->gz_len  = gz_len;
   strlcpy(job->idx_path, idx_path, sizeof(job->idx_path));
   t = sthread_create(dp_idx_emit_worker, job);
   if (!t)
   {
      free(job);
      return false;
   }
   sthread_detach(t);
   return true;
}

static void dp_cb_index_download(retro_task_t *task, void *task_data,
      void *user_data, const char *err)
{
   http_transfer_data_t *data    = (http_transfer_data_t*)task_data;
   file_transfer_t      *transf  = (file_transfer_t*)user_data;
   uint8_t              *gz_copy = NULL;
   (void)task;

   if (!transf)
      return;
   if (!data || !data->data || !*transf->path)
   {
      err = "no data";
      goto finish;
   }
   if (data->status != 200)
   {
      err = "non-200";
      goto finish;
   }
   /* Compressed-size gate: matches dp_cb_pf_index_download.  dp_gunzip
    * also caps internally, but the early-out avoids paying memcpy +
    * thread spawn for a multi-MB CDN error page. */
   if (data->len > DP_THUMBS_PF_INDEX_MAX_BYTES)
   {
      err = "response too large";
      goto finish;
   }
   /* Copy the gzipped payload — http_transfer_data_t->data is freed
    * when this callback returns, so the worker can't borrow it. */
   gz_copy = (uint8_t*)malloc((size_t)data->len);
   if (!gz_copy)
   {
      err = "alloc failed";
      goto finish;
   }
   memcpy(gz_copy, data->data, (size_t)data->len);
   if (!dp_idx_dispatch_emit(gz_copy, (size_t)data->len, transf->path))
   {
      free(gz_copy);
      err = "worker spawn failed";
      goto finish;
   }
   /* Worker now owns gz_copy and will log the final outcome. */

finish:
   if (err && *err)
      RARCH_WARN("[Pastime] thumbs index \"%s\" failed: %s\n",
            transf->path, err);
   free(transf);
}

static void dp_cb_image_download(retro_task_t *task, void *task_data,
      void *user_data, const char *err)
{
   http_transfer_data_t *data   = (http_transfer_data_t*)task_data;
   dp_img_transfer_t    *ctx    = (dp_img_transfer_t*)user_data;
   file_transfer_t      *transf = ctx ? &ctx->base : NULL;
   char                  output_dir[DP_THUMBS_PATH_MAX];
   char                  tmp_path[DP_THUMBS_PATH_MAX];
   (void)task;

   if (!ctx)
      return;
   if (!data || !data->data || !*transf->path)
      goto finish;
   if (data->status != 200)
   {
      err = "non-200";
      goto finish;
   }
   /* Reject payloads whose magic bytes don't match the requested
    * format.  A 200 OK with HTML or zero bytes (CDN error page,
    * typo-squat, content-type confusion) would otherwise get cached
    * and handed to the image decoder on every frame for a week
    * (cache TTL).  Path extension picks which sniff to apply:
    *   .jpg  → SOI must be FF D8
    *   .webp → RIFF....WEBP (12-byte container header) */
   {
      const unsigned char *p = (const unsigned char*)data->data;
      const char          *ext = strrchr(transf->path, '.');
      bool ok = false;
      if (ext && data->len >= 12 && !strcmp(ext, ".webp"))
      {
         ok = (p[0] == 'R' && p[1] == 'I' && p[2] == 'F' && p[3] == 'F'
            && p[8] == 'W' && p[9] == 'E' && p[10] == 'B' && p[11] == 'P');
         if (!ok)
            err = "not a WebP";
      }
      else
      {
         ok = (data->len >= 4 && p[0] == 0xFF && p[1] == 0xD8);
         if (!ok)
            err = "not a JPEG";
      }
      if (!ok)
         goto finish;
   }

   strlcpy(output_dir, transf->path, sizeof(output_dir));
   path_basedir_wrapper(output_dir);
   if (!path_mkdir(output_dir))
   {
      err = "mkdir failed";
      goto finish;
   }

   /* Atomic write: .tmp + rename.  Without this, a crash mid-write
    * leaves a truncated JPEG cached at the canonical path; the next
    * pump sees the file-exists, marks ON_DISK, and the bad image
    * stays cached for the full TTL.  Mirrors the index-fetch path. */
   snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", transf->path);
   if (!filestream_write_file(tmp_path, data->data, data->len))
   {
      err = "write failed";
      goto finish;
   }
#ifdef _WIN32
   filestream_delete(transf->path);
#endif
   if (rename(tmp_path, transf->path) != 0)
   {
      filestream_delete(tmp_path);
      err = "rename failed";
      goto finish;
   }

finish:
   if (err && *err)
      RARCH_WARN("[Pastime] thumbs image \"%s\" failed: %s\n",
            transf->path, err);
   /* Settle manager state directly from the callback.  Failed fetches
    * used to leave entries permanently pinned at DP_ATT_FETCHING — the
    * old reaper polled path_is_valid() and never observed misses, so
    * after DP_THUMBS_INFLIGHT_MAX failures the queue would deadlock.
    * Now the callback writes the terminal state and frees its slot. */
   if (ctx->mgr && ctx->mgr->index && ctx->mgr->attempt)
   {
      pastime_thumbs_t *t     = ctx->mgr;
      uint32_t           e_idx = ctx->e_idx;
      int                i;
      /* err is set by every failure branch above (non-200, magic-byte
       * mismatch, mkdir/write/rename failure), so an empty err here
       * means the rename succeeded and the canonical file is on disk. */
      if (e_idx < pastime_thumbs_index_count(t->index))
         t->attempt[e_idx] = (err && *err) ? DP_ATT_FAILED : DP_ATT_ON_DISK;
      for (i = 0; i < t->inflight; i++)
      {
         if (t->outstanding[i] == ctx)
         {
            t->outstanding[i] = t->outstanding[--t->inflight];
            break;
         }
      }
   }
   free(ctx);
}

/* ---- internal: miss log ----
 *
 * Records definitive misses (index loaded, basename matched nothing)
 * to a TSV file under <root>/Pastime/Thumbs/misses.log.  In-memory
 * dedup per manager session avoids per-frame spam on the active row.
 * Diagnostic only — failure to write is silent. */

static bool dp_miss_already_logged(pastime_thumbs_t *t, const char *basename)
{
   size_t i;
   for (i = 0; i < t->logged_misses_count; i++)
   {
      if (!strcmp(t->logged_misses[i], basename))
         return true;
   }
   return false;
}

static void dp_log_miss(pastime_thumbs_t *t, const char *basename)
{
   FILE      *f;
   char     **r;
   size_t     new_cap;
   time_t     now;
   struct tm *tmv;
   char      *copy;

   if (!*t->log_path)
      return;
   if (dp_miss_already_logged(t, basename))
      return;

   /* Grow the dedup set first; if that fails, skip the log entry —
    * keeps memory bounded if some pathological folder has thousands
    * of unmatched ROMs (debug only, no point growing forever). */
   if (t->logged_misses_count == t->logged_misses_cap)
   {
      new_cap = t->logged_misses_cap ? t->logged_misses_cap * 2 : 32;
      if (new_cap > 4096)
         return;
      r = (char**)realloc(t->logged_misses, new_cap * sizeof(*r));
      if (!r)
         return;
      t->logged_misses     = r;
      t->logged_misses_cap = new_cap;
   }
   copy = strdup(basename);
   if (!copy)
      return;
   t->logged_misses[t->logged_misses_count++] = copy;

   /* Append a TSV row.  POSIX fopen("a") rather than libretro-common's
    * filestream API because we want O_APPEND atomic-line semantics
    * and don't need VFS abstraction for a debug log. */
   f = fopen(t->log_path, "a");
   if (!f)
      return;
   now = time(NULL);
   tmv = localtime(&now);
   if (tmv)
      fprintf(f, "%04d-%02d-%02d %02d:%02d:%02d\t%s\t%s\n",
            tmv->tm_year + 1900, tmv->tm_mon + 1, tmv->tm_mday,
            tmv->tm_hour, tmv->tm_min, tmv->tm_sec,
            t->system, basename);
   else
      fprintf(f, "?\t%s\t%s\n", t->system, basename);
   fclose(f);
}

/* ---- internal: state advancement ---- */

/* Compute index file age in seconds; returns -1 if missing.
 * libretro-common's RFILE wrapper has no portable mtime accessor, so
 * fall back to POSIX stat — every Pastime target (Android, macOS,
 * Linux) supports it; on Windows the MS CRT exposes the same API
 * under the same name. */
static int64_t dp_index_age_seconds(const char *path)
{
   struct stat st;
   time_t      now;
   if (!path_is_valid(path))
      return -1;
   if (stat(path, &st) != 0)
      return -1;
   now = time(NULL);
   if (st.st_mtime > now)
      return 0;
   return (int64_t)(now - st.st_mtime);
}

/* Try to load the on-disk binary index into the manager.  Reads the
 * `.idx` file into a malloc'd buffer (single read, no JSON parse),
 * validates it via dp_idx_open, and installs.  Returns true on
 * success (sets load_state=READY).  Leaves state unchanged on
 * failure so caller can decide between FETCHING (still waiting) vs
 * FAILED (give up). */
static bool dp_try_load_local_index(pastime_thumbs_t *t)
{
   int64_t  size = 0;
   void    *buf  = NULL;

   if (!path_is_valid(t->idx_path))
      return false;
   if (!filestream_read_file(t->idx_path, &buf, &size) || !buf || size <= 0)
   {
      free(buf);
      return false;
   }
   /* dp_idx_open takes ownership of buf — frees on validation failure. */
   t->index = dp_idx_open((uint8_t*)buf, (size_t)size);
   if (!t->index)
   {
      RARCH_WARN("[Pastime] thumbs: validate failed for %s\n", t->idx_path);
      return false;
   }
   t->attempt = (uint8_t*)calloc(pastime_thumbs_index_count(t->index), 1);
   if (!t->attempt)
   {
      pastime_thumbs_index_free(t->index);
      t->index = NULL;
      return false;
   }
   t->load_state = DP_LOAD_READY;
   return true;
}

/* Push the index HTTP fetch.  Caller should set state=FETCHING. */
static void dp_kick_index_fetch(pastime_thumbs_t *t)
{
   file_transfer_t *transf;
   char raw_url[2048];
   char url[2048];

   /* Server hosts a gzipped index alongside the JSON; ~10x smaller
    * over the wire.  Cache writes the decompressed JSON to disk so
    * the parse path is unaffected. */
   snprintf(raw_url, sizeof(raw_url),
         "%s/%s/Named_Boxarts/index.json.gz",
         DP_THUMBS_BASE_URL, t->system);
   net_http_urlencode_full(url, raw_url, sizeof(url));
   if (!*url)
      return;

   transf = (file_transfer_t*)calloc(1, sizeof(*transf));
   if (!transf)
      return;
   transf->enum_idx = MSG_UNKNOWN;
   strlcpy(transf->path, t->idx_path, sizeof(transf->path));

   RARCH_LOG("[Pastime] thumbs index fetch: %s\n", url);
   if (!task_push_http_transfer_file(url, true /* mute */, NULL,
            dp_cb_index_download, transf))
   {
      RARCH_WARN("[Pastime] thumbs index task push failed: %s\n", url);
      free(transf);
   }
   t->index_task_pushed = true;
}

/* Push entry_idx onto the queue if not already there.  pri=1 (active)
 * goes to the head (next-out); pri=0 (prefetch) to the tail.
 *
 * On a full queue we drop the *oldest prefetch entry* (sequentially
 * scanning back from the tail) to make room for an active request;
 * a prefetch overflow is dropped silently.  Naive "decrement tail"
 * would corrupt the contents at the new head — see code-review. */
static void dp_queue_push(pastime_thumbs_t *t, uint32_t entry_idx, uint8_t pri)
{
   int i;
   /* De-dup: skip if already queued. */
   for (i = t->queue_head; i != t->queue_tail;
         i = (i + 1) % DP_THUMBS_QUEUE_CAP)
   {
      if (t->queue[i] == entry_idx)
         return;
   }
   /* Full? */
   if (((t->queue_tail + 1) % DP_THUMBS_QUEUE_CAP) == t->queue_head)
   {
      int j;
      int found;
      if (pri != 1)
         return; /* prefetch overflow — drop. */
      /* Active request, queue full: walk forward from head, find the
       * first prefetch entry, and shift everything after it back by
       * one to overwrite it.  This preserves all active entries and
       * drops one prefetch (the oldest). */
      found = -1;
      for (i = t->queue_head; i != t->queue_tail;
            i = (i + 1) % DP_THUMBS_QUEUE_CAP)
      {
         if (t->queue_pri[i] == 0)
         {
            found = i;
            break;
         }
      }
      if (found < 0)
      {
         /* All entries are active: drop the oldest one (tail-1). */
         t->queue_tail = (t->queue_tail - 1 + DP_THUMBS_QUEUE_CAP)
               % DP_THUMBS_QUEUE_CAP;
      }
      else
      {
         /* Shift [found+1 .. tail) one slot left over `found`. */
         j = found;
         for (;;)
         {
            int next = (j + 1) % DP_THUMBS_QUEUE_CAP;
            if (next == t->queue_tail)
               break;
            t->queue[j]     = t->queue[next];
            t->queue_pri[j] = t->queue_pri[next];
            j = next;
         }
         t->queue_tail = (t->queue_tail - 1 + DP_THUMBS_QUEUE_CAP)
               % DP_THUMBS_QUEUE_CAP;
      }
   }
   if (pri == 1)
   {
      /* Insert at head: rotate head back into the slot we just freed. */
      t->queue_head = (t->queue_head - 1 + DP_THUMBS_QUEUE_CAP)
            % DP_THUMBS_QUEUE_CAP;
      t->queue[t->queue_head]     = entry_idx;
      t->queue_pri[t->queue_head] = 1;
   }
   else
   {
      t->queue[t->queue_tail]     = entry_idx;
      t->queue_pri[t->queue_tail] = 0;
      t->queue_tail = (t->queue_tail + 1) % DP_THUMBS_QUEUE_CAP;
   }
}

/* Drain one queue slot to an in-flight HTTP task if we're below the
 * concurrency cap.  Called from pump.  Caller is responsible for
 * looping (within budget) — this only dispatches at most one task.
 *
 * Inflight bookkeeping: on successful task push we record the heap
 * `dp_img_transfer_t` in t->outstanding[].  The HTTP completion
 * callback writes the terminal attempt state (ON_DISK or FAILED) and
 * compacts itself out of that array.  No polling sweep needed. */
static void dp_drain_queue(pastime_thumbs_t *t)
{
   uint32_t e_idx;
   const char *canonical;
   char path[DP_THUMBS_PATH_MAX];
   char url[2048];
   dp_img_transfer_t *ctx;

   if (t->load_state != DP_LOAD_READY)
      return;
   if (t->queue_head == t->queue_tail)
      return;
   if (t->inflight >= DP_THUMBS_INFLIGHT_MAX)
      return;

   e_idx = t->queue[t->queue_head];
   t->queue_head = (t->queue_head + 1) % DP_THUMBS_QUEUE_CAP;

   if (e_idx >= pastime_thumbs_index_count(t->index))
      return;
   /* Re-check state: row may have transitioned to ON_DISK since enqueue. */
   if (t->attempt[e_idx] != DP_ATT_UNTRIED
       && t->attempt[e_idx] != DP_ATT_FETCHING)
      return;

   /* Lazy on-disk check.  This entry was queued without an upfront
    * stat-sweep proving it was missing, so recheck now — the user
    * may have triggered a fetch for an image already cached on disk
    * (webp landed via a different code path, etc.). */
   if (dp_resolve_local_image(t, e_idx, path, sizeof(path)))
   {
      t->attempt[e_idx] = DP_ATT_ON_DISK;
      return;
   }
   canonical = dp_idx_canonical(t->index, e_idx);
   dp_build_image_url(t, canonical, url, sizeof(url));
   if (!*url)
      return;
   ctx = (dp_img_transfer_t*)calloc(1, sizeof(*ctx));
   if (!ctx)
      return;
   ctx->base.enum_idx = MSG_UNKNOWN;
   strlcpy(ctx->base.path, path, sizeof(ctx->base.path));
   ctx->mgr   = t;
   ctx->e_idx = e_idx;
   /* Pass &ctx->base (file_transfer_t*); the callback casts user_data
    * back to dp_img_transfer_t* via the first-member-aliasing rule. */
   if (!task_push_http_transfer_file(url, true /* mute */, NULL,
            dp_cb_image_download, &ctx->base))
   {
      free(ctx);
      t->attempt[e_idx] = DP_ATT_FAILED;
      return;
   }
   t->attempt[e_idx]            = DP_ATT_FETCHING;
   t->outstanding[t->inflight++] = ctx;
}

/* ---- index prefetch (boot-time fan-out) ---------------------------
 *
 * Drive a small global queue of <root>/Thumbs/index/<system>.idx
 * fetches at app launch, before the user can navigate.  By the time
 * they enter any system view, the index is already on disk and
 * `_open` skips its own HTTP call.
 *
 * THREADING INVARIANT: every entry below (g_pf_pending, g_pf_inflight,
 * the count ints, and every helper that touches them) is main-thread
 * only.  RA's task callbacks fire from `retro_task_internal_gather`,
 * which is reached only from `task_queue_check`; that in turn is
 * called from main-thread sites (runloop + menu select-handler).
 * Both threaded and non-threaded task modes funnel through this one
 * gather point.  Therefore no locks are required.  If a future
 * maintainer ever moves task_queue_check off the main thread, this
 * block needs an slock — the corruption would otherwise be silent. */

#define DP_THUMBS_PF_PENDING_CAP   128
#define DP_THUMBS_PF_INFLIGHT_MAX  3
/* DP_THUMBS_PF_INDEX_MAX_BYTES lives near the gunzip helper above
 * (the compressed payload cap is also the input to the decompressed
 * cap calculation).  Both callbacks gate on it. */

static char g_pf_pending[DP_THUMBS_PF_PENDING_CAP][256];
static int  g_pf_pending_count;
static bool g_pf_overflow_warned; /* one-shot log when cap is first hit */

static char g_pf_inflight[DP_THUMBS_PF_INFLIGHT_MAX][256];
static int  g_pf_inflight_count;

/* Subtype of file_transfer_t (must be first member): RA's task API
 * takes file_transfer_t* and reads .path on completion; we tail-extend
 * with the canonical system name so the callback can update the
 * in-flight set without re-parsing the path. */
typedef struct
{
   file_transfer_t base;          /* must be first; .path is the on-disk dest */
   char            system[256];   /* canonical name; key for inflight set */
} dp_pf_transfer_t;

/* Reject malformed system strings.  Path-escape (..  /  \) was the
 * obvious vector; NUL / control chars in the middle are a subtler one
 * — they'd silently truncate our `g_pf_inflight` keys (kernel terminates
 * at NUL) and let two concurrent fetches dodge dedup with different
 * effective names but the same ToS prefix.  Single source of truth:
 * pastime_thumbs_open / recents API / prefetch all funnel through
 * this validator — they treat `system` as both a URL component and a
 * filesystem directory name, so the rules are the same for all three. */
static bool dp_system_safe(const char *system)
{
   const unsigned char *p;
   if (!system || !*system)
      return false;
   if (   strstr(system, "..")
       || strchr(system, '/')
       || strchr(system, '\\'))
      return false;
   for (p = (const unsigned char*)system; *p; p++)
      if (*p < 0x20 || *p == 0x7F)
         return false;
   return true;
}

static bool dp_pf_inflight_contains(const char *system)
{
   int i;
   if (!system)
      return false;
   for (i = 0; i < g_pf_inflight_count; i++)
   {
      if (string_is_equal(g_pf_inflight[i], system))
         return true;
   }
   return false;
}

static bool dp_pf_pending_contains(const char *system)
{
   int i;
   if (!system)
      return false;
   for (i = 0; i < g_pf_pending_count; i++)
   {
      if (string_is_equal(g_pf_pending[i], system))
         return true;
   }
   return false;
}

static bool dp_pf_inflight_add(const char *system)
{
   if (g_pf_inflight_count >= DP_THUMBS_PF_INFLIGHT_MAX)
      return false;
   strlcpy(g_pf_inflight[g_pf_inflight_count], system,
         sizeof(g_pf_inflight[0]));
   g_pf_inflight_count++;
   return true;
}

static void dp_pf_inflight_remove(const char *system)
{
   int i;
   if (!system)
      return;
   for (i = 0; i < g_pf_inflight_count; i++)
   {
      if (string_is_equal(g_pf_inflight[i], system))
      {
         /* Compact: move last entry into this slot. */
         g_pf_inflight_count--;
         if (i != g_pf_inflight_count)
            strlcpy(g_pf_inflight[i],
                  g_pf_inflight[g_pf_inflight_count],
                  sizeof(g_pf_inflight[0]));
         g_pf_inflight[g_pf_inflight_count][0] = '\0';
         return;
      }
   }
}

/* Remove `system` from pending if present.  Used when a user-initiated
 * `_open` arrives for a system that was queued but not yet started:
 * we pull it out of pending and kick the fetch directly so the user
 * doesn't wait behind unrelated systems already inflight.  Returns
 * true if the entry was found and removed. */
static bool dp_pf_pending_remove(const char *system)
{
   int i;
   int j;
   if (!system)
      return false;
   for (i = 0; i < g_pf_pending_count; i++)
   {
      if (!string_is_equal(g_pf_pending[i], system))
         continue;
      for (j = i + 1; j < g_pf_pending_count; j++)
         strlcpy(g_pf_pending[j - 1], g_pf_pending[j],
               sizeof(g_pf_pending[0]));
      g_pf_pending_count--;
      g_pf_pending[g_pf_pending_count][0] = '\0';
      return true;
   }
   return false;
}

static bool dp_pf_pending_pop(char *out_system, size_t out_size)
{
   int i;
   if (g_pf_pending_count <= 0)
      return false;
   strlcpy(out_system, g_pf_pending[0], out_size);
   for (i = 1; i < g_pf_pending_count; i++)
      strlcpy(g_pf_pending[i - 1], g_pf_pending[i],
            sizeof(g_pf_pending[0]));
   g_pf_pending_count--;
   g_pf_pending[g_pf_pending_count][0] = '\0';
   return true;
}

static void dp_pf_drain(void); /* fwd */

static void dp_cb_pf_index_download(retro_task_t *task, void *task_data,
      void *user_data, const char *err)
{
   http_transfer_data_t *data    = (http_transfer_data_t*)task_data;
   dp_pf_transfer_t     *pf      = (dp_pf_transfer_t*)user_data;
   uint8_t              *gz_copy = NULL;
   (void)task;

   retro_assert(task_is_on_main_thread());
   if (!pf)
      return;
   if (!data || !data->data || !*pf->base.path)
   {
      err = "no data";
      goto finish;
   }
   if (data->status != 200)
   {
      err = "non-200";
      goto finish;
   }
   if (data->len > DP_THUMBS_PF_INDEX_MAX_BYTES)
   {
      err = "response too large";
      goto finish;
   }
   /* Hand parse+emit+write off to a detached worker.  The prefetch
    * inflight slot is released below regardless of worker dispatch
    * outcome — the network round-trip is done either way, and a
    * stuck slot would block the rest of the boot fan-out. */
   gz_copy = (uint8_t*)malloc((size_t)data->len);
   if (!gz_copy)
   {
      err = "alloc failed";
      goto finish;
   }
   memcpy(gz_copy, data->data, (size_t)data->len);
   if (!dp_idx_dispatch_emit(gz_copy, (size_t)data->len, pf->base.path))
   {
      free(gz_copy);
      err = "worker spawn failed";
      goto finish;
   }
   /* Worker now owns gz_copy + will log final outcome. */

finish:
   if (err && *err)
      RARCH_WARN("[Pastime] thumbs prefetch \"%s\" failed: %s\n",
            pf->system, err);
   dp_pf_inflight_remove(pf->system);
   free(pf);
   dp_pf_drain();
}

/* Drain the pending queue up to the concurrency cap. */
static void dp_pf_drain(void)
{
   retro_assert(task_is_on_main_thread());
   while (g_pf_inflight_count < DP_THUMBS_PF_INFLIGHT_MAX
         && g_pf_pending_count > 0)
   {
      char system[256];
      char idx_path[DP_THUMBS_PATH_MAX];
      char raw_url[2048];
      char url[2048];
      dp_pf_transfer_t *pf;
      int64_t age;

      if (!dp_pf_pending_pop(system, sizeof(system)))
         break;

      /* Re-validate before constructing the URL.  Defence-in-depth:
       * the public-API path filters at enqueue, but if any future
       * code path inserts directly into g_pf_pending this stops a
       * bogus name from reaching the wire. */
      if (!dp_system_safe(system))
         continue;
      /* Re-check freshness right before issuing — the file may have
       * landed in the window between enqueue and drain (e.g. a user
       * `_open` raced ahead of us and won). */
      if (!dp_thumbs_index_path(system, idx_path, sizeof(idx_path)))
         continue;
      age = dp_index_age_seconds(idx_path);
      if (age >= 0 && age < DP_THUMBS_INDEX_TTL_SEC)
         continue;

      snprintf(raw_url, sizeof(raw_url),
            "%s/%s/Named_Boxarts/index.json.gz",
            DP_THUMBS_BASE_URL, system);
      net_http_urlencode_full(url, raw_url, sizeof(url));
      if (!*url)
         continue;

      pf = (dp_pf_transfer_t*)calloc(1, sizeof(*pf));
      if (!pf)
         continue;
      pf->base.enum_idx = MSG_UNKNOWN;
      strlcpy(pf->base.path, idx_path, sizeof(pf->base.path));
      strlcpy(pf->system,    system,   sizeof(pf->system));

      if (!dp_pf_inflight_add(system))
      {
         free(pf);
         continue;
      }
      RARCH_LOG("[Pastime] thumbs prefetch fetch: %s\n", url);
      if (!task_push_http_transfer_file(url, true /* mute */, NULL,
               dp_cb_pf_index_download, &pf->base))
      {
         RARCH_WARN("[Pastime] thumbs prefetch task push failed: %s\n",
               url);
         dp_pf_inflight_remove(system);
         free(pf);
      }
   }
}

void pastime_thumbs_prefetch_indexes(
      const char * const *systems, size_t count)
{
   size_t i;
   retro_assert(task_is_on_main_thread());
   if (!systems || !count)
      return;
   /* Reset the one-shot overflow log: callers are entitled to know
    * about a fresh-call overflow even if a prior call hit the cap. */
   g_pf_overflow_warned = false;
   for (i = 0; i < count; i++)
   {
      const char *system = systems[i];
      char idx_path[DP_THUMBS_PATH_MAX];
      int64_t age;

      if (!dp_system_safe(system))
         continue;
      /* Bound: must fit our fixed-size slot. */
      if (strlen(system) >= sizeof(g_pf_pending[0]))
         continue;
      /* Skip if fresh on disk. */
      if (!dp_thumbs_index_path(system, idx_path, sizeof(idx_path)))
         continue;
      age = dp_index_age_seconds(idx_path);
      if (age >= 0 && age < DP_THUMBS_INDEX_TTL_SEC)
         continue;
      /* Skip if already in flight or already queued. */
      if (dp_pf_inflight_contains(system))
         continue;
      if (dp_pf_pending_contains(system))
         continue;
      if (g_pf_pending_count >= DP_THUMBS_PF_PENDING_CAP)
      {
         /* Capacity is generous (128 systems); a hit means something
          * unusual.  Warn once so we have a bread crumb in logcat. */
         if (!g_pf_overflow_warned)
         {
            RARCH_WARN("[Pastime] thumbs prefetch queue full at %d; "
                  "remaining systems will not be prefetched\n",
                  DP_THUMBS_PF_PENDING_CAP);
            g_pf_overflow_warned = true;
         }
         continue;
      }
      strlcpy(g_pf_pending[g_pf_pending_count], system,
            sizeof(g_pf_pending[0]));
      g_pf_pending_count++;
   }
   dp_pf_drain();
}

/* ---- public manager API ---- */

pastime_thumbs_t *pastime_thumbs_open(const char *system)
{
   pastime_thumbs_t *t;
   char root[DP_THUMBS_PATH_MAX];
   char base[DP_THUMBS_PATH_MAX];

   /* `system` is used as both a URL path component AND a filesystem
    * directory name; reject anything that could escape either.  In
    * the common path it comes from the curated disambig table, but
    * the core_info `database` fallback is third-party content. */
   if (!dp_system_safe(system))
      return NULL;
   if (!pastime_paths_get_root(root, sizeof(root)))
      return NULL;

   t = (pastime_thumbs_t*)calloc(1, sizeof(*t));
   if (!t)
      return NULL;

   strlcpy(t->system, system, sizeof(t->system));

   /* <root>/Thumbs/ */
   fill_pathname_join_special(base, root, "Thumbs", sizeof(base));
   /* <root>/Thumbs/index/<system>.idx — via shared helper so boot-
    * time prefetch and per-system open agree on the location. */
   if (!dp_thumbs_index_path(system, t->idx_path, sizeof(t->idx_path)))
   {
      free(t);
      return NULL;
   }
   /* <root>/Thumbs/<system>/ */
   fill_pathname_join_special(t->cache_dir, base, system,
         sizeof(t->cache_dir));
   /* <root>/Thumbs/misses.log */
   fill_pathname_join_special(t->log_path, base, "misses.log",
         sizeof(t->log_path));

   t->queue_head = t->queue_tail = 0;

   /* If on-disk index is present and fresh, parse synchronously.
    * Otherwise fire HTTP fetch. */
   {
      int64_t age = dp_index_age_seconds(t->idx_path);
      if (age >= 0 && age < DP_THUMBS_INDEX_TTL_SEC)
      {
         if (dp_try_load_local_index(t))
            return t;
      }
   }
   t->load_state = DP_LOAD_FETCHING;
   /* Priority promotion: if this system is queued in the boot-time
    * prefetch but hasn't started yet, the user shouldn't wait behind
    * unrelated systems already in flight.  Pull it out of pending and
    * kick the fetch ourselves — the prefetch pool's concurrency cap
    * doesn't apply to user-initiated fetches.  If it's already in
    * flight (callback will land soon) we suppress to avoid a duplicate
    * HTTP transfer; both prefetch and direct callbacks atomic-rename
    * to the same `idx_path`, so racing them is correct but wasteful. */
   dp_pf_pending_remove(system);
   if (!dp_pf_inflight_contains(system))
      dp_kick_index_fetch(t);
   return t;
}

void pastime_thumbs_close(pastime_thumbs_t *t)
{
   int i;
   if (!t)
      return;
   /* Detach in-flight HTTP tasks: they hold dp_img_transfer_t* whose
    * `mgr` field we null here.  Any callback that fires after we
    * return sees mgr==NULL and skips its writes to manager state
    * (attempt[], outstanding[]) before freeing its own ctx. */
   for (i = 0; i < t->inflight; i++)
      if (t->outstanding[i])
         t->outstanding[i]->mgr = NULL;
   t->inflight = 0;
   pastime_thumbs_index_free(t->index);
   free(t->attempt);
   {
      size_t i;
      for (i = 0; i < t->logged_misses_count; i++)
         free(t->logged_misses[i]);
      free(t->logged_misses);
   }
   free(t);
}

void pastime_thumbs_request(pastime_thumbs_t *t,
      const char *rom_basename,
      pastime_thumb_result_t *out)
{
   int e_idx;

   if (!out)
      return;
   out->status        = DP_THUMB_UNKNOWN;
   out->local_path[0] = '\0';
   out->image_w       = 0;
   out->image_h       = 0;
   out->thumbhash     = NULL;
   out->thumbhash_len = 0;
   if (!t || !rom_basename || !*rom_basename)
      return;

   /* Maybe the index just landed.  Cheap: stat once. */
   if (t->load_state == DP_LOAD_FETCHING && !t->index
       && path_is_valid(t->idx_path))
   {
      if (!dp_try_load_local_index(t))
      {
         /* File present but parse failed — give up.  A future open()
          * after the cached file is replaced (TTL expiry, manual
          * delete) will retry. */
         t->load_state = DP_LOAD_FAILED;
      }
   }

   if (t->load_state != DP_LOAD_READY)
      return; /* UNKNOWN — caller polls again next frame */

   {
      size_t mi = dp_idx_match(t->index, rom_basename);
      if (mi == (size_t)-1)
      {
         out->status = DP_THUMB_MISSING;
         dp_log_miss(t, rom_basename);
         return;
      }
      e_idx = (int)mi;
   }

   /* Populate per-entry metadata regardless of where the function
    * returns from below — the menu driver wants dims at layout time
    * even before the image file lands on disk.  Thumbhash pointer
    * lifetime is bounded by t->index (see header contract). */
   dp_idx_dims(t->index, (uint32_t)e_idx, &out->image_w, &out->image_h);
   dp_idx_thumbhash(t->index, (uint32_t)e_idx,
         &out->thumbhash, &out->thumbhash_len);

   if (t->attempt[e_idx] == DP_ATT_ON_DISK)
   {
      dp_build_image_path(t, dp_idx_canonical(t->index, (uint32_t)e_idx),
            out->local_path, sizeof(out->local_path));
      out->status = DP_THUMB_OK;
      return;
   }
   if (dp_resolve_local_image(t, (uint32_t)e_idx,
            out->local_path, sizeof(out->local_path)))
   {
      t->attempt[e_idx] = DP_ATT_ON_DISK;
      out->status = DP_THUMB_OK;
      return;
   }

   if (t->attempt[e_idx] == DP_ATT_FAILED)
   {
      /* We tried to push the HTTP task and the queue/network rejected
       * it.  Surface as MISSING (per-row art will not appear) rather
       * than leaving the row stuck at UNKNOWN forever. */
      out->status = DP_THUMB_MISSING;
      out->local_path[0] = '\0';
      return;
   }

   /* Not on disk and not yet tried; promote to active queue.  pri=1.
    * If currently FETCHING we leave it alone — pump will discover
    * landing on a future frame. */
   if (t->attempt[e_idx] == DP_ATT_UNTRIED)
      dp_queue_push(t, (uint32_t)e_idx, 1);
   /* status stays UNKNOWN — next frame will see ON_DISK once landed. */
}

bool pastime_thumbs_peek(pastime_thumbs_t *t,
      const char *rom_basename,
      uint16_t *out_w, uint16_t *out_h,
      const uint8_t **out_thumbhash, size_t *out_thumbhash_len)
{
   size_t mi;
   if (out_w)             *out_w = 0;
   if (out_h)             *out_h = 0;
   if (out_thumbhash)     *out_thumbhash = NULL;
   if (out_thumbhash_len) *out_thumbhash_len = 0;
   if (!t || !rom_basename || !*rom_basename)
      return false;
   /* Caller is responsible for pumping the manager — peek itself
    * never triggers I/O.  If the index isn't loaded yet, the peek
    * misses and the caller retries on a later frame. */
   if (t->load_state != DP_LOAD_READY || !t->index)
      return false;
   mi = dp_idx_match(t->index, rom_basename);
   if (mi == (size_t)-1)
      return false;
   if (out_w || out_h)
   {
      uint16_t w = 0, h = 0;
      dp_idx_dims(t->index, (uint32_t)mi, &w, &h);
      if (out_w) *out_w = w;
      if (out_h) *out_h = h;
   }
   if (out_thumbhash)
   {
      const uint8_t *p = NULL;
      size_t         n = 0;
      dp_idx_thumbhash(t->index, (uint32_t)mi, &p, &n);
      *out_thumbhash = p;
      if (out_thumbhash_len) *out_thumbhash_len = n;
   }
   return true;
}

void pastime_thumbs_prefetch(pastime_thumbs_t *t,
      const char * const *basenames, size_t count)
{
   size_t i;
   if (!t || t->load_state != DP_LOAD_READY || !basenames)
      return;
   for (i = 0; i < count; i++)
   {
      size_t mi;
      if (!basenames[i])
         continue;
      mi = dp_idx_match(t->index, basenames[i]);
      if (mi == (size_t)-1)
         continue;
      if (t->attempt[mi] != DP_ATT_UNTRIED)
         continue;
      dp_queue_push(t, (uint32_t)mi, 0 /* prefetch */);
   }
}

void pastime_thumbs_pump(pastime_thumbs_t *t)
{
   int budget;
   if (!t)
      return;
   if (t->load_state == DP_LOAD_FETCHING && !t->index
       && path_is_valid(t->idx_path))
   {
      if (!dp_try_load_local_index(t))
         t->load_state = DP_LOAD_FAILED;
   }
   if (t->load_state != DP_LOAD_READY)
      return;
   /* No reap step: dp_cb_image_download settles attempt[e_idx] and
    * removes itself from t->outstanding directly, so finished/failed
    * slots are already free by the time we get here. */
   /* Drain queued requests up to the concurrency cap. */
   for (budget = DP_THUMBS_INFLIGHT_MAX; budget > 0; budget--)
   {
      int before_head = t->queue_head;
      if (t->inflight >= DP_THUMBS_INFLIGHT_MAX)
         break;
      dp_drain_queue(t);
      if (t->queue_head == before_head)
         break;
   }
}

/* ---- recents resolver -----------------------------------------
 *
 * Lazy multi-system reader.  Holds at most one parsed binary index
 * per distinct system seen across the recents view.  No HTTP, no
 * pump, no prefetch — if a system's `.idx` isn't on disk yet the
 * row simply renders without art.  Designed so the recents view
 * does the minimum work to surface a thumbnail when one's already
 * cached (i.e. the user has visited that system before).
 *
 * Lifecycle: open on view enter, resolve per row per frame, close
 * on view exit.  Slot table grows by doubling; expected steady-
 * state size is the small set of distinct systems in the user's
 * recently-played list (typically <10). */

typedef struct
{
   char                    *system;     /* heap, owned */
   pastime_thumbs_index_t *index;      /* NULL on miss/parse-fail */
   bool                     tried;      /* don't retry the load every frame */
} dp_recents_slot_t;

struct pastime_thumbs_recents
{
   dp_recents_slot_t *slots;
   size_t             count;
   size_t             cap;
};

pastime_thumbs_recents_t *pastime_thumbs_recents_open(void)
{
   return (pastime_thumbs_recents_t*)calloc(1,
         sizeof(pastime_thumbs_recents_t));
}

void pastime_thumbs_recents_close(pastime_thumbs_recents_t *r)
{
   size_t i;
   if (!r)
      return;
   for (i = 0; i < r->count; i++)
   {
      free(r->slots[i].system);
      pastime_thumbs_index_free(r->slots[i].index);
   }
   free(r->slots);
   free(r);
}

/* Find or insert the slot for `system`.  On insert, sets tried=false
 * so the next resolve attempts a load exactly once.  Linear scan is
 * fine here — distinct-systems count is in the single digits in
 * practice.  Returns NULL on allocation failure. */
static dp_recents_slot_t *dp_recents_get_slot(
      pastime_thumbs_recents_t *r, const char *system)
{
   size_t i;
   dp_recents_slot_t *s;
   for (i = 0; i < r->count; i++)
      if (string_is_equal(r->slots[i].system, system))
         return &r->slots[i];
   if (r->count == r->cap)
   {
      size_t             new_cap = r->cap ? r->cap * 2 : 8;
      dp_recents_slot_t *n       = (dp_recents_slot_t*)realloc(
            r->slots, new_cap * sizeof(*n));
      if (!n)
         return NULL;
      r->slots = n;
      r->cap   = new_cap;
   }
   s = &r->slots[r->count];
   memset(s, 0, sizeof(*s));
   s->system = strdup(system);
   if (!s->system)
      return NULL;
   r->count++;
   return s;
}

/* Lazy-load the slot's binary index from disk if not yet attempted.
 * Marks the slot tried even on a missing-or-broken file so subsequent
 * frames don't re-stat / re-read.  Returns true if work was done
 * this call (i.e. the slot just transitioned from untried). */
static bool dp_recents_slot_load(dp_recents_slot_t *slot)
{
   char     idx_path[DP_THUMBS_PATH_MAX];
   int64_t  size = 0;
   void    *buf  = NULL;
   if (!slot || slot->tried || !slot->system)
      return false;
   slot->tried = true;
   if (!dp_thumbs_index_path(slot->system, idx_path, sizeof(idx_path)))
      return true;
   if (!path_is_valid(idx_path))
      return true;
   if (filestream_read_file(idx_path, &buf, &size) && buf && size > 0)
      slot->index = dp_idx_open((uint8_t*)buf, (size_t)size);
   else
      free(buf);
   return true;
}

void pastime_thumbs_recents_seed(pastime_thumbs_recents_t *r,
      const char *system)
{
   if (!r || !dp_system_safe(system))
      return;
   /* dp_recents_get_slot creates the slot on first sight; we don't
    * care about the return — the slot exists in the table now and
    * pump/resolve will pick it up. */
   (void)dp_recents_get_slot(r, system);
}

bool pastime_thumbs_recents_pump(pastime_thumbs_recents_t *r)
{
   size_t i;
   if (!r)
      return false;
   /* Walk slots in seed order; load the first not-yet-tried one and
    * stop.  This bounds blocking I/O to one .idx read per frame —
    * a 1500-system recents list (impossible in practice; bound is ~10)
    * would still pre-warm in well under a second of real time. */
   for (i = 0; i < r->count; i++)
      if (dp_recents_slot_load(&r->slots[i]))
         return true;
   return false;
}

bool pastime_thumbs_recents_peek(pastime_thumbs_recents_t *r,
      const char *system, const char *rom_basename,
      uint16_t *out_w, uint16_t *out_h,
      const uint8_t **out_thumbhash, size_t *out_thumbhash_len)
{
   dp_recents_slot_t *slot;
   size_t             mi;
   if (out_w)             *out_w = 0;
   if (out_h)             *out_h = 0;
   if (out_thumbhash)     *out_thumbhash = NULL;
   if (out_thumbhash_len) *out_thumbhash_len = 0;
   if (!r || !rom_basename || !*rom_basename)
      return false;
   if (!dp_system_safe(system))
      return false;
   slot = dp_recents_get_slot(r, system);
   if (!slot || !slot->index)
      return false;  /* not yet pumped, or load failed — caller retries */
   mi = dp_idx_match(slot->index, rom_basename);
   if (mi == (size_t)-1)
      return false;
   if (out_w || out_h)
   {
      uint16_t w = 0, h = 0;
      dp_idx_dims(slot->index, (uint32_t)mi, &w, &h);
      if (out_w) *out_w = w;
      if (out_h) *out_h = h;
   }
   if (out_thumbhash)
   {
      const uint8_t *p = NULL;
      size_t         n = 0;
      dp_idx_thumbhash(slot->index, (uint32_t)mi, &p, &n);
      *out_thumbhash = p;
      if (out_thumbhash_len) *out_thumbhash_len = n;
   }
   return true;
}

bool pastime_thumbs_recents_resolve(pastime_thumbs_recents_t *r,
      const char *system, const char *rom_basename,
      char *out, size_t out_size)
{
   dp_recents_slot_t *slot;
   size_t             mi;
   char               root[DP_THUMBS_PATH_MAX];
   char               base[DP_THUMBS_PATH_MAX];
   char               cache_dir[DP_THUMBS_PATH_MAX];
   char               tmp[DP_THUMBS_PATH_MAX];

   if (!r || !rom_basename || !*rom_basename
       || !out || out_size == 0)
      return false;
   if (!dp_system_safe(system))
      return false;

   slot = dp_recents_get_slot(r, system);
   if (!slot)
      return false;
   /* No-op once tried — pump pre-warmed it, or a previous resolve
    * already settled it.  Worst case (caller hovered a row before
    * pump caught up, or never seeded): load inline now.  slot_load
    * marks tried regardless of outcome so the pump skips this slot
    * on subsequent frames. */
   dp_recents_slot_load(slot);

   if (!slot->index)
      return false;
   mi = dp_idx_match(slot->index, rom_basename);
   if (mi == (size_t)-1)
      return false;

   if (!pastime_paths_get_root(root, sizeof(root)))
      return false;
   fill_pathname_join_special(base, root, "Thumbs", sizeof(base));
   fill_pathname_join_special(cache_dir, base, system, sizeof(cache_dir));
   fill_pathname_join_special(tmp, cache_dir,
         dp_idx_canonical(slot->index, (uint32_t)mi), sizeof(tmp));
   snprintf(out, out_size, "%s.webp", tmp);
   return path_is_valid(out);
}

