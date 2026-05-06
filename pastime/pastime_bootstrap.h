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

#ifndef PASTIME_BOOTSTRAP_H
#define PASTIME_BOOTSTRAP_H

#include <retro_common_api.h>

RETRO_BEGIN_DECLS

/* Ensure the on-disk Pastime/{Roms,Bios,Saves,States} layout exists
 * under the resolved Pastime/ root, and drop a README.txt in Roms/
 * explaining the "Display Name (corename)" convention.  Idempotent;
 * safe to call on every launch. */
void pastime_bootstrap(void);

RETRO_END_DECLS

#endif
