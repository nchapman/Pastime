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

#ifndef PASTIME_DEFAULTS_H
#define PASTIME_DEFAULTS_H

#include <stddef.h>
#include <boolean.h>
#include <retro_common_api.h>

RETRO_BEGIN_DECLS

/* Resolve the on-disk Pastime/ root.  Writes the absolute path to out
 * and returns true on success.  Returns false (and leaves out untouched)
 * if no candidate root is available — caller should not assume the path
 * exists on disk; pair with pastime_bootstrap() to create it.
 *
 * Android: prefers internal_storage_path (the sdcard tier RA already
 * resolved at startup), falls back to internal_storage_app_path.
 * Other Unix: $HOME. */
bool pastime_paths_get_root(char *out, size_t out_len);

/* Overlay Pastime's opinionated defaults onto the loaded settings.
 * Called from retroarch_main_init after config_load() and before the
 * CLI second pass.  The menu_driver override is unconditional; path
 * settings are overlaid only when their current value is empty or
 * equals the upstream platform default (so an explicit user override
 * in retroarch.cfg is preserved). */
void pastime_defaults_apply(void);

/* One-shot flag: when set, the runloop auto-state gate (runloop.c)
 * loads the auto-save state for this content load, bypassing
 * settings->bools.savestate_auto_load (which defaults_apply keeps
 * false so A-button launches always start fresh).
 * Consumed immediately after the gate fires. */
void pastime_defaults_request_auto_load(void);
void pastime_defaults_cancel_auto_load(void);
bool pastime_defaults_should_auto_load(void);

RETRO_END_DECLS

#endif
