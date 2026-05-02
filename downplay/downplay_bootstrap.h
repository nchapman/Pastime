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

#ifndef DOWNPLAY_BOOTSTRAP_H
#define DOWNPLAY_BOOTSTRAP_H

#include <retro_common_api.h>

RETRO_BEGIN_DECLS

/* Ensure the on-disk Downplay/{Roms,Bios,Saves,States} layout exists
 * under the resolved Downplay/ root, and drop a README.txt in Roms/
 * explaining the "Display Name (corename)" convention.  Idempotent;
 * safe to call on every launch. */
void downplay_bootstrap(void);

RETRO_END_DECLS

#endif
