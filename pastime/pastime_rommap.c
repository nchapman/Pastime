#include "pastime_rommap.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifndef PASTIME_ROMMAP_TEST_BUILD
#include <streams/file_stream.h>
#endif

typedef struct
{
   uint32_t key_off;
   uint32_t val_off;
} rommap_entry_t;

struct pastime_rommap
{
   char           *buf;
   size_t          buf_len;
   rommap_entry_t *entries;
   size_t          count;
};

static const struct
{
   const char *core_ident;
   const char *map_filename;
} k_rommap_routes[] = {
   { "fbneo",              "arcade.txt" },
   { "mame2003_plus",      "arcade.txt" },
   { "mame2010",           "arcade.txt" },
   { "mame",              "arcade.txt" },
   { "fbalpha2012",        "arcade.txt" },
   { "fbalpha2012_neogeo", "arcade.txt" },
   { "fbalpha",            "arcade.txt" },
   { "mame2003",           "arcade.txt" },
   { "mame2000",           "arcade.txt" },
};

/* File-scope variable to pass buf pointer into qsort comparison
 * function during load (no portable qsort_r).  Only written/read
 * on the main thread during pastime_rommap_load_buf. */
static const char *s_rommap_buf;

static int rommap_entry_cmp(const void *a, const void *b)
{
   const rommap_entry_t *ea = (const rommap_entry_t *)a;
   const rommap_entry_t *eb = (const rommap_entry_t *)b;
   return strcmp(s_rommap_buf + ea->key_off,
                 s_rommap_buf + eb->key_off);
}

typedef struct
{
   const char *needle;
   const char *buf;
} rommap_key_t;

static int rommap_lookup_cmp(const void *key, const void *entry)
{
   const rommap_key_t   *k = (const rommap_key_t *)key;
   const rommap_entry_t *e = (const rommap_entry_t *)entry;
   return strcmp(k->needle, k->buf + e->key_off);
}

pastime_rommap_t *pastime_rommap_load_buf(char *buf, size_t len)
{
   pastime_rommap_t *map;
   size_t            i;
   size_t            count    = 0;
   size_t            cap      = 0;
   rommap_entry_t   *entries  = NULL;

   if (!buf || len == 0)
   {
      free(buf);
      return NULL;
   }

   /* First pass: count entries and NUL-patch delimiters in place. */
   i = 0;
   while (i < len)
   {
      uint32_t key_start = (uint32_t)i;
      uint32_t tab_pos   = 0;
      int      has_tab   = 0;

      /* Scan to end of line or buffer */
      while (i < len && buf[i] != '\n' && buf[i] != '\r')
      {
         if (!has_tab && buf[i] == '\t')
         {
            tab_pos = (uint32_t)i;
            has_tab = 1;
         }
         i++;
      }

      /* NUL-terminate at line end; handle \r\n */
      if (i < len)
      {
         int was_cr = (buf[i] == '\r');
         buf[i] = '\0';
         i++;
         if (was_cr && i < len && buf[i] == '\n')
            i++;
      }

      if (!has_tab)
         continue;
      /* Empty key check */
      if (tab_pos == key_start)
         continue;

      /* NUL-terminate key at the tab position */
      buf[tab_pos] = '\0';

      /* Empty value check */
      if (tab_pos + 1 >= len || buf[tab_pos + 1] == '\0')
         continue;

      if (count == cap)
      {
         size_t          new_cap = cap ? cap * 2 : 256;
         rommap_entry_t *grown   = (rommap_entry_t *)realloc(entries,
               new_cap * sizeof(*entries));
         if (!grown)
            break;
         entries = grown;
         cap     = new_cap;
      }
      entries[count].key_off = key_start;
      entries[count].val_off = tab_pos + 1;
      count++;
   }

   if (count == 0)
   {
      free(entries);
      free(buf);
      return NULL;
   }

   /* Right-size the array to release wasted capacity from geometric growth */
   if (count < cap)
   {
      rommap_entry_t *shrunk = (rommap_entry_t *)realloc(entries,
            count * sizeof(*entries));
      if (shrunk)
         entries = shrunk;
   }

   /* Sort by key for bsearch */
   s_rommap_buf = buf;
   qsort(entries, count, sizeof(*entries), rommap_entry_cmp);

   map = (pastime_rommap_t *)calloc(1, sizeof(*map));
   if (!map)
   {
      free(entries);
      free(buf);
      return NULL;
   }
   map->buf     = buf;
   map->buf_len = len;
   map->entries = entries;
   map->count   = count;
   return map;
}

pastime_rommap_t *pastime_rommap_load(const char *path)
{
#ifdef PASTIME_ROMMAP_TEST_BUILD
   (void)path;
   return NULL;
#else
   char  *buf = NULL;
   int64_t len = 0;

   if (!path || !*path)
      return NULL;
   if (!filestream_read_file(path, (void **)&buf, &len))
      return NULL;
   if (!buf || len <= 0)
   {
      free(buf);
      return NULL;
   }
   return pastime_rommap_load_buf(buf, (size_t)len);
#endif
}

const char *pastime_rommap_lookup(const pastime_rommap_t *map,
      const char *filename)
{
   rommap_entry_t *found;
   rommap_key_t    k;

   if (!map || !filename || !*filename || !map->entries)
      return NULL;

   k.needle = filename;
   k.buf    = map->buf;
   found = (rommap_entry_t *)bsearch(&k, map->entries,
         map->count, sizeof(*map->entries), rommap_lookup_cmp);
   if (!found)
      return NULL;
   return map->buf + found->val_off;
}

size_t pastime_rommap_count(const pastime_rommap_t *map)
{
   return map ? map->count : 0;
}

void pastime_rommap_free(pastime_rommap_t *map)
{
   if (!map)
      return;
   free(map->entries);
   free(map->buf);
   free(map);
}

const char *pastime_rommap_route(const char *core_ident)
{
   size_t i;
   if (!core_ident || !*core_ident)
      return NULL;
   for (i = 0; i < sizeof(k_rommap_routes) / sizeof(k_rommap_routes[0]); i++)
   {
      if (strcmp(core_ident, k_rommap_routes[i].core_ident) == 0)
         return k_rommap_routes[i].map_filename;
   }
   return NULL;
}
