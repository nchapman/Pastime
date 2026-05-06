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

#ifndef PASTIME_CORES_H
#define PASTIME_CORES_H

#include <stddef.h>
#include <boolean.h>
#include <retro_common_api.h>

RETRO_BEGIN_DECLS

/* State machine for boot-time core install. */
enum pastime_cores_state
{
   PASTIME_CORES_INACTIVE = 0,  /* nothing in flight; no splash needed */
   PASTIME_CORES_AWAITING_LIST, /* buildbot list fetch is pending */
   PASTIME_CORES_INSTALLING,    /* downloading cores one at a time */
   PASTIME_CORES_DONE           /* all done (or cancelled); splash dismissed */
};

/* True iff the named core is currently installed locally.  Resolves
 * "<ident>_libretro" through core_info_find. */
bool pastime_cores_is_installed(const char *core_ident);

/* Begin the boot install pass: dedupe idents, drop the ones already
 * installed, and (if any remain) kick off the buildbot list fetch.
 * Idempotent — calling this while a previous pass is in flight cancels
 * the previous one first.  The idents array and its strings may be freed
 * by the caller after this returns; the module makes its own copies. */
void pastime_cores_begin_boot_setup(const char *const *idents, size_t count);

/* Drive one frame of the state machine.  Cheap; safe to call every frame.
 * AWAITING_LIST → INSTALLING happens here once the list arrives.
 * INSTALLING → INSTALLING and INSTALLING → DONE happen in the download
 * completion callback. */
void pastime_cores_pump(void);

enum pastime_cores_state pastime_cores_get_state(void);

/* While INSTALLING, fills out_done / out_total and returns the ident
 * currently downloading.  Returns NULL when not installing. */
const char *pastime_cores_get_progress(size_t *out_done, size_t *out_total);

/* Abort: skip the rest of the queue and jump straight to DONE.  Any
 * download already in flight runs to completion (the underlying task
 * doesn't expose cancellation), but its result is discarded. */
void pastime_cores_cancel(void);

RETRO_END_DECLS

#endif
