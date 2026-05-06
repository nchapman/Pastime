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
 */

#include <stdlib.h>
#include <string.h>

#ifdef HAVE_CONFIG_H
#include "../config.h"
#endif

#include <file/file_path.h>
#include <lists/dir_list.h>
#include <lists/string_list.h>
#include <streams/file_stream.h>
#include <net/net_http.h>
#include <queues/task_queue.h>

#include "pastime_setup.h"
#include "pastime_cores.h"

#include "../command.h"
#include "../configuration.h"
#include "../file_path_special.h"
#include "../input/input_driver.h"
#include "../msg_hash.h"
#include "../tasks/tasks_internal.h"
#include "../tasks/task_file_transfer.h"
#include "../verbosity.h"

/* ---------- bucket descriptors ---------- */

/* Returns the install dir for the bucket, or NULL if unresolvable.
 * Sourced from settings; cb_generic_download's switch is the canonical
 * mapping (menu/cbs/menu_cbs_ok.c). */
typedef const char *(*bucket_dir_fn)(settings_t *);

typedef struct
{
   const char         *phase_label;       /* "Downloading assets..." */
   const char         *zip_filename;      /* "assets.zip" */
   enum msg_hash_enums enum_idx;          /* used as the http "type" string */
   bucket_dir_fn       get_dir;
   /* Optional fixed subpath joined onto get_dir() — used by shaders,
    * which install into "<directory_video_shader>/shaders_slang"
    * rather than the bare base.  NULL = use get_dir as-is. */
   const char         *dir_subpath;
   bool                use_joypad_subdir; /* autoconfig only */
   enum event_command  post_install_cmd;  /* CMD_EVENT_NONE if none */
} pastime_setup_bucket_t;

static const char *bucket_dir_assets(settings_t *s)
   { return s ? s->paths.directory_assets        : NULL; }
static const char *bucket_dir_autoconfig(settings_t *s)
   { return s ? s->paths.directory_autoconfig    : NULL; }
static const char *bucket_dir_databases(settings_t *s)
   { return s ? s->paths.path_content_database   : NULL; }
static const char *bucket_dir_overlays(settings_t *s)
   { return s ? s->paths.directory_overlay       : NULL; }
static const char *bucket_dir_core_info(settings_t *s)
   { return s ? s->paths.path_libretro_info      : NULL; }
static const char *bucket_dir_video_shader(settings_t *s)
   { return s ? s->paths.directory_video_shader  : NULL; }

/* Resolve the install dir for a bucket, applying dir_subpath if set.
 * Returns false when the base dir is unset.  out must be PATH_MAX_LENGTH. */
static bool pastime_setup_resolve_dir(
      const pastime_setup_bucket_t *b, settings_t *s,
      char *out, size_t out_len)
{
   const char *base = b->get_dir(s);
   if (!base || !*base)
      return false;
   if (b->dir_subpath && *b->dir_subpath)
      fill_pathname_join_special(out, base, b->dir_subpath, out_len);
   else
      strlcpy(out, base, out_len);
   return true;
}

/* Order matters for UX (what the user sees in sequence) and for
 * post-install side effects: core_info first so subsequent core lookups
 * see refreshed metadata; assets/autoconfig before others so the REINIT
 * they trigger lands once, early.  Cheats are intentionally absent — see
 * PLAN.md (extracting thousands of tiny .cht files dominates first-run
 * time on Android). */
static const pastime_setup_bucket_t k_buckets[] =
{
   { "Downloading core info...",
     FILE_PATH_CORE_INFO_ZIP,
     MENU_ENUM_LABEL_CB_UPDATE_CORE_INFO_FILES,
     bucket_dir_core_info,  NULL,            false, CMD_EVENT_CORE_INFO_INIT },
   { "Downloading assets...",
     FILE_PATH_ASSETS_ZIP,
     MENU_ENUM_LABEL_CB_UPDATE_ASSETS,
     bucket_dir_assets,     NULL,            false, CMD_EVENT_REINIT },
   { "Downloading controller profiles...",
     FILE_PATH_AUTOCONFIG_ZIP,
     MENU_ENUM_LABEL_CB_UPDATE_AUTOCONFIG_PROFILES,
     bucket_dir_autoconfig, NULL,            true,  CMD_EVENT_REINIT },
   { "Downloading databases...",
     FILE_PATH_DATABASE_RDB_ZIP,
     MENU_ENUM_LABEL_CB_UPDATE_DATABASES,
     bucket_dir_databases,  NULL,            false, CMD_EVENT_NONE },
   { "Downloading overlays...",
     FILE_PATH_OVERLAYS_ZIP,
     MENU_ENUM_LABEL_CB_UPDATE_OVERLAYS,
     bucket_dir_overlays,   NULL,            false, CMD_EVENT_NONE },
   /* Slang only — cg/glsl excluded per request.  Vulkan/D3D backends
    * use slang; the legacy GL2 path uses glsl, which we don't ship. */
   { "Downloading shaders...",
     FILE_PATH_SHADERS_SLANG_ZIP,
     MENU_ENUM_LABEL_CB_UPDATE_SHADERS_SLANG,
     bucket_dir_video_shader, "shaders_slang", false, CMD_EVENT_NONE }
};
#define PASTIME_SETUP_BUCKET_COUNT \
   ((size_t)(sizeof(k_buckets) / sizeof(k_buckets[0])))

/* ---------- state ---------- */

typedef struct
{
   enum pastime_setup_phase phase;
   size_t cores_total;        /* planned core count (filtered + deduped) */
   /* Stashed ident list for the deferred start().  Copies of caller's
    * strings; freed when start() fires (handed to cores module) or on
    * a re-plan. */
   char **planned_idents;
   size_t planned_count;
   /* Indices into k_buckets[] of buckets we'll actually run (those whose
    * install dir is missing or empty).  Length = bucket_count. */
   size_t buckets[PASTIME_SETUP_BUCKET_COUNT];
   size_t bucket_count;
   size_t bucket_cursor;      /* 0..bucket_count; advances on completion */
   bool   bucket_in_flight;   /* true between push and decompress callback */
   bool   cancelled;
} setup_state_t;

static setup_state_t g_setup;

/* ---------- helpers ---------- */

/* True iff the dir exists and contains at least one entry (any kind).
 * Used to decide whether to skip a bucket — first-run heuristic.  We
 * intentionally accept "non-empty" rather than "fully populated"; a
 * partial install gets treated as installed.  Re-running buckets is a
 * separate, user-initiated operation (PLAN follow-up). */
static bool pastime_setup_dir_populated(const char *dir)
{
   struct string_list *list;
   bool                populated;

   if (!dir || !*dir || !path_is_directory(dir))
      return false;
   /* include_dirs=true: autoconfig (and similar) lay out per-driver
    * subdirs, so the top level may have zero files but be effectively
    * populated.  Counting subdirs catches that. */
   list = dir_list_new(dir, NULL, true /* dirs */, true /* hidden */,
         false, false);
   populated = (list && list->size > 0);
   if (list)
      string_list_free(list);
   return populated;
}

/* Forward decls — bucket dispatch chain. */
static void pastime_setup_dispatch_next_bucket(void);
static void pastime_setup_bucket_http_cb(retro_task_t *task,
      void *task_data, void *user_data, const char *err);
static void pastime_setup_bucket_decompress_cb(retro_task_t *task,
      void *task_data, void *user_data, const char *err);

/* Mark the in-flight bucket finished and move to the next.  Called from
 * both the success path (decompress cb) and several error paths. */
static void pastime_setup_advance_bucket(void)
{
   g_setup.bucket_in_flight = false;
   g_setup.bucket_cursor++;
   if (g_setup.cancelled)
   {
      g_setup.phase = PASTIME_SETUP_DONE;
      return;
   }
   pastime_setup_dispatch_next_bucket();
}

/* Push the http+decompress chain for k_buckets[g_setup.buckets[cursor]].
 * On any push failure we log and advance to the next bucket. */
static void pastime_setup_dispatch_next_bucket(void)
{
   const pastime_setup_bucket_t *b;
   settings_t                    *settings = config_get_ptr();
   const char                    *base_url;
   char                           url[PATH_MAX_LENGTH];
   char                           url_enc[PATH_MAX_LENGTH];
   file_transfer_t               *transf;

   if (g_setup.bucket_cursor >= g_setup.bucket_count)
   {
      g_setup.phase = PASTIME_SETUP_DONE;
      RARCH_LOG("[Pastime-setup] all buckets complete\n");
      return;
   }

   b        = &k_buckets[g_setup.buckets[g_setup.bucket_cursor]];
   base_url = settings ? settings->paths.network_buildbot_assets_url : NULL;
   if (!base_url || !*base_url)
   {
      RARCH_WARN("[Pastime-setup] no buildbot assets URL; skipping %s\n",
            b->zip_filename);
      g_setup.bucket_cursor++;
      pastime_setup_dispatch_next_bucket();
      return;
   }

   /* Path layout under the assets buildbot is "<base>/frontend/<file>"
    * for every non-core bucket — see action_ok_download_generic. */
   {
      char tmp[PATH_MAX_LENGTH];
      fill_pathname_join_special(tmp, base_url, "frontend", sizeof(tmp));
      fill_pathname_join_special(url, tmp, b->zip_filename, sizeof(url));
   }
   net_http_urlencode_full(url_enc, url, sizeof(url_enc));

   transf = (file_transfer_t*)calloc(1, sizeof(*transf));
   if (!transf)
   {
      RARCH_ERR("[Pastime-setup] OOM allocating transfer; skipping %s\n",
            b->zip_filename);
      g_setup.bucket_cursor++;
      pastime_setup_dispatch_next_bucket();
      return;
   }
   transf->enum_idx = b->enum_idx;
   strlcpy(transf->path, b->zip_filename, sizeof(transf->path));

   RARCH_LOG("[Pastime-setup] downloading %s\n", b->zip_filename);
   g_setup.bucket_in_flight = true;
   if (!task_push_http_transfer_file(url_enc, true /* mute */,
         msg_hash_to_str(b->enum_idx),
         pastime_setup_bucket_http_cb, transf))
   {
      RARCH_WARN("[Pastime-setup] http push refused for %s\n",
            b->zip_filename);
      free(transf);
      g_setup.bucket_in_flight = false;
      g_setup.bucket_cursor++;
      pastime_setup_dispatch_next_bucket();
   }
}

/* Mirrors cb_generic_download (menu_cbs_ok.c) but tailored to our
 * bucket table.  Writes the downloaded ZIP to <dir>/<file>, then pushes
 * a decompress task and registers our completion hook. */
static void pastime_setup_bucket_http_cb(retro_task_t *task,
      void *task_data, void *user_data, const char *err)
{
   const pastime_setup_bucket_t *b;
   settings_t                    *settings = config_get_ptr();
   file_transfer_t               *transf   = (file_transfer_t*)user_data;
   http_transfer_data_t          *data     = (http_transfer_data_t*)task_data;
   const char                    *dir_path;
   char                           output_path[PATH_MAX_LENGTH];
   char                           subdir_buf[PATH_MAX_LENGTH];
   const char                    *subdir   = NULL;
   void                          *frontend_userdata;
   retro_task_t                  *decompress_task;

   if (g_setup.bucket_cursor >= g_setup.bucket_count)
   {
      /* Cancelled mid-flight or state otherwise reset.  Drop result. */
      if (transf)
         free(transf);
      return;
   }

   b = &k_buckets[g_setup.buckets[g_setup.bucket_cursor]];

   if (err && *err)
   {
      RARCH_WARN("[Pastime-setup] http failed for %s: %s\n",
            b->zip_filename, err);
      goto fail;
   }
   if (!data || !data->data || !transf)
   {
      RARCH_WARN("[Pastime-setup] http empty for %s\n", b->zip_filename);
      goto fail;
   }

   {
      /* Stack-local; the RA task system is single-threaded (callbacks
       * fire on the main thread via task_queue_check), so a static
       * isn't load-bearing — and `dir_path` is consumed before this
       * function returns, so the buffer doesn't need to outlive it. */
      char dir_buf[PATH_MAX_LENGTH];
      if (!pastime_setup_resolve_dir(b, settings, dir_buf, sizeof(dir_buf)))
      {
         RARCH_WARN("[Pastime-setup] no install dir for %s\n",
               b->zip_filename);
         goto fail;
      }
      dir_path = dir_buf;
   }
   /* Ensure the leaf install dir exists when a subpath is in play —
    * decompress will fail to extract into a missing dir.  For non-
    * subpath buckets the path_basedir/path_mkdir below covers it. */
   if (b->dir_subpath && !path_is_directory(dir_path)
         && !path_mkdir(dir_path))
   {
      RARCH_WARN("[Pastime-setup] mkdir failed: %s\n", dir_path);
      goto fail;
   }

   /* Joypad-driver subdir for autoconfig — mirrors cb_generic_download.
    * Empty/single-driver case still gets a trailing '|' so the decompress
    * task uses subdir-aware extraction. */
   if (b->use_joypad_subdir)
   {
      const char *opts = config_get_joypad_driver_options();
      if (opts && *opts)
      {
         size_t n = strlcpy(subdir_buf, opts, sizeof(subdir_buf));
         strlcpy(subdir_buf + n, "|", sizeof(subdir_buf) - n);
         subdir = subdir_buf;
      }
   }

   fill_pathname_join_special(output_path, dir_path, transf->path,
         sizeof(output_path));

   /* Ensure parent dir exists.  path_basedir_wrapper mutates output_path,
    * so we re-join after. */
   path_basedir_wrapper(output_path);
   if (!path_mkdir(output_path))
   {
      RARCH_WARN("[Pastime-setup] mkdir failed: %s\n", output_path);
      goto fail;
   }
   fill_pathname_join_special(output_path, dir_path, transf->path,
         sizeof(output_path));

   if (!filestream_write_file(output_path, data->data, data->len))
   {
      RARCH_WARN("[Pastime-setup] write failed: %s\n", output_path);
      goto fail;
   }

   /* Hand frontend_userdata across to the decompress task — the upstream
    * cb_generic_download does this dance to keep async progress reporting
    * consistent.  We zero our task's slot before the push to avoid a
    * double-finalize. */
   frontend_userdata       = task ? task->frontend_userdata : NULL;
   if (task)
      task->frontend_userdata = NULL;

   decompress_task = (retro_task_t*)task_push_decompress(
         output_path,
         dir_path,
         NULL /* target_file */,
         subdir,
         NULL /* valid_ext */,
         pastime_setup_bucket_decompress_cb,
         (void*)(uintptr_t)b->enum_idx,
         frontend_userdata,
         true /* mute */);

   if (!decompress_task)
   {
      RARCH_WARN("[Pastime-setup] decompress push failed for %s\n",
            output_path);
      /* We already moved frontend_userdata off the http task; if the
       * decompress task didn't take ownership, restore it so the http
       * task's normal finalizer cleans it up.  Without this restore
       * the userdata leaks. */
      if (task)
         task->frontend_userdata = frontend_userdata;
      goto fail;
   }

   /* transf is freed by the http task itself once we return — only the
    * decompress task is in flight now, with our cb registered. */
   free(transf);
   return;

fail:
   if (transf)
      free(transf);
   pastime_setup_advance_bucket();
}

static void pastime_setup_bucket_decompress_cb(retro_task_t *task,
      void *task_data, void *user_data, const char *err)
{
   decompress_task_data_t *dec      = (decompress_task_data_t*)task_data;
   unsigned                enum_idx = (unsigned)(uintptr_t)user_data;
   const pastime_setup_bucket_t *b = NULL;
   size_t                  i;

   (void)task;

   if (err && *err)
      RARCH_WARN("[Pastime-setup] decompress failed: %s\n", err);

   /* Find bucket by enum_idx.  Cursor isn't reliable here — by the time
    * this fires we may already have advanced (shouldn't happen with our
    * single-flight model but defensive against re-entry). */
   for (i = 0; i < PASTIME_SETUP_BUCKET_COUNT; i++)
   {
      if ((unsigned)k_buckets[i].enum_idx == enum_idx)
      {
         b = &k_buckets[i];
         break;
      }
   }

   if (b && (!err || !*err) && b->post_install_cmd != CMD_EVENT_NONE)
      command_event(b->post_install_cmd, NULL);

   /* Delete the staged ZIP — matches cb_decompressed's cleanup. */
   if (dec)
   {
      if (dec->source_file)
      {
         if (filestream_exists(dec->source_file))
            filestream_delete(dec->source_file);
         free(dec->source_file);
      }
      free(dec);
   }

   pastime_setup_advance_bucket();
}

/* ---------- public API ---------- */

static void pastime_setup_clear_planned_idents(void)
{
   size_t i;
   if (!g_setup.planned_idents)
      return;
   for (i = 0; i < g_setup.planned_count; i++)
      free(g_setup.planned_idents[i]);
   free(g_setup.planned_idents);
   g_setup.planned_idents = NULL;
   g_setup.planned_count  = 0;
}

void pastime_setup_plan_boot(const char * const *core_idents,
      size_t core_count)
{
   settings_t *settings = config_get_ptr();
   size_t      i;
   size_t      cores_total = 0;

   /* Refuse re-plan while running — would orphan in-flight callbacks. */
   if (g_setup.phase == PASTIME_SETUP_RUNNING)
   {
      RARCH_WARN("[Pastime-setup] plan_boot ignored — already running\n");
      return;
   }
   pastime_setup_clear_planned_idents();
   memset(&g_setup, 0, sizeof(g_setup));
   g_setup.phase = PASTIME_SETUP_INACTIVE;

   /* Filter idents locally — same dedupe + already-installed check the
    * cores module would do, but without firing the network.  We stash
    * the surviving idents so start() can hand them to cores_begin_boot. */
   if (core_idents && core_count > 0)
   {
      g_setup.planned_idents = (char**)calloc(core_count, sizeof(char*));
      if (g_setup.planned_idents)
      {
         for (i = 0; i < core_count; i++)
         {
            const char *ident = core_idents[i];
            size_t      j;
            bool        seen  = false;
            if (!ident || !*ident)
               continue;
            if (pastime_cores_is_installed(ident))
               continue;
            for (j = 0; j < cores_total; j++)
            {
               if (strcmp(g_setup.planned_idents[j], ident) == 0)
               { seen = true; break; }
            }
            if (seen)
               continue;
            if (!(g_setup.planned_idents[cores_total] = strdup(ident)))
               continue;
            cores_total++;
         }
         g_setup.planned_count = cores_total;
      }
   }
   g_setup.cores_total = cores_total;

   /* Bucket queue — same per-boot first-run check as before. */
   for (i = 0; i < PASTIME_SETUP_BUCKET_COUNT; i++)
   {
      char dir[PATH_MAX_LENGTH];
      if (!pastime_setup_resolve_dir(&k_buckets[i], settings,
            dir, sizeof(dir)))
         continue;
      if (pastime_setup_dir_populated(dir))
      {
         RARCH_LOG("[Pastime-setup] %s already populated; skipping\n",
               k_buckets[i].zip_filename);
         continue;
      }
      g_setup.buckets[g_setup.bucket_count++] = i;
   }

   if (cores_total == 0 && g_setup.bucket_count == 0)
   {
      g_setup.phase = PASTIME_SETUP_DONE;
      pastime_setup_clear_planned_idents();
      return;
   }

   g_setup.phase = PASTIME_SETUP_PLANNED;
}

void pastime_setup_start(void)
{
   if (g_setup.phase != PASTIME_SETUP_PLANNED)
      return;

   /* Hand the stashed idents off to cores.  cores_begin_boot_setup
    * makes its own copies, so we can free ours after. */
   if (g_setup.planned_count > 0)
      pastime_cores_begin_boot_setup(
            (const char * const *)g_setup.planned_idents,
            g_setup.planned_count);
   pastime_setup_clear_planned_idents();

   g_setup.phase = PASTIME_SETUP_RUNNING;
}

void pastime_setup_begin_boot(const char * const *core_idents,
      size_t core_count)
{
   pastime_setup_plan_boot(core_idents, core_count);
   if (g_setup.phase == PASTIME_SETUP_PLANNED)
      pastime_setup_start();
}

size_t pastime_setup_planned_core_count(void)
{
   return g_setup.cores_total;
}

size_t pastime_setup_planned_bucket_count(void)
{
   return g_setup.bucket_count;
}

void pastime_setup_pump(void)
{
   enum pastime_cores_state cs;

   if (g_setup.phase != PASTIME_SETUP_RUNNING)
      return;

   /* Drive cores first.  Once it lands in DONE/INACTIVE, kick buckets
    * (single-shot — the bucket_in_flight guard prevents re-dispatch). */
   pastime_cores_pump();
   cs = pastime_cores_get_state();
   if (cs == PASTIME_CORES_AWAITING_LIST
         || cs == PASTIME_CORES_INSTALLING)
      return;

   if (g_setup.bucket_count == 0)
   {
      g_setup.phase = PASTIME_SETUP_DONE;
      return;
   }

   if (!g_setup.bucket_in_flight
         && g_setup.bucket_cursor < g_setup.bucket_count)
      pastime_setup_dispatch_next_bucket();

   if (g_setup.bucket_cursor >= g_setup.bucket_count
         && !g_setup.bucket_in_flight)
      g_setup.phase = PASTIME_SETUP_DONE;
}

enum pastime_setup_phase pastime_setup_get_phase(void)
{
   return g_setup.phase;
}

bool pastime_setup_get_progress(size_t *out_total,
      size_t *out_done,
      const char **out_phase_label,
      const char **out_item_label)
{
   size_t      cores_done  = 0;
   size_t      cores_total = g_setup.cores_total;
   const char *core_ident  = NULL;
   size_t      total;
   size_t      done;

   if (g_setup.phase == PASTIME_SETUP_INACTIVE)
   {
      if (out_total)        *out_total        = 0;
      if (out_done)         *out_done         = 0;
      if (out_phase_label)  *out_phase_label  = NULL;
      if (out_item_label)   *out_item_label   = NULL;
      return false;
   }

   core_ident = pastime_cores_get_progress(&cores_done, NULL);
   total      = cores_total + g_setup.bucket_count;
   if (total == 0)
      total   = 1; /* avoid div-by-zero in caller */

   if (cores_done > cores_total)
      cores_done = cores_total;

   done = cores_done + g_setup.bucket_cursor;
   if (done > total)
      done = total;

   if (out_total) *out_total = total;
   if (out_done)  *out_done  = done;

   if (out_phase_label)
      *out_phase_label = NULL;
   if (out_item_label)
      *out_item_label = NULL;

   if (g_setup.phase == PASTIME_SETUP_DONE)
      return true;

   /* Phase label = cores while cores aren't done yet, else current
    * bucket.  Item label = current core ident during cores phase. */
   {
      enum pastime_cores_state cs = pastime_cores_get_state();
      if (cs == PASTIME_CORES_AWAITING_LIST
            || cs == PASTIME_CORES_INSTALLING)
      {
         if (out_phase_label) *out_phase_label = "Downloading cores...";
         if (out_item_label)  *out_item_label  = core_ident;
      }
      else if (g_setup.bucket_cursor < g_setup.bucket_count)
      {
         const pastime_setup_bucket_t *b =
               &k_buckets[g_setup.buckets[g_setup.bucket_cursor]];
         if (out_phase_label) *out_phase_label = b->phase_label;
      }
   }
   return true;
}

void pastime_setup_cancel(void)
{
   if (g_setup.phase == PASTIME_SETUP_PLANNED)
   {
      pastime_setup_clear_planned_idents();
      g_setup.phase = PASTIME_SETUP_DONE;
      return;
   }
   if (g_setup.phase != PASTIME_SETUP_RUNNING)
      return;
   g_setup.cancelled = true;
   pastime_cores_cancel();
   /* Bucket-phase cancellation: the in-flight task (if any) finishes;
    * our http/decompress cbs short-circuit via the cursor check. */
   g_setup.bucket_cursor = g_setup.bucket_count;
   g_setup.phase         = PASTIME_SETUP_DONE;
}
