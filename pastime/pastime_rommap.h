#ifndef PASTIME_ROMMAP_H
#define PASTIME_ROMMAP_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct pastime_rommap pastime_rommap_t;

/* Load a ROM name map from a tab-delimited file.
 * Returns NULL on failure (missing file, empty, OOM). */
pastime_rommap_t *pastime_rommap_load(const char *path);

/* Load from an already-read buffer (takes ownership of buf).
 * buf must be heap-allocated; the map patches it in place and
 * frees it on pastime_rommap_free. */
pastime_rommap_t *pastime_rommap_load_buf(char *buf, size_t len);

/* Look up a filename (with extension, e.g. "mslug.zip").
 * Returns the display name or NULL.  Pointer is valid until
 * the map is freed.  Case-sensitive. */
const char *pastime_rommap_lookup(const pastime_rommap_t *map,
      const char *filename);

/* Number of entries in the map. */
size_t pastime_rommap_count(const pastime_rommap_t *map);

/* Free all storage.  Safe to call with NULL. */
void pastime_rommap_free(pastime_rommap_t *map);

/* Given a core_ident (e.g. "fbneo"), return the baked map
 * filename it routes to (e.g. "arcade.txt"), or NULL if
 * the core has no baked map. */
const char *pastime_rommap_route(const char *core_ident);

#ifdef __cplusplus
}
#endif

#endif /* PASTIME_ROMMAP_H */
