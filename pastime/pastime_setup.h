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

#ifndef PASTIME_SETUP_H
#define PASTIME_SETUP_H

#include <stddef.h>
#include <boolean.h>
#include <retro_common_api.h>

RETRO_BEGIN_DECLS

enum pastime_setup_phase
{
   PASTIME_SETUP_INACTIVE = 0,
   PASTIME_SETUP_PLANNED,   /* something to do, awaiting user ack */
   PASTIME_SETUP_RUNNING,
   PASTIME_SETUP_DONE
};

/* Plan a boot pass without committing.  Determines which cores need
 * installing (deduped + filtered against locally-installed) and which
 * content buckets are unpopulated (assets, autoconfig, databases,
 * overlays, core-info, slang shaders — cheats skipped per PLAN).
 *
 * If anything would run, lands in PLANNED and stashes the ident list
 * for pastime_setup_start to consume.  If nothing would run, lands
 * in DONE — caller can skip the welcome screen. */
void pastime_setup_plan_boot(const char * const *core_idents,
      size_t core_count);

/* Transition from PLANNED to RUNNING — fires the buildbot core-list
 * fetch and starts the actual download chain.  No-op if not PLANNED. */
void pastime_setup_start(void);

/* Combined plan + start in one call.  Used by the lazy-install flow
 * (user picked a ROM whose core isn't installed) where the action
 * is already user-initiated and a welcome screen would be redundant. */
void pastime_setup_begin_boot(const char * const *core_idents,
      size_t core_count);

/* Number of cores + buckets we plan to download.  Available once
 * phase >= PLANNED.  Used by the welcome screen to summarize what
 * the user is about to commit to. */
size_t pastime_setup_planned_core_count(void);
size_t pastime_setup_planned_bucket_count(void);

/* Drive one frame of the state machine.  Cheap; safe to call every
 * frame.  Pumps cores, then dispatches the next bucket once cores
 * settle, then transitions to DONE. */
void pastime_setup_pump(void);

enum pastime_setup_phase pastime_setup_get_phase(void);

/* Progress query.  Out-params describe the *segments* model: every core
 * is one segment, every bucket is one segment.
 *   out_total      : total segment count for this pass (>= 1).
 *   out_done       : completed segments so far.
 *   out_phase_label: short human label for the active phase
 *                    ("Downloading cores...", "Downloading assets...", ...).
 *                    NULL when nothing is in flight.
 *   out_item_label : optional finer label (current core ident; NULL for
 *                    bucket phases since each bucket is one item).
 * Returns true when there's something meaningful to render. */
bool pastime_setup_get_progress(size_t *out_total,
      size_t *out_done,
      const char **out_phase_label,
      const char **out_item_label);

/* Skip the rest of the queue; jump to DONE.  In-flight tasks finish but
 * their results are ignored for state-machine purposes. */
void pastime_setup_cancel(void);

RETRO_END_DECLS

#endif
