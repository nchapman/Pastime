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

#ifndef __PASTIME_STORAGE_H
#define __PASTIME_STORAGE_H

#include <boolean.h>
#include <stddef.h>

enum pastime_storage_state
{
   PASTIME_STORAGE_IDLE = 0,
   PASTIME_STORAGE_AWAITING_PICK,
   PASTIME_STORAGE_PICKED,
   PASTIME_STORAGE_CANCELLED
};

/* Launch the platform folder picker.  Transitions state to AWAITING_PICK. */
void pastime_storage_show_picker(void);

/* Platform callbacks — called when the picker returns a result. */
void pastime_storage_on_picked(const char *filesystem_path);
void pastime_storage_on_cancelled(void);

/* Current picker state. */
enum pastime_storage_state pastime_storage_get_state(void);

/* Reset state back to IDLE (e.g. after handling CANCELLED). */
void pastime_storage_reset_state(void);

/* After PICKED: the chosen path.  Valid until next show_picker call. */
const char *pastime_storage_get_picked_path(void);

/* Persist the path to config, run bootstrap, re-apply path overlays. */
void pastime_storage_commit(const char *path);

/* True if the user has previously committed a storage root. */
bool pastime_storage_is_configured(void);

#endif
