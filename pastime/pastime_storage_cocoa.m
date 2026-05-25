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

#import <Cocoa/Cocoa.h>
#include "pastime_storage.h"

void pastime_cocoa_show_directory_picker(void)
{
   NSOpenPanel *panel = [NSOpenPanel openPanel];
   panel.title = @"Choose Pastime Storage Location";
   panel.canChooseDirectories = YES;
   panel.canChooseFiles = NO;
   panel.allowsMultipleSelection = NO;
   panel.canCreateDirectories = YES;

   if ([panel runModal] == NSModalResponseOK)
   {
      const char *path = [[panel.URL path] UTF8String];
      pastime_storage_on_picked(path);
   }
   else
      pastime_storage_on_cancelled();
}
