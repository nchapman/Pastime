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

/* See downplay_thumbs.h for intent.
 *
 * Module layout:
 *
 *   Section 1: index entries + sorted permutation arrays
 *     - dp_thumb_entry_t
 *     - region scoring
 *     - sort comparators
 *
 *   Section 2: JSON parse → downplay_thumbs_index_t
 *     - rjson event handlers populating a builder
 *
 *   Section 3: pure match cascade (T0..T4)
 *     - downplay_thumbs_index_match
 *
 *   Section 4: manager (open/close/request/prefetch/pump)
 *     - on-disk index TTL refresh
 *     - per-entry attempt-state tracking
 *     - HTTP task dispatch (index + image), filesystem polling
 *
 * Coding-guideline notes:
 *   - C89-style declarations at top of blocks.
 *   - Allman braces; single-statement bodies unbraced.
 *   - All RA-aware path/IO via libretro-common helpers.
 *   - `#ifdef DOWNPLAY_THUMBS_TEST_BUILD` swaps RA verbosity macros for
 *     stubs so the unit-test binary builds standalone (mirrors the
 *     pattern used by downplay_nav.c). */

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <time.h>
#include <sys/stat.h>
#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#endif

#include <boolean.h>
#include <compat/strl.h>
#include <file/file_path.h>
#include <formats/rjson.h>
#include <net/net_http.h>
#include <streams/file_stream.h>
#include <streams/trans_stream.h>
#include <string/stdstring.h>

#include "downplay_thumbs.h"
#include "downplay_display_name.h"

#ifndef DOWNPLAY_THUMBS_TEST_BUILD
#include <retro_assert.h>
#include <retro_miscellaneous.h>
#include <queues/task_queue.h>
#include "../verbosity.h"
#include "../msg_hash.h"
#include "../tasks/task_file_transfer.h"
#include "../tasks/tasks_internal.h"
#include "downplay_defaults.h"
#else
/* Test stubs — pure-string code paths only.  No HTTP, no log. */
#define RARCH_LOG(...)  ((void)0)
#define RARCH_WARN(...) ((void)0)
#define RARCH_ERR(...)  ((void)0)
#endif

/* Hard upper bound on parsed entries.  Real No-Intro / Redump
 * indexes are 5k–20k entries per system; 100k gives generous
 * headroom while preventing a hostile 256 MB index from OOM-killing
 * the launcher.  Also caps the binary-format `entry_count` field at
 * open time before we multiply it out — see dp_idx_open. */
#define DP_THUMBS_MAX_ENTRIES 100000u

/* On-disk binary index format.  Replaces gzipped JSON as the cached
 * representation: parse JSON once on download, emit a packed binary
 * file, then every subsequent open is a `pread` + 32-byte header
 * validation — no JSON, no allocations beyond the buffer itself.
 *
 * Layout (little-endian, 4-byte aligned sections):
 *
 *   [HEADER 32B]
 *     u32 magic           = 'DPTH'
 *     u32 version         = 1
 *     u32 entry_count
 *     u32 strings_size
 *     u32 entries_off     -- always 32
 *     u32 by_canonical_off
 *     u32 by_heavy_off
 *     u32 strings_off
 *
 *   [ENTRIES, 8 bytes each, original load order]
 *     u32 canonical_off   -- offset into string pool
 *     u32 heavy_off
 *
 *   [BY_CANONICAL, u32 entry indices, sorted by strcmp(canonical)]
 *
 *   [BY_HEAVY, {u32 hash, u32 entry_idx} pairs, sorted by
 *    (hash, then strcmp(heavy) for stability across collisions)]
 *
 *   [STRINGS, packed NUL-terminated string pool, dedup'd]
 *
 *   [FOOTER 8B]
 *     u32 magic_repeat
 *     u32 entry_count_repeat
 *
 * Tiebreak fields (region / rev / bad_dump / disc) are NOT stored —
 * they're derived from the canonical name on demand inside the
 * cascade.  Per-query cost is bounded (cascade walks 1-5 candidates;
 * helpers are ~50 ns each).  Net win: zero per-record metadata
 * overhead, no parse-time computation, single source of truth.
 *
 * Hash in BY_HEAVY (fnv1a-32 of the heavy string) lets bsearch
 * compare 32-bit ints on the hot path, only falling through to
 * strcmp on hash hits — collisions for 20k entries in 2^32 space
 * are statistically negligible but handled correctly via the
 * secondary strcmp key.
 *
 * Webp is always present on the server (we control it), so format
 * size fields are gone too — every image URL ends in `.webp`. */
#define DP_IDX_MAGIC      0x48545044u  /* 'D','P','T','H' LE */
#define DP_IDX_VERSION    1u
#define DP_IDX_HEADER_SZ  32u
#define DP_IDX_FOOTER_SZ  8u
#define DP_IDX_REC_SZ     8u           /* canonical_off + heavy_off */
#define DP_IDX_BCAN_SZ    4u           /* entry_idx */
#define DP_IDX_BHEV_SZ    8u           /* hash + entry_idx */

/* ---------------------------------------------------------------- */
/* Section 1: parse-time entry struct + normalization + tiebreaks   */
/* ---------------------------------------------------------------- */

/* Transient struct used during JSON parse only.  Does NOT survive
 * past `downplay_thumbs_index_parse` — we emit a binary buffer from
 * a temp array of these and free them.  All tiebreak data is
 * derived from `canonical` on demand at cascade time, which is why
 * this struct only carries the two strings the binary format needs
 * to persist. */
typedef struct
{
   char *canonical;     /* exactly the index.json key */
   char *heavy;         /* normalized form for the bsearch index */
} dp_thumb_entry_t;

/* Validate the id portion of a disc/cd/side tag — i.e. the bytes
 * after "(Disc " up to ')'.  Accept either:
 *   - 1-3 digits ("1", "2", "12")
 *   - a single letter A-Z / a-z ("A", "B")
 * Reject anything else, which kills false positives like "(CD ROM)"
 * and "(CD Audio)" that would otherwise capture "ROM" / "Audio" as
 * the disc id and corrupt tiebreak. */
static bool dp_disc_id_valid(const char *id, size_t len)
{
   size_t i;
   if (len == 0 || len > 3)
      return false;
   if (len == 1 && ((id[0] >= 'A' && id[0] <= 'Z')
                  || (id[0] >= 'a' && id[0] <= 'z')))
      return true;
   for (i = 0; i < len; i++)
   {
      if (id[i] < '0' || id[i] > '9')
         return false;
   }
   return true;
}

/* Extract a disc/cd/side disambiguator from a raw title.  Writes a
 * keyword-cased token (e.g. "Disc 2") to `out`.  Returns true on
 * hit.  Lives outside the heavy normalize because (Disc N) is the
 * only difference between disc 1 and disc 2 of a multi-disc release —
 * stripping it would collapse them to the same key, and we'd pick
 * Disc 1 every time.  Used only as a tiebreak field. */
static bool dp_extract_disc_token(const char *raw, char *out, size_t out_size)
{
   static const char * const keywords[] = { "Disc", "CD", "Side" };
   const size_t              n_kw       = sizeof(keywords) / sizeof(keywords[0]);
   const char *p;
   if (out_size > 0)
      out[0] = '\0';
   if (!raw)
      return false;
   for (p = raw; *p; p++)
   {
      size_t ki;
      if (*p != '(')
         continue;
      for (ki = 0; ki < n_kw; ki++)
      {
         size_t klen = strlen(keywords[ki]);
         if (strncasecmp(p + 1, keywords[ki], klen) != 0)
            continue;
         if (p[1 + klen] != ' ')
            continue;
         {
            const char *id_start = p + 1 + klen + 1;
            const char *end      = strchr(id_start, ')');
            size_t      id_len;
            size_t      total;
            if (!end)
               return false;
            id_len = (size_t)(end - id_start);
            if (!dp_disc_id_valid(id_start, id_len))
               continue; /* not a real disc tag — keep scanning */
            total = (size_t)(end - (p + 1));
            if (total + 1 > out_size)
               return false;
            memcpy(out, p + 1, total);
            out[total] = '\0';
            /* Canonicalise keyword case (preserve id verbatim). */
            memcpy(out, keywords[ki], klen);
            return true;
         }
      }
   }
   return false;
}

/* Extract Rev N from a title like "Foo (Rev 2)".  Returns the int,
 * or 0 if no numeric Rev tag.  Letter revs (Rev A/B/C) are returned
 * as 1/2/3 so they sort the same way numeric revs do.
 *
 * Clamped to [0, 99]: the score function uses rev_num as a tie-
 * breaker offset alongside region_score×100 (range 0..900); an
 * unbounded value here from a malformed/hostile entry would leak
 * into the region band and let `(Rev 1000)` outrank a USA release. */
static int dp_extract_rev(const char *raw)
{
   const char *p;
   int         v;
   if (!raw)
      return 0;
   for (p = raw; *p; p++)
   {
      if (*p != '(')
         continue;
      if (strncasecmp(p + 1, "Rev ", 4) != 0)
         continue;
      {
         const char *q = p + 5;
         while (*q == ' ')
            q++;
         if (*q >= '0' && *q <= '9')
         {
            v = atoi(q);
            if (v < 0)  v = 0;
            if (v > 99) v = 99;
            return v;
         }
         /* Letter rev: A=1, B=2, ... */
         if ((*q >= 'A' && *q <= 'Z'))
            return (int)(*q - 'A' + 1);
         if ((*q >= 'a' && *q <= 'z'))
            return (int)(*q - 'a' + 1);
      }
   }
   return 0;
}

/* Detect "bad dump" / non-final-release qualifiers.  A user pointing
 * at a clean ROM should never resolve to a Beta or Proto, so these
 * entries get filtered out at tiebreak time unless they're the only
 * candidates available.  List is the union of common No-Intro /
 * Redump / TOSEC tags with strong "this is not the canonical
 * release" semantics. */
static bool dp_detect_bad_dump(const char *raw)
{
   static const char * const flags[] =
   {
      "(Beta", "(Proto", "(Demo", "(Sample", "(Pirate",
      "(Unl)", "(Unlicensed", "(Hack", "(Aftermarket",
      "(Program)", "(Test Program", "(Bonus"
   };
   size_t fi;
   if (!raw)
      return false;
   for (fi = 0; fi < sizeof(flags) / sizeof(flags[0]); fi++)
   {
      const char *needle = flags[fi];
      size_t      nlen   = strlen(needle);
      const char *p;
      for (p = raw; *p; p++)
      {
         if (!strncasecmp(p, needle, nlen))
            return true;
      }
   }
   return false;
}

/* Roman → arabic for the small set we trust.  Skip I and X — both
 * collide with English usage / standalone titles ("Mega Man X").
 * Input must be lowercased.  Returns 0 if not a recognised roman
 * numeral, else the digit. */
static int dp_roman_to_int(const char *s, size_t len)
{
   if (len == 2 && !memcmp(s, "ii",   2)) return 2;
   if (len == 3 && !memcmp(s, "iii",  3)) return 3;
   if (len == 2 && !memcmp(s, "iv",   2)) return 4;
   if (len == 1 && s[0] == 'v')           return 5;
   if (len == 2 && !memcmp(s, "vi",   2)) return 6;
   if (len == 3 && !memcmp(s, "vii",  3)) return 7;
   if (len == 4 && !memcmp(s, "viii", 4)) return 8;
   if (len == 2 && !memcmp(s, "ix",   2)) return 9;
   return 0;
}

/* Fold a small set of Latin-1 / Latin-Extended UTF-8 sequences down
 * to ASCII so accented titles match unaccented ones.  Real-world
 * cases: "Pokémon" vs "Pokemon", "Café" vs "Cafe", "Doña" vs "Dona".
 *
 * Operates on UTF-8 bytes in place.  Recognised patterns are 2-byte
 * sequences in the Latin-1 Supplement range (0xC2 / 0xC3 lead byte)
 * + a few common ligatures; emits 1 or 2 ASCII bytes.  Unrecognised
 * bytes ≥ 0x80 are left untouched (the alphanumeric tokeniser will
 * later treat them as separators and drop them, which keeps the
 * output strictly ASCII regardless). */
static void dp_fold_latin(char *s)
{
   const unsigned char *r = (const unsigned char*)s;
   char                *w = s;
   while (*r)
   {
      const char *rep = NULL;
      char        rep_buf[3];
      size_t      consumed = 1;
      if (r[0] == 0xC3 && r[1])
      {
         /* Latin-1 Supplement upper half: À–ÿ. */
         unsigned char lo = r[1];
         consumed = 2;
         switch (lo)
         {
            case 0x80: case 0x81: case 0x82: case 0x83: case 0x84: case 0x85: /* ÀÁÂÃÄÅ */
            case 0xA0: case 0xA1: case 0xA2: case 0xA3: case 0xA4: case 0xA5: /* àáâãäå */
               rep = "a"; break;
            case 0x88: case 0x89: case 0x8A: case 0x8B: /* ÈÉÊË */
            case 0xA8: case 0xA9: case 0xAA: case 0xAB: /* èéêë */
               rep = "e"; break;
            case 0x8C: case 0x8D: case 0x8E: case 0x8F: /* ÌÍÎÏ */
            case 0xAC: case 0xAD: case 0xAE: case 0xAF: /* ìíîï */
               rep = "i"; break;
            case 0x92: case 0x93: case 0x94: case 0x95: case 0x96: /* ÒÓÔÕÖ */
            case 0xB2: case 0xB3: case 0xB4: case 0xB5: case 0xB6: /* òóôõö */
               rep = "o"; break;
            case 0x99: case 0x9A: case 0x9B: case 0x9C: /* ÙÚÛÜ */
            case 0xB9: case 0xBA: case 0xBB: case 0xBC: /* ùúûü */
               rep = "u"; break;
            case 0x91: case 0xB1: rep = "n"; break;        /* Ññ */
            case 0x87: case 0xA7: rep = "c"; break;        /* Çç */
            case 0x9F: rep = "ss"; break;                  /* ß */
            case 0x86: case 0xA6: rep = "ae"; break;       /* Ææ */
            case 0x98: case 0xB8: rep = "o"; break;        /* Øø */
            case 0xBE: case 0x9E: rep = "th"; break;       /* Þþ */
            default:
               break;
         }
      }
      else if (r[0] == 0xC5 && (r[1] == 0x93 || r[1] == 0x92))
      {
         /* Œœ */
         consumed = 2;
         rep = "oe";
      }
      else if (r[0] == 0xC2 && (r[1] == 0xA9 || r[1] == 0xAE))
      {
         /* © ® — strip silently. */
         consumed = 2;
         rep = "";
      }
      if (rep)
      {
         while (*rep)
            *w++ = *rep++;
         r += consumed;
         continue;
      }
      *w++ = (char)*r++;
      /* If this was the start of a multi-byte sequence we don't
       * recognise, fall through and emit each byte verbatim — the
       * tokeniser drops bytes ≥ 0x80 later anyway. */
      (void)rep_buf;
   }
   *w = '\0';
}

/* Strip every "(...)" and "[...]" block in `s` in place; same shape
 * as the brackets-strip in downplay_display_name.c but inlined here
 * to keep this module self-contained.  Doesn't handle nesting (No-
 * Intro / Redump don't nest). */
static void dp_strip_parens(char *s)
{
   char *r = s;
   char *w = s;
   while (*r)
   {
      if (*r == '(' || *r == '[')
      {
         char close = (*r == '(') ? ')' : ']';
         const char *p = r + 1;
         while (*p && *p != close)
            p++;
         if (*p == close)
         {
            r = (char*)p + 1;
            continue;
         }
         /* Unmatched opener: keep the rest verbatim. */
      }
      *w++ = *r++;
   }
   *w = '\0';
}

/* The single normalize pass.  Build a comparable form that absorbs
 * the most common naming variations:
 *
 *   1. Strip every (...) and [...] block (region/disc/rev/lang flags
 *      and ROM-set quality tags) — they only matter for tiebreak.
 *   2. Tokenise on alphanumeric runs.  For each token (lowercased):
 *        - drop connectives ("the"/"a"/"an"/"and")
 *        - convert recognised roman numerals → digit
 *        - else emit verbatim
 *   3. Concatenate emitted tokens with no separators.
 *
 * Non-ASCII bytes (™ © ® é etc.) and all punctuation (incl. & and _)
 * are silently dropped by the alphanumeric tokeniser. */
static void dp_normalize_heavy(const char *raw, char *out, size_t out_size)
{
   char        buf[768];
   size_t      o = 0;
   size_t      i;
   size_t      n;

   if (out_size == 0)
      return;
   out[0] = '\0';
   if (!raw || !*raw)
      return;

   /* Stage 1: copy + fold latin diacritics + strip parens/brackets.
    * Folding before paren-strip handles cases like "Café (USA)" →
    * "Cafe" before the paren goes; ordering vs paren-strip is
    * arbitrary since neither touches the other's targets. */
   strlcpy(buf, raw, sizeof(buf));
   dp_fold_latin(buf);
   dp_strip_parens(buf);

   /* No "&" → "and" substitution: the libretro-thumbnails mirror
    * sanitises filesystem-unsafe characters (& * / : ? < > \ | ") to
    * '_' in canonical keys, which our tokeniser already drops as a
    * non-alphanumeric separator.  Substituting "&" → " and " on the
    * user side would diverge from the index side ("Mario & Luigi"
    * vs canonical "Mario _ Luigi").  Treat both as separators and
    * drop "and" as a connective so "Tom & Jerry" still matches
    * "Tom and Jerry".  See dp_normalize_heavy article-drop below. */

   /* Stage 3: tokenise alphanumeric runs.  ASCII-only; non-ASCII bytes
    * are treated as separators (silently dropped). */
   n = strlen(buf);
   i = 0;
   while (i < n)
   {
      size_t start;
      size_t tlen;
      char   tok[64];
      int    rn;
      unsigned char c = (unsigned char)buf[i];
      if (c < 0x80 && !isalnum(c))
      {
         i++;
         continue;
      }
      start = i;
      while (i < n)
      {
         unsigned char d = (unsigned char)buf[i];
         if (d >= 0x80 || !isalnum(d))
            break;
         i++;
      }
      tlen = i - start;
      if (tlen >= sizeof(tok))
         tlen = sizeof(tok) - 1;
      {
         size_t k;
         for (k = 0; k < tlen; k++)
            tok[k] = (char)tolower((unsigned char)buf[start + k]);
         tok[tlen] = '\0';
      }

      /* Drop articles + "and".  "and" is dropped because the libretro
       * mirror substitutes '&' → '_' (filesystem sanitisation), which
       * tokenises as a separator on the index side; the user side
       * may have either "&" or "and" verbatim.  Dropping "and"
       * collapses all three forms ("Tom & Jerry" / "Tom _ Jerry" /
       * "Tom and Jerry") to the same key. */
      if (   (tlen == 3 && !memcmp(tok, "the", 3))
          || (tlen == 3 && !memcmp(tok, "and", 3))
          || (tlen == 1 && tok[0] == 'a')
          || (tlen == 2 && !memcmp(tok, "an", 2)))
         continue;

      /* Roman → arabic where safe. */
      rn = dp_roman_to_int(tok, tlen);
      if (rn > 0)
      {
         if (o + 1 >= out_size)
            break;
         out[o++] = (char)('0' + rn);
         continue;
      }

      /* Emit verbatim. */
      if (o + tlen >= out_size)
      {
         size_t avail = out_size - 1 - o;
         memcpy(out + o, tok, avail);
         o += avail;
         break;
      }
      memcpy(out + o, tok, tlen);
      o += tlen;
   }
   out[o] = '\0';
}

/* Region preference: walk the canonical key for parenthesised region
 * tokens and pick the lowest-scoring one we see.  Lower is better.
 * Only used for tiebreak when multiple entries share the same
 * `heavy` key. */
enum
{
   DP_REGION_USA   = 0,
   DP_REGION_EU    = 1,
   DP_REGION_WORLD = 2,
   DP_REGION_OTHER = 5,
   DP_REGION_JP    = 7,
   DP_REGION_NONE  = 9   /* No region tag at all. */
};

/* Token-boundary check: ',' or ')' or NUL.  Whitespace does NOT
 * terminate ("(USA Proto)" is not USA). */
static bool dp_is_region_terminator(char c)
{
   return c == '\0' || c == ',' || c == ')';
}

static int dp_score_region(const char *canonical)
{
   int  score = DP_REGION_NONE;
   const char *p = canonical;
   if (!p)
      return DP_REGION_NONE;
   while (*p)
   {
      if (*p == '(')
      {
         const char *q = p + 1;
         int local = DP_REGION_OTHER;
         while (*q && *q != ')')
         {
            while (*q == ' ' || *q == ',')
               q++;
            if (!strncasecmp(q, "USA", 3) && dp_is_region_terminator(q[3]))
            {
               if (DP_REGION_USA < local) local = DP_REGION_USA;
            }
            else if (!strncasecmp(q, "Europe", 6) && dp_is_region_terminator(q[6]))
            {
               if (DP_REGION_EU < local) local = DP_REGION_EU;
            }
            else if (!strncasecmp(q, "World", 5) && dp_is_region_terminator(q[5]))
            {
               if (DP_REGION_WORLD < local) local = DP_REGION_WORLD;
            }
            else if (!strncasecmp(q, "Japan", 5) && dp_is_region_terminator(q[5]))
            {
               if (DP_REGION_JP < local) local = DP_REGION_JP;
            }
            while (*q && *q != ',' && *q != ')')
               q++;
         }
         if (local < score)
            score = local;
         if (*q == ')')
            p = q + 1;
         else
            break;
         continue;
      }
      p++;
   }
   return score;
}

/* ---------------------------------------------------------------- */
/* Section 2: JSON parse → temp dp_parse_index_t                    */
/* ---------------------------------------------------------------- */

/* Parse-time temporary index.  Lives only across one
 * `downplay_thumbs_index_parse` call: rjson handlers populate it,
 * then dp_idx_emit_buffer serialises it to the on-disk binary
 * format and frees it.  The public-API `downplay_thumbs_index_t`
 * (defined later) wraps the resulting binary buffer. */
typedef struct
{
   dp_thumb_entry_t *entries;
   size_t            entries_count;
   size_t            entries_cap;
} dp_parse_index_t;

/* JSON parse state machine.  The input shape:
 *   { "system": "...", "image_type": "...", "files": {
 *       "Title (Region)": { "formats": { ... }, ... },
 *       ...
 *     }
 *   }
 *
 * We only care about each key inside "files" (= entry canonical).
 * The legacy "formats.{jpg,webp}" sub-objects are still parsed past
 * — to stay in sync with rjson's depth tracking — but their values
 * are discarded since the binary format always assumes webp. */
typedef enum
{
   DP_PS_TOPLEVEL = 0, /* inside outer { ... } */
   DP_PS_AT_FILES,     /* inside "files": { ... } — keys here are titles */
   DP_PS_IN_ENTRY,     /* inside one entry object */
   DP_PS_IN_FORMATS,   /* inside entry.formats */
   DP_PS_OTHER         /* skipping irrelevant subtree */
} dp_parse_state_t;

typedef struct
{
   dp_parse_index_t *idx;

   dp_parse_state_t state_stack[16];
   int              depth;            /* current depth, 0 == before root */

   /* Accumulator for the entry currently being parsed.  NULL between
    * entries; strdup'd while we're inside one. */
   char *cur_canonical;

   /* Last seen object-member name; consumed by the next value handler. */
   char  last_member[256];

   bool  in_files;     /* did we see "files" at top level? */
   bool  saw_files_obj;

   bool  oom;
} dp_parse_ctx_t;

static bool dp_idx_grow(dp_parse_index_t *idx)
{
   size_t new_cap;
   dp_thumb_entry_t *e;
   if (idx->entries_cap >= DP_THUMBS_MAX_ENTRIES)
      return false;
   new_cap = idx->entries_cap ? idx->entries_cap * 2 : 256;
   if (new_cap > DP_THUMBS_MAX_ENTRIES)
      new_cap = DP_THUMBS_MAX_ENTRIES;
   e = (dp_thumb_entry_t*)realloc(idx->entries,
         new_cap * sizeof(*e));
   if (!e)
      return false;
   idx->entries     = e;
   idx->entries_cap = new_cap;
   return true;
}

/* Append one entry to the parse-time temp array.  `canonical` and
 * `heavy` are strdup'd.  Tiebreak metadata (region/rev/bad_dump/disc)
 * is NOT pre-computed — the binary format doesn't store it and the
 * cascade derives it on demand at query time.  `heavy` is supplied
 * by the caller so alt-name segments can reuse the normalize buffer
 * without re-running it on the same canonical.  Returns false on
 * OOM (caller propagates). */
static bool dp_append_entry(dp_parse_index_t *idx,
      const char *canonical, const char *heavy, bool *oom)
{
   dp_thumb_entry_t *e;
   if (idx->entries_count == idx->entries_cap && !dp_idx_grow(idx))
   {
      *oom = true;
      return false;
   }
   e = &idx->entries[idx->entries_count];
   e->canonical = strdup(canonical);
   e->heavy     = strdup(heavy);
   if (!e->canonical || !e->heavy)
   {
      free(e->canonical);
      free(e->heavy);
      *oom = true;
      return false;
   }
   idx->entries_count++;
   return true;
}

/* Commit cur_canonical as one or more entries.  Most canonicals
 * produce a single entry.  Canonicals that contain ` _ ` (libretro's
 * alt-name separator after & or `/` sanitization) are recognised as
 * multi-name bundles like
 *
 *   "F-16 Fighting Falcon _ F-16 Fighter _ F16 Falcon Fighter (USA)"
 *
 * and produce one entry per segment, all pointing at the same
 * canonical key (and therefore the same on-disk image path).  This
 * lets a user filename like "F-16 Fighter (USA).zip" match the
 * bundle via its second alt name. */
static bool dp_commit_entry(dp_parse_ctx_t *c)
{
   dp_parse_index_t *idx = c->idx;
   char  paren_strip[768];
   char  heavy_buf[512];
   const char *p;
   const char *seg_start;
   bool        any_committed = false;

   if (!c->cur_canonical)
      return true;

   /* Always commit the original canonical with its full heavy form
    * (preserves the as-shipped key in entries[].canonical for the
    * exact-canonical fast path). */
   dp_normalize_heavy(c->cur_canonical, heavy_buf, sizeof(heavy_buf));
   if (*heavy_buf)
   {
      if (!dp_append_entry(idx, c->cur_canonical, heavy_buf, &c->oom))
         goto fail;
      any_committed = true;
   }

   /* Detect alt-name bundle: paren-strip first (region/disc tags
    * also contain spaces but never " _ "), then look for ` _ `
    * inside the title body.  Only walk the body up to the first
    * '(' or '[' — alt names live in the title, never in flags. */
   {
      char *body_end;
      strlcpy(paren_strip, c->cur_canonical, sizeof(paren_strip));
      body_end = strchr(paren_strip, '(');
      if (!body_end)
         body_end = strchr(paren_strip, '[');
      if (body_end)
         *body_end = '\0';
      if (strstr(paren_strip, " _ "))
      {
         /* Reconstruct each segment with the original tail (parens)
          * appended, so per-segment heavies match the same way the
          * full canonical's heavy did — minus the other alt names. */
         const char *tail = body_end ? c->cur_canonical
               + (body_end - paren_strip) : "";
         seg_start = paren_strip;
         for (p = paren_strip; ; p++)
         {
            bool at_sep = (p[0] == ' ' && p[1] == '_' && p[2] == ' ');
            if (at_sep || *p == '\0')
            {
               size_t seg_len = (size_t)(p - seg_start);
               char   seg[768];
               if (seg_len == 0 || seg_len + strlen(tail) + 1
                     > sizeof(seg))
                  goto seg_advance;
               memcpy(seg, seg_start, seg_len);
               seg[seg_len] = '\0';
               strlcat(seg, tail, sizeof(seg));
               dp_normalize_heavy(seg, heavy_buf, sizeof(heavy_buf));
               /* Skip if heavy matches the full-canonical heavy
                * we already added (segment is the whole title). */
               if (*heavy_buf
                     && strcmp(heavy_buf, idx->entries[
                           idx->entries_count - 1].heavy) != 0)
               {
                  if (!dp_append_entry(idx, c->cur_canonical,
                           heavy_buf, &c->oom))
                     goto fail;
               }
seg_advance:
               if (*p == '\0')
                  break;
               seg_start = p + 3;
               p += 2; /* loop's p++ advances past the third byte */
            }
         }
      }
   }

   free(c->cur_canonical);
   c->cur_canonical = NULL;
   (void)any_committed;
   return true;

fail:
   free(c->cur_canonical);
   c->cur_canonical = NULL;
   return false;
}

/* Reject canonical keys that would let a hostile or buggy mirror
 * write JPGs outside the cache directory.  We use the key verbatim
 * as a filename, so:
 *   - path separators ('/', '\') would create subdirs or escape
 *   - ".." would escape via parent traversal
 *   - leading '.' would create a hidden file
 *   - NUL or control chars are pathological / cross-platform unsafe
 *
 * Returns true if the key is safe to use as a filename. */
static bool dp_canonical_safe(const char *s, size_t len)
{
   size_t i;
   if (len == 0 || len > 250)
      return false;
   if (s[0] == '.')
      return false;
   for (i = 0; i < len; i++)
   {
      unsigned char c = (unsigned char)s[i];
      if (c < 0x20 || c == 0x7f)
         return false;
      if (c == '/' || c == '\\')
         return false;
   }
   /* Reject embedded ".." anywhere — even mid-string it's suspicious
    * and not a legitimate No-Intro / Redump pattern. */
   if (len >= 2)
   {
      for (i = 0; i + 1 < len; i++)
      {
         if (s[i] == '.' && s[i + 1] == '.')
            return false;
      }
   }
   return true;
}

static bool dp_h_member(void *ctx, const char *str, size_t len)
{
   dp_parse_ctx_t *c = (dp_parse_ctx_t*)ctx;
   size_t n = len < sizeof(c->last_member) - 1 ? len : sizeof(c->last_member) - 1;
   memcpy(c->last_member, str, n);
   c->last_member[n] = '\0';

   /* If we're at the layer one deep inside "files", this member name
    * IS the canonical key of the next entry.  Stash it now; the
    * subsequent start_object will switch us into DP_PS_IN_ENTRY.
    *
    * Rejected keys leave cur_canonical=NULL → dp_commit_entry skips
    * the entry on end_object.  We still parse the value subtree to
    * stay in sync with rjson's depth tracking. */
   if (c->depth >= 1 && c->state_stack[c->depth - 1] == DP_PS_AT_FILES)
   {
      free(c->cur_canonical);
      c->cur_canonical = NULL;
      if (!dp_canonical_safe(str, len))
         return true;
      c->cur_canonical = (char*)malloc(len + 1);
      if (!c->cur_canonical)
      {
         c->oom = true;
         return false;
      }
      memcpy(c->cur_canonical, str, len);
      c->cur_canonical[len] = '\0';
   }
   return true;
}

static bool dp_h_start_object(void *ctx)
{
   dp_parse_ctx_t *c = (dp_parse_ctx_t*)ctx;
   dp_parse_state_t parent = (c->depth > 0)
         ? c->state_stack[c->depth - 1] : DP_PS_TOPLEVEL;
   dp_parse_state_t s;

   if (c->depth == 0)
      s = DP_PS_TOPLEVEL;
   else if (parent == DP_PS_TOPLEVEL && !strcmp(c->last_member, "files"))
   {
      s = DP_PS_AT_FILES;
      c->in_files = true;
      c->saw_files_obj = true;
   }
   else if (parent == DP_PS_AT_FILES)
      s = DP_PS_IN_ENTRY; /* an entry's value object */
   else if (parent == DP_PS_IN_ENTRY && !strcmp(c->last_member, "formats"))
      s = DP_PS_IN_FORMATS;
   else
      s = DP_PS_OTHER;

   if (c->depth >= (int)(sizeof(c->state_stack) / sizeof(c->state_stack[0])))
      return false;
   c->state_stack[c->depth++] = s;
   c->last_member[0] = '\0';
   return true;
}

static bool dp_h_end_object(void *ctx)
{
   dp_parse_ctx_t *c = (dp_parse_ctx_t*)ctx;
   if (c->depth > 0)
      c->depth--;
   /* Closing an IN_ENTRY object → commit accumulated entry. */
   if (c->depth > 0 && c->state_stack[c->depth] == DP_PS_IN_ENTRY)
   {
      if (!dp_commit_entry(c))
         return false;
   }
   return true;
}

static bool dp_h_start_array(void *ctx)
{
   dp_parse_ctx_t *c = (dp_parse_ctx_t*)ctx;
   if (c->depth >= (int)(sizeof(c->state_stack) / sizeof(c->state_stack[0])))
      return false;
   c->state_stack[c->depth++] = DP_PS_OTHER;
   c->last_member[0] = '\0';
   return true;
}

static bool dp_h_end_array(void *ctx)
{
   dp_parse_ctx_t *c = (dp_parse_ctx_t*)ctx;
   if (c->depth > 0)
      c->depth--;
   return true;
}

static bool dp_h_number(void *ctx, const char *str, size_t len)
{
   dp_parse_ctx_t *c = (dp_parse_ctx_t*)ctx;
   (void)str; (void)len;
   /* `formats.{jpg,webp}` numbers are still emitted by the server
    * but we no longer care: webp is always available, the URL never
    * carries a size, and the binary format stores no per-entry size.
    * The handler keeps consuming the value to stay in sync with
    * rjson's depth tracking. */
   c->last_member[0] = '\0';
   return true;
}

static bool dp_h_string(void *ctx, const char *str, size_t len)
{
   dp_parse_ctx_t *c = (dp_parse_ctx_t*)ctx;
   (void)str; (void)len;
   c->last_member[0] = '\0';
   return true;
}

static bool dp_h_bool(void *ctx, bool v) { dp_parse_ctx_t *c = (dp_parse_ctx_t*)ctx; (void)v; c->last_member[0] = '\0'; return true; }
static bool dp_h_null(void *ctx) { dp_parse_ctx_t *c = (dp_parse_ctx_t*)ctx; c->last_member[0] = '\0'; return true; }

#ifdef DOWNPLAY_THUMBS_TEST_BUILD
static void dp_h_error(void *ctx, int line, int col, const char *err)
{
   (void)ctx;
   fprintf(stderr, "  rjson error line %d col %d: %s\n", line, col, err);
}
#endif

/* ---------------------------------------------------------------- */
/* Section 2.5: binary format writer (parse temp → packed buffer)   */
/* ---------------------------------------------------------------- */

/* fnv1a-32: small, no table, no allocation.  Used both at write time
 * (BY_HEAVY index keys) and at query time (cascade bsearch needle).
 * Both sites must produce identical bytes — keep the constants and
 * the iteration order locked. */
static uint32_t dp_fnv1a32(const char *s)
{
   uint32_t h = 0x811c9dc5u;
   while (*s)
   {
      h ^= (unsigned char)*s++;
      h *= 0x01000193u;
   }
   return h;
}

/* Build-time string interning pool with open-addressing hash dedup.
 * Lives only across one dp_idx_emit_buffer call.  Hash table stores
 * (offset+1) values so 0 is a sentinel for "empty slot". */
typedef struct
{
   char     *data;
   size_t    size;
   size_t    cap;
   uint32_t *ht;
   size_t    htmask;        /* (htsize - 1); htsize is power of two */
   size_t    htcount;
} dp_strpool_t;

static bool dp_strpool_grow_ht(dp_strpool_t *p)
{
   size_t    new_size = p->htmask ? (p->htmask + 1) * 2 : 256;
   uint32_t *nht;
   size_t    i;
   nht = (uint32_t*)calloc(new_size, sizeof(uint32_t));
   if (!nht)
      return false;
   if (p->ht)
   {
      for (i = 0; i <= p->htmask; i++)
      {
         uint32_t off = p->ht[i];
         if (!off)
            continue;
         {
            const char *s = p->data + (off - 1);
            size_t      pos = dp_fnv1a32(s) & (new_size - 1);
            while (nht[pos])
               pos = (pos + 1) & (new_size - 1);
            nht[pos] = off;
         }
      }
      free(p->ht);
   }
   p->ht     = nht;
   p->htmask = new_size - 1;
   return true;
}

static bool dp_strpool_grow_data(dp_strpool_t *p, size_t need)
{
   size_t new_cap = p->cap ? p->cap : 4096;
   char  *n;
   while (new_cap < p->size + need)
   {
      if (new_cap > ((size_t)1 << 30))
         return false;
      new_cap *= 2;
   }
   n = (char*)realloc(p->data, new_cap);
   if (!n)
      return false;
   p->data = n;
   p->cap  = new_cap;
   return true;
}

/* Intern `s`.  On success writes its byte offset into the pool to
 * `*out`.  Returns false on allocation failure. */
static bool dp_strpool_intern(dp_strpool_t *p, const char *s, uint32_t *out)
{
   size_t   slen = strlen(s);
   uint32_t h    = dp_fnv1a32(s);
   size_t   pos;

   /* Grow ht if load factor would exceed 0.5. */
   if (!p->ht || (p->htcount + 1) * 2 > p->htmask + 1)
      if (!dp_strpool_grow_ht(p))
         return false;

   pos = h & p->htmask;
   while (p->ht[pos])
   {
      uint32_t off = p->ht[pos] - 1;
      if (strcmp(p->data + off, s) == 0)
      {
         *out = off;
         return true;
      }
      pos = (pos + 1) & p->htmask;
   }
   if (p->size + slen + 1 > p->cap)
      if (!dp_strpool_grow_data(p, slen + 1))
         return false;
   memcpy(p->data + p->size, s, slen + 1);
   *out       = (uint32_t)p->size;
   p->ht[pos] = (uint32_t)(p->size + 1);
   p->htcount++;
   p->size   += slen + 1;
   return true;
}

static void dp_strpool_free(dp_strpool_t *p)
{
   free(p->data);
   free(p->ht);
   memset(p, 0, sizeof(*p));
}

/* qsort comparator state.  File-static; only set during the writer's
 * sort calls, on the single UI thread.  qsort takes no user-data
 * pointer, hence the global. */
static const dp_thumb_entry_t *g_dp_emit_entries;

static int dp_cmp_canonical_idx(const void *a, const void *b)
{
   uint32_t ia = *(const uint32_t*)a;
   uint32_t ib = *(const uint32_t*)b;
   return strcmp(g_dp_emit_entries[ia].canonical,
                 g_dp_emit_entries[ib].canonical);
}

typedef struct { uint32_t hash; uint32_t idx; } dp_hev_pair_t;

static int dp_cmp_heavy_pair(const void *a, const void *b)
{
   const dp_hev_pair_t *pa = (const dp_hev_pair_t*)a;
   const dp_hev_pair_t *pb = (const dp_hev_pair_t*)b;
   int rv;
   if (pa->hash != pb->hash)
      return pa->hash < pb->hash ? -1 : 1;
   /* On hash collision, sort by heavy strcmp so equal-heavy entries
    * stay contiguous within an equal-hash run.  Then by canonical for
    * full determinism even when two entries share an identical heavy. */
   rv = strcmp(g_dp_emit_entries[pa->idx].heavy,
               g_dp_emit_entries[pb->idx].heavy);
   if (rv != 0)
      return rv;
   return strcmp(g_dp_emit_entries[pa->idx].canonical,
                 g_dp_emit_entries[pb->idx].canonical);
}

/* Serialise the parsed entry array into the on-disk binary format.
 * On success transfers ownership of the buffer to `*out_buf` (caller
 * frees) and writes its length to `*out_len`.  Returns false on
 * allocation failure or if `n` exceeds the entry-count cap. */
static bool dp_idx_emit_buffer(const dp_thumb_entry_t *entries, size_t n,
      uint8_t **out_buf, size_t *out_len)
{
   uint8_t        *buf      = NULL;
   uint32_t       *can_offs = NULL;
   uint32_t       *hev_offs = NULL;
   uint32_t       *bcanon   = NULL;
   dp_hev_pair_t  *bheavy   = NULL;
   dp_strpool_t    pool;
   size_t          i;
   size_t          entries_off, bcanon_off, bheavy_off, strings_off, footer_off;
   size_t          entries_sz, bcanon_sz, bheavy_sz, total;
   bool            ok = false;

   memset(&pool, 0, sizeof(pool));
   if (n > DP_THUMBS_MAX_ENTRIES)
      return false;

   if (n > 0)
   {
      can_offs = (uint32_t*)malloc(n * sizeof(*can_offs));
      hev_offs = (uint32_t*)malloc(n * sizeof(*hev_offs));
      if (!can_offs || !hev_offs)
         goto out;
   }
   for (i = 0; i < n; i++)
   {
      if (!dp_strpool_intern(&pool, entries[i].canonical, &can_offs[i]))
         goto out;
      if (!dp_strpool_intern(&pool, entries[i].heavy,     &hev_offs[i]))
         goto out;
   }

   entries_off = DP_IDX_HEADER_SZ;
   entries_sz  = n * DP_IDX_REC_SZ;
   bcanon_off  = entries_off + entries_sz;
   bcanon_sz   = n * DP_IDX_BCAN_SZ;
   bheavy_off  = bcanon_off + bcanon_sz;
   bheavy_sz   = n * DP_IDX_BHEV_SZ;
   strings_off = bheavy_off + bheavy_sz;
   footer_off  = strings_off + pool.size;
   total       = footer_off + DP_IDX_FOOTER_SZ;

   /* All section offsets are written into the header as u32.  Today
    * the entry-count cap (100k) and the string-pool cap (1 GB) keep
    * us comfortably under 4 GB, but a future cap bump that pushed
    * any offset past UINT32_MAX would silently produce a truncated
    * header that subsequent dp_idx_open calls would fail with no
    * useful error.  Fail loudly here instead. */
   if (footer_off > UINT32_MAX || pool.size > UINT32_MAX)
      goto out;

   buf = (uint8_t*)calloc(total, 1);
   if (!buf)
      goto out;

   /* Header.  All multibyte fields written as little-endian via
    * memcpy of host u32 — Downplay ships only on LE targets. */
   {
      uint32_t v;
      v = DP_IDX_MAGIC;            memcpy(buf +  0, &v, 4);
      v = DP_IDX_VERSION;          memcpy(buf +  4, &v, 4);
      v = (uint32_t)n;             memcpy(buf +  8, &v, 4);
      v = (uint32_t)pool.size;     memcpy(buf + 12, &v, 4);
      v = (uint32_t)entries_off;   memcpy(buf + 16, &v, 4);
      v = (uint32_t)bcanon_off;    memcpy(buf + 20, &v, 4);
      v = (uint32_t)bheavy_off;    memcpy(buf + 24, &v, 4);
      v = (uint32_t)strings_off;   memcpy(buf + 28, &v, 4);
   }

   /* ENTRIES section: u32 canonical_off, u32 heavy_off per entry,
    * original load order. */
   for (i = 0; i < n; i++)
   {
      memcpy(buf + entries_off + i * DP_IDX_REC_SZ + 0, &can_offs[i], 4);
      memcpy(buf + entries_off + i * DP_IDX_REC_SZ + 4, &hev_offs[i], 4);
   }

   /* BY_CANONICAL: u32 entry indices sorted by strcmp(canonical). */
   if (n > 0)
   {
      bcanon = (uint32_t*)malloc(n * sizeof(*bcanon));
      if (!bcanon)
         goto out;
      for (i = 0; i < n; i++)
         bcanon[i] = (uint32_t)i;
      g_dp_emit_entries = entries;
      qsort(bcanon, n, sizeof(*bcanon), dp_cmp_canonical_idx);
      g_dp_emit_entries = NULL;
      for (i = 0; i < n; i++)
         memcpy(buf + bcanon_off + i * DP_IDX_BCAN_SZ, &bcanon[i], 4);
   }

   /* BY_HEAVY: {u32 hash, u32 idx} sorted by (hash, strcmp(heavy)). */
   if (n > 0)
   {
      bheavy = (dp_hev_pair_t*)malloc(n * sizeof(*bheavy));
      if (!bheavy)
         goto out;
      for (i = 0; i < n; i++)
      {
         bheavy[i].hash = dp_fnv1a32(entries[i].heavy);
         bheavy[i].idx  = (uint32_t)i;
      }
      g_dp_emit_entries = entries;
      qsort(bheavy, n, sizeof(*bheavy), dp_cmp_heavy_pair);
      g_dp_emit_entries = NULL;
      for (i = 0; i < n; i++)
      {
         memcpy(buf + bheavy_off + i * DP_IDX_BHEV_SZ + 0, &bheavy[i].hash, 4);
         memcpy(buf + bheavy_off + i * DP_IDX_BHEV_SZ + 4, &bheavy[i].idx,  4);
      }
   }

   /* String pool. */
   if (pool.size > 0)
      memcpy(buf + strings_off, pool.data, pool.size);

   /* Footer: magic + entry_count repeat.  Catches partial writes on
    * FUSE-backed filesystems where rename(2) isn't reliably atomic. */
   {
      uint32_t v;
      v = DP_IDX_MAGIC; memcpy(buf + footer_off + 0, &v, 4);
      v = (uint32_t)n;  memcpy(buf + footer_off + 4, &v, 4);
   }

   *out_buf = buf;
   *out_len = total;
   buf      = NULL; /* ownership transferred */
   ok       = true;

out:
   dp_strpool_free(&pool);
   free(can_offs);
   free(hev_offs);
   free(bcanon);
   free(bheavy);
   free(buf);
   return ok;
}

/* ---------------------------------------------------------------- */
/* Section 3: binary index reader + match cascade                   */
/* ---------------------------------------------------------------- */

/* Public-API index handle.  Wraps a validated binary buffer; every
 * field accessor reads via memcpy from offsets known at open time.
 * No allocations during query, no string copies — all string returns
 * are pointers into `buf`'s string pool, valid for the lifetime of
 * the handle. */
struct downplay_thumbs_index
{
   uint8_t  *buf;          /* owned, freed on _index_free */
   size_t    buf_len;
   uint32_t  entry_count;
   uint32_t  entries_off;
   uint32_t  bcanon_off;
   uint32_t  bheavy_off;
   uint32_t  strings_off;
   uint32_t  strings_size;
};

/* All field reads go through memcpy.  Avoids strict-aliasing UB and
 * compiles to a single ldr on arm64 at -O2. */
static uint32_t dp_idx_read_u32(const uint8_t *base, size_t off)
{
   uint32_t v;
   memcpy(&v, base + off, 4);
   return v;
}

/* Validate `buf` against the format spec.  On success, returns a
 * heap-allocated index_t that owns `buf` (frees on _index_free).
 * On failure, frees `buf` and returns NULL.  Caller must not touch
 * `buf` after this call regardless of outcome.
 *
 * Validation scope: header magic/version/section layout, total file
 * size matches layout exactly, string pool ends in NUL, footer
 * matches header.  We deliberately do NOT walk every entry to
 * confirm its `canonical_off` / `heavy_off` is in-bounds — that
 * would be O(N) work on open, and the file is written by trusted
 * code (the writer never produces an out-of-range offset).  The
 * dp_idx_str accessor returns "" on out-of-bounds offsets as a
 * defensive fallback against post-validation memory corruption. */
static downplay_thumbs_index_t *dp_idx_open(uint8_t *buf, size_t buf_len)
{
   downplay_thumbs_index_t *idx;
   uint32_t magic, version, entry_count, strings_size;
   uint32_t entries_off, bcanon_off, bheavy_off, strings_off;
   uint64_t entries_sz, bcanon_sz, bheavy_sz, footer_off, expected;
   uint32_t fmagic, fcount;

   if (!buf || buf_len < (size_t)(DP_IDX_HEADER_SZ + DP_IDX_FOOTER_SZ))
      goto fail;

   magic        = dp_idx_read_u32(buf,  0);
   version      = dp_idx_read_u32(buf,  4);
   entry_count  = dp_idx_read_u32(buf,  8);
   strings_size = dp_idx_read_u32(buf, 12);
   entries_off  = dp_idx_read_u32(buf, 16);
   bcanon_off   = dp_idx_read_u32(buf, 20);
   bheavy_off   = dp_idx_read_u32(buf, 24);
   strings_off  = dp_idx_read_u32(buf, 28);

   if (magic != DP_IDX_MAGIC)              goto fail;
   if (version != DP_IDX_VERSION)          goto fail;
   if (entry_count > DP_THUMBS_MAX_ENTRIES) goto fail;

   /* Layout must be exact.  uint64_t math makes overflow impossible
    * since entry_count is already capped above. */
   entries_sz = (uint64_t)entry_count * DP_IDX_REC_SZ;
   bcanon_sz  = (uint64_t)entry_count * DP_IDX_BCAN_SZ;
   bheavy_sz  = (uint64_t)entry_count * DP_IDX_BHEV_SZ;
   if (entries_off  != DP_IDX_HEADER_SZ)                              goto fail;
   if ((uint64_t)bcanon_off  != (uint64_t)entries_off + entries_sz)   goto fail;
   if ((uint64_t)bheavy_off  != (uint64_t)bcanon_off  + bcanon_sz)    goto fail;
   if ((uint64_t)strings_off != (uint64_t)bheavy_off  + bheavy_sz)    goto fail;

   footer_off = (uint64_t)strings_off + strings_size;
   expected   = footer_off + DP_IDX_FOOTER_SZ;
   if ((uint64_t)buf_len != expected)
      goto fail;

   /* String pool must end with NUL so strcmp inside the pool needs no
    * per-call bounds check — anything off-by-one would walk straight
    * into the footer's magic bytes. */
   if (strings_size > 0 && buf[footer_off - 1] != '\0')
      goto fail;

   /* Footer mirrors the header — partial-write detection. */
   fmagic = dp_idx_read_u32(buf, (size_t)footer_off);
   fcount = dp_idx_read_u32(buf, (size_t)footer_off + 4);
   if (fmagic != DP_IDX_MAGIC || fcount != entry_count)
      goto fail;

   idx = (downplay_thumbs_index_t*)calloc(1, sizeof(*idx));
   if (!idx)
      goto fail;
   idx->buf          = buf;
   idx->buf_len      = buf_len;
   idx->entry_count  = entry_count;
   idx->entries_off  = entries_off;
   idx->bcanon_off   = bcanon_off;
   idx->bheavy_off   = bheavy_off;
   idx->strings_off  = strings_off;
   idx->strings_size = strings_size;
   return idx;

fail:
   free(buf);
   return NULL;
}

void downplay_thumbs_index_free(downplay_thumbs_index_t *idx)
{
   if (!idx)
      return;
   free(idx->buf);
   free(idx);
}

size_t downplay_thumbs_index_count(const downplay_thumbs_index_t *idx)
{
   return idx ? idx->entry_count : 0;
}

/* String pool resolver.  Validation guarantees `off < strings_size`
 * for any offset the format itself produced, and that the pool is
 * NUL-terminated, so strcmp on the returned pointer is safe.  Out-of-
 * bounds offsets (post-validation memory corruption — not reachable
 * via the file format) return "" defensively. */
static const char *dp_idx_str(const downplay_thumbs_index_t *idx, uint32_t off)
{
   if (off >= idx->strings_size)
      return "";
   return (const char*)(idx->buf + idx->strings_off + off);
}

static const char *dp_idx_canonical(
      const downplay_thumbs_index_t *idx, uint32_t e)
{
   return dp_idx_str(idx, dp_idx_read_u32(idx->buf,
         idx->entries_off + (size_t)e * DP_IDX_REC_SZ + 0));
}

static const char *dp_idx_heavy(
      const downplay_thumbs_index_t *idx, uint32_t e)
{
   return dp_idx_str(idx, dp_idx_read_u32(idx->buf,
         idx->entries_off + (size_t)e * DP_IDX_REC_SZ + 4));
}

static uint32_t dp_idx_bcanon_at(
      const downplay_thumbs_index_t *idx, size_t i)
{
   return dp_idx_read_u32(idx->buf,
         idx->bcanon_off + i * DP_IDX_BCAN_SZ);
}

static void dp_idx_bheavy_at(const downplay_thumbs_index_t *idx, size_t i,
      uint32_t *out_hash, uint32_t *out_idx)
{
   *out_hash = dp_idx_read_u32(idx->buf,
         idx->bheavy_off + i * DP_IDX_BHEV_SZ + 0);
   *out_idx  = dp_idx_read_u32(idx->buf,
         idx->bheavy_off + i * DP_IDX_BHEV_SZ + 4);
}

/* Strip `.ext` from `in` into `out`.  Returns out for chaining. */
static char *dp_strip_ext(const char *in, char *out, size_t out_size)
{
   const char *dot;
   if (out_size == 0)
      return out;
   strlcpy(out, in ? in : "", out_size);
   /* Caller passes a basename (no path separators), so plain strrchr
    * on the buffer is correct.  Skip files starting with '.' — they
    * have no real extension to strip ("." at index 0). */
   dot = strrchr(out, '.');
   if (dot && dot != out)
      out[dot - out] = '\0';
   return out;
}

/* Phase 1: bsearch BY_CANONICAL for an exact match.  Returns the
 * entry index or SIZE_MAX on miss. */
static size_t dp_lookup_exact_canonical(
      const downplay_thumbs_index_t *idx, const char *stem)
{
   size_t lo = 0, hi = idx->entry_count;
   while (lo < hi)
   {
      size_t   mid = lo + (hi - lo) / 2;
      uint32_t e   = dp_idx_bcanon_at(idx, mid);
      int      rv  = strcmp(dp_idx_canonical(idx, e), stem);
      if (rv == 0)
         return e;
      if (rv < 0) lo = mid + 1;
      else        hi = mid;
   }
   return (size_t)-1;
}

/* Phase 2 helper: bsearch BY_HEAVY by (hash, then strcmp on tie).
 * Returns the smallest BY_HEAVY index whose key is >= (needle_hash,
 * needle), or entry_count if all keys are smaller. */
static size_t dp_lower_bound_heavy(const downplay_thumbs_index_t *idx,
      const char *needle, uint32_t needle_hash)
{
   size_t lo = 0, hi = idx->entry_count;
   while (lo < hi)
   {
      size_t   mid = lo + (hi - lo) / 2;
      uint32_t h, ei;
      dp_idx_bheavy_at(idx, mid, &h, &ei);
      if (h < needle_hash)
         lo = mid + 1;
      else if (h > needle_hash)
         hi = mid;
      else
      {
         /* Tie on hash — refine by strcmp on the heavy string. */
         int rv = strcmp(dp_idx_heavy(idx, ei), needle);
         if (rv < 0) lo = mid + 1;
         else        hi = mid;
      }
   }
   return lo;
}

/* Tiebreak score for one candidate, derived from canonical on
 * demand — the binary format stores none of region/rev/disc/bad_dump,
 * so we recompute here.  Cascade ranges are tiny (1-5 entries) so
 * total per-query cost is well under a microsecond.  Lower wins.
 *
 * Layered, in order of significance:
 *   - bad_dump:      +1,000,000
 *   - disc match:    ±100,000 (matches user_disc → bonus, mismatches → penalty)
 *   - region_score:  ×100, range 0..900
 *   - -rev_num:      negative so newer rev wins */
static int dp_score_canonical(const char *canonical, const char *user_disc)
{
   int sc = 0;
   if (dp_detect_bad_dump(canonical))
      sc += 1000000;
   if (user_disc && *user_disc)
   {
      char disc[16];
      dp_extract_disc_token(canonical, disc, sizeof(disc));
      if      (*disc && !strcmp(disc, user_disc)) sc -= 100000;
      else if (*disc)                             sc += 100000;
   }
   sc += dp_score_region(canonical) * 100;
   sc -= dp_extract_rev(canonical);
   return sc;
}

/* Among the BY_HEAVY range starting at `start` whose (hash, heavy)
 * equal the needle, pick the best candidate by dp_score_canonical.
 * Canonical lex order breaks score ties for full determinism.
 * Returns SIZE_MAX if no candidate matched. */
static size_t dp_pick_best(const downplay_thumbs_index_t *idx,
      size_t start, uint32_t needle_hash, const char *needle_heavy,
      const char *user_disc)
{
   size_t      i;
   size_t      best       = (size_t)-1;
   const char *best_canon = NULL;
   int         best_sc    = 0x7fffffff;
   for (i = start; i < idx->entry_count; i++)
   {
      uint32_t    h, ei;
      const char *can;
      int         sc;
      dp_idx_bheavy_at(idx, i, &h, &ei);
      if (h != needle_hash)
         break;
      if (strcmp(dp_idx_heavy(idx, ei), needle_heavy) != 0)
         break;
      can = dp_idx_canonical(idx, ei);
      sc  = dp_score_canonical(can, user_disc);
      if (sc < best_sc
            || (sc == best_sc && best_canon
                  && strcmp(can, best_canon) < 0))
      {
         best_sc    = sc;
         best_canon = can;
         best       = ei;
      }
   }
   return best;
}

/* Internal lookup returning the entry index (or SIZE_MAX).  Used
 * directly by the manager; the public `_match` wraps it for tests. */
static size_t dp_index_match_idx(
      const downplay_thumbs_index_t *idx,
      const char *rom_basename)
{
   char        stem[512];
   char        heavy_user[512];
   char        user_disc[16];
   uint32_t    needle_hash;
   uint32_t    h, ei;
   size_t      lo;
   size_t      hit_idx;

   if (!idx || !rom_basename || !*rom_basename)
      return (size_t)-1;

   dp_strip_ext(rom_basename, stem, sizeof(stem));
   if (!*stem)
      return (size_t)-1;

   hit_idx = dp_lookup_exact_canonical(idx, stem);
   if (hit_idx != (size_t)-1)
      return hit_idx;

   dp_normalize_heavy(stem, heavy_user, sizeof(heavy_user));
   if (!*heavy_user)
      return (size_t)-1;

   needle_hash = dp_fnv1a32(heavy_user);
   lo = dp_lower_bound_heavy(idx, heavy_user, needle_hash);
   if (lo >= idx->entry_count)
      return (size_t)-1;
   /* Confirm the lower-bound hit is actually equal — lower-bound
    * returns the first slot >= so a non-equal landing means miss. */
   dp_idx_bheavy_at(idx, lo, &h, &ei);
   if (h != needle_hash)
      return (size_t)-1;
   if (strcmp(dp_idx_heavy(idx, ei), heavy_user) != 0)
      return (size_t)-1;

   dp_extract_disc_token(stem, user_disc, sizeof(user_disc));
   return dp_pick_best(idx, lo, needle_hash, heavy_user, user_disc);
}

const char *downplay_thumbs_index_match(
      const downplay_thumbs_index_t *idx,
      const char *rom_basename)
{
   size_t hit_idx = dp_index_match_idx(idx, rom_basename);
   if (hit_idx == (size_t)-1)
      return NULL;
   return dp_idx_canonical(idx, (uint32_t)hit_idx);
}

/* ---------------------------------------------------------------- */
/* Section 3.5: public parse entrypoint                             */
/* ---------------------------------------------------------------- */

/* Free the parse-time temp index (transient — used only inside the
 * JSON → binary conversion helpers). */
static void dp_parse_index_free(dp_parse_index_t *idx)
{
   size_t i;
   if (!idx)
      return;
   for (i = 0; i < idx->entries_count; i++)
   {
      free(idx->entries[i].canonical);
      free(idx->entries[i].heavy);
   }
   free(idx->entries);
   free(idx);
}

/* Parse JSON into a serialised binary `.idx` buffer.  Shared between
 * the public test entrypoint (which then opens+validates) and the
 * download callbacks (which write the buffer to disk).  On success
 * the caller owns `*out_buf` and frees with free(). */
static bool dp_idx_parse_json_to_buffer(const char *json, size_t json_len,
      uint8_t **out_buf, size_t *out_len)
{
   dp_parse_index_t *parse_idx;
   dp_parse_ctx_t    ctx;
   bool              ok;
   bool              emitted = false;

   if (!json || json_len == 0)
      return false;
   parse_idx = (dp_parse_index_t*)calloc(1, sizeof(*parse_idx));
   if (!parse_idx)
      return false;

   memset(&ctx, 0, sizeof(ctx));
   ctx.idx = parse_idx;

   ok = rjson_parse_quick(json, json_len, &ctx, 0,
         dp_h_member, dp_h_string, dp_h_number,
         dp_h_start_object, dp_h_end_object,
         dp_h_start_array, dp_h_end_array,
         dp_h_bool, dp_h_null,
#ifdef DOWNPLAY_THUMBS_TEST_BUILD
         dp_h_error
#else
         NULL /* error_handler */
#endif
         );
   /* Free leftover parse-context state regardless of outcome.
    * cur_canonical is the only ctx-side allocation that survives
    * past handlers when parsing aborts mid-entry. */
   free(ctx.cur_canonical);

   if (!ok || ctx.oom || !ctx.saw_files_obj)
   {
      dp_parse_index_free(parse_idx);
      return false;
   }
   emitted = dp_idx_emit_buffer(parse_idx->entries, parse_idx->entries_count,
         out_buf, out_len);
   dp_parse_index_free(parse_idx);
   return emitted;
}

downplay_thumbs_index_t *downplay_thumbs_index_parse(
      const char *json, size_t json_len)
{
   uint8_t *buf     = NULL;
   size_t   buf_len = 0;
   if (!dp_idx_parse_json_to_buffer(json, json_len, &buf, &buf_len))
      return NULL;
   /* dp_idx_open takes ownership of buf — frees on validation failure. */
   return dp_idx_open(buf, buf_len);
}

/* ---------------------------------------------------------------- */
/* Section 4: manager                                               */
/* ---------------------------------------------------------------- */

#ifndef DOWNPLAY_THUMBS_TEST_BUILD

#define DP_THUMBS_BASE_URL       "https://thumbnails.pastime.gg"
#define DP_THUMBS_INDEX_TTL_SEC  (7 * 24 * 60 * 60)
#define DP_THUMBS_QUEUE_CAP      32
#define DP_THUMBS_INFLIGHT_MAX   3

enum
{
   DP_LOAD_IDLE = 0,    /* never tried */
   DP_LOAD_FETCHING,    /* HTTP task in flight, polling for file */
   DP_LOAD_READY,       /* parsed; queries answer authoritatively */
   DP_LOAD_FAILED       /* HTTP/parse failed; queries answer UNKNOWN */
};

struct downplay_thumbs
{
   char system[256];
   char idx_path[DP_THUMBS_PATH_MAX];
   char cache_dir[DP_THUMBS_PATH_MAX];

   int  load_state;
   bool index_task_pushed;

   downplay_thumbs_index_t *index;

   /* Per-canonical fetch tracking.  Parallel to the binary index's
    * ENTRIES section; an entry's status drives _request return values
    * + de-dups in-flight downloads.  Allocated on index ready. */
   uint8_t *attempt;        /* DP_ATT_* values, length entry_count */

   /* Image fetch ring buffer of entry indices into the binary index. */
   uint32_t queue[DP_THUMBS_QUEUE_CAP];
   uint8_t  queue_pri[DP_THUMBS_QUEUE_CAP]; /* 1=active, 0=prefetch */
   int      queue_head, queue_tail;

   /* Bounded set of currently-fetching entry indices.  Replaces the
    * old O(N) recount sweep — pump only stats this small set, and we
    * compact in place when an entry lands. */
   uint32_t fetching[DP_THUMBS_INFLIGHT_MAX];
   int      inflight;

   /* Diagnostic miss log.  Appended on first definitive miss
    * (index loaded, no match) for a given basename; in-memory set
    * prevents per-frame spam.  Path:
    *   <root>/Downplay/Thumbs/misses.log
    * Pull off-device with `adb pull` to triage real-world misses. */
   char     log_path[DP_THUMBS_PATH_MAX];
   char   **logged_misses;
   size_t   logged_misses_count;
   size_t   logged_misses_cap;
};

enum
{
   DP_ATT_UNTRIED = 0,
   DP_ATT_FETCHING,
   DP_ATT_ON_DISK,
   DP_ATT_FAILED
};

/* ---- internal: paths ---- */

/* Compute <root>/Thumbs/index/<system>.idx into `out`.  Single source
 * of truth for the on-disk binary-format index location: both
 * `downplay_thumbs_open` and the boot-time prefetch share this so
 * they cannot disagree on where the file lives.  Returns false if
 * the filesystem root can't be resolved.
 *
 * Filename used to be `<system>.index.json` when the cache held
 * gzipped JSON.  The on-disk format is now a packed binary file
 * (see DP_IDX_* constants); old `.index.json` caches are abandoned
 * naturally — first open after upgrade sees no `.idx`, refetches,
 * writes the new format. */
static bool dp_thumbs_index_path(const char *system,
      char *out, size_t out_size)
{
   char root[DP_THUMBS_PATH_MAX];
   char base[DP_THUMBS_PATH_MAX];
   char idx_dir[DP_THUMBS_PATH_MAX];
   char fname[256];
   if (!system || !*system || !out || out_size == 0)
      return false;
   if (!downplay_paths_get_root(root, sizeof(root)))
      return false;
   fill_pathname_join_special(base, root, "Thumbs", sizeof(base));
   fill_pathname_join_special(idx_dir, base, "index", sizeof(idx_dir));
   snprintf(fname, sizeof(fname), "%s.idx", system);
   fill_pathname_join_special(out, idx_dir, fname, out_size);
   return true;
}

/* Build the on-disk path for an image of canonical key `canonical`.
 * The canonical key is used verbatim as the filename — it's already
 * the No-Intro safe form (no path separators, since it's the title
 * of a game).  We always use `.webp` since the server is guaranteed
 * to publish WebP for every entry. */
static void dp_build_image_path(downplay_thumbs_t *t,
      const char *canonical, char *out, size_t out_size)
{
   char tmp[DP_THUMBS_PATH_MAX];
   fill_pathname_join_special(tmp, t->cache_dir, canonical, sizeof(tmp));
   snprintf(out, out_size, "%s.webp", tmp);
}

/* Resolve the on-disk path for an entry.  Stat the cache; on hit,
 * write the path and return true.  We no longer need a webp→jpg
 * sibling fallback: the manager always writes `.webp` and the
 * server always publishes `.webp`.  Pre-WebP `.jpg` siblings from
 * old Downplay versions are simply ignored (they sit on disk
 * orphaned until manual cleanup; harmless). */
static bool dp_resolve_local_image(downplay_thumbs_t *t,
      uint32_t e_idx, char *out, size_t out_size)
{
   const char *canonical = dp_idx_canonical(t->index, e_idx);
   dp_build_image_path(t, canonical, out, out_size);
   return path_is_valid(out);
}

/* Build the remote URL for an image of canonical key `canonical`. */
static void dp_build_image_url(downplay_thumbs_t *t,
      const char *canonical, char *out, size_t out_size)
{
   char raw[2048];
   snprintf(raw, sizeof(raw), "%s/%s/Named_Boxarts/%s.webp",
         DP_THUMBS_BASE_URL, t->system, canonical);
   net_http_urlencode_full(out, raw, out_size);
}

/* Hostile / misconfigured server can't blow up our cache.  Real
 * indexes are tens-to-hundreds of KB; 1 MB leaves real headroom for
 * the largest No-Intro systems (PSX, NDS, etc.) with their alt-name
 * bundles, while still capping a runaway response at a safe size.
 * Applied to the *compressed* (gzipped) payload before decompression. */
#define DP_THUMBS_PF_INDEX_MAX_BYTES (1024u * 1024u)

/* ---- internal: gzip helper ----
 *
 * Indexes are served as `index.json.gz`.  We decompress once at
 * fetch-completion time, parse the JSON straight into the binary
 * `.idx` form, and write that to disk — JSON never lands on the
 * filesystem.  See dp_cb_index_download.
 *
 * gzip wraps a deflate stream with a 10-byte header (magic 1F 8B + a
 * tiny metadata block) and an 8-byte footer carrying CRC32 + ISIZE
 * (uncompressed size mod 2^32).  Reading ISIZE up front lets us
 * allocate exactly the output buffer needed and reject anything that
 * would exceed our hard cap before we even hand bytes to zlib.
 *
 * Returns a malloc'd buffer (caller frees) on success; NULL on bad
 * magic, oversize ISIZE, alloc failure, or inflate error.  The cap
 * is the same DP_THUMBS_PF_INDEX_MAX_BYTES applied to compressed
 * input scaled by a max ratio; tune via DP_THUMBS_INDEX_MAX_RATIO
 * if the server ever switches to a different compressor. */
#define DP_THUMBS_INDEX_MAX_RATIO 32  /* gzip-of-JSON typical ratio is 6-12; 32 leaves headroom */

static uint8_t *dp_gunzip(const uint8_t *in, size_t in_len, size_t *out_len)
{
   const struct trans_stream_backend *backend;
   void                              *stream  = NULL;
   uint8_t                           *out     = NULL;
   uint32_t                           isize;
   uint32_t                           rd      = 0;
   uint32_t                           wn      = 0;
   enum trans_stream_error            xerr    = TRANS_STREAM_ERROR_NONE;
   const size_t                       max_out =
         (size_t)DP_THUMBS_PF_INDEX_MAX_BYTES * DP_THUMBS_INDEX_MAX_RATIO;

   if (out_len)
      *out_len = 0;
   if (!in || in_len < 18) /* 10-byte header + min payload + 8-byte footer */
      return NULL;
   /* gzip magic: 1F 8B.  Reject anything else (CDN error pages, raw
    * deflate, unknown compressor) before allocating. */
   if (in[0] != 0x1F || in[1] != 0x8B)
      return NULL;

   /* ISIZE is the last 4 bytes, little-endian, mod 2^32.  For inputs
    * >4 GiB this lies; we cap well below that elsewhere so it's fine. */
   isize = (uint32_t)in[in_len - 4]
         | ((uint32_t)in[in_len - 3] << 8)
         | ((uint32_t)in[in_len - 2] << 16)
         | ((uint32_t)in[in_len - 1] << 24);
   if (isize == 0 || isize > max_out)
      return NULL;
   /* trans_stream's set_in/set_out take uint32_t lengths.  Our caps
    * keep both sides well under 4 GiB today, but make the narrowing
    * explicit so a future cap bump can't silently truncate. */
   if (in_len > UINT32_MAX || (size_t)isize > UINT32_MAX)
      return NULL;

   backend = trans_stream_get_zlib_inflate_backend();
   if (!backend)
      return NULL;
   stream = backend->stream_new();
   if (!stream)
      return NULL;
   /* window_bits = MAX_WBITS (15) + 16 = 31 ⇒ gzip-only.  Strict on
    * purpose: anything that isn't gzip should already have been
    * rejected by the magic check above, but we don't want zlib to
    * silently accept a raw-deflate stream either. */
   if (backend->define && !backend->define(stream, "window_bits", 31))
   {
      backend->stream_free(stream);
      return NULL;
   }

   out = (uint8_t*)malloc(isize);
   if (!out)
   {
      backend->stream_free(stream);
      return NULL;
   }
   backend->set_in (stream, in,  (uint32_t)in_len);
   backend->set_out(stream, out, isize);
   if (!backend->trans(stream, true /* flush */, &rd, &wn, &xerr)
       || xerr != TRANS_STREAM_ERROR_NONE
       || wn != isize)
   {
      free(out);
      backend->stream_free(stream);
      return NULL;
   }
   backend->stream_free(stream);
   if (out_len)
      *out_len = (size_t)wn;
   return out;
}

/* ---- internal: HTTP callbacks (detached from manager) ----
 *
 * Both callbacks run on the main thread (RA's task system dispatches
 * `t->callback` from `task_queue_check`).  They write the file then
 * free their context — they never touch the manager.  The manager
 * discovers completion by stat'ing the path on the next pump/request. */

/* Atomically publish a binary `.idx` payload at `path`.  Writes to a
 * sibling .tmp first, renames over.  fsync's the parent directory so
 * the rename is durable across power loss; this matters less for an
 * index cache (we'd just refetch) but the cost is one syscall.
 *
 * Returns NULL on success, or a short error string for the caller's
 * log line (caller frees nothing — the string is static). */
static const char *dp_atomic_write_idx(const char *path,
      const uint8_t *buf, size_t buf_len)
{
   char  output_dir[DP_THUMBS_PATH_MAX];
   char  tmp_path[DP_THUMBS_PATH_MAX];

   if (!path || !*path || !buf || buf_len == 0)
      return "bad args";

   strlcpy(output_dir, path, sizeof(output_dir));
   path_basedir_wrapper(output_dir);
   if (!path_mkdir(output_dir))
      return "mkdir failed";

   snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);
   if (!filestream_write_file(tmp_path, buf, (int64_t)buf_len))
      return "write failed";
#ifdef _WIN32
   /* rename() refuses to clobber on Windows; remove first. */
   filestream_delete(path);
#endif
   if (rename(tmp_path, path) != 0)
   {
      filestream_delete(tmp_path);
      return "rename failed";
   }
#ifndef _WIN32
   /* fsync the parent directory so the rename survives power loss.
    * Best-effort: ENOTDIR / ENOSYS / sandboxed paths just leave us
    * with the same durability as the previous code path. */
   {
      int dir_fd = open(output_dir, O_RDONLY
#ifdef O_DIRECTORY
            | O_DIRECTORY
#endif
            );
      if (dir_fd >= 0)
      {
         (void)fsync(dir_fd);
         close(dir_fd);
      }
   }
#endif
   return NULL;
}

static void dp_cb_index_download(retro_task_t *task, void *task_data,
      void *user_data, const char *err)
{
   http_transfer_data_t *data    = (http_transfer_data_t*)task_data;
   file_transfer_t      *transf  = (file_transfer_t*)user_data;
   uint8_t              *json    = NULL;
   size_t                json_len = 0;
   uint8_t              *idx_buf = NULL;
   size_t                idx_len = 0;
   const char           *werr;
   (void)task;

   if (!transf)
      return;
   if (!data || !data->data || !*transf->path)
      goto finish;
   if (data->status != 200)
   {
      err = "non-200";
      goto finish;
   }
   /* Compressed-size gate: matches dp_cb_pf_index_download.  dp_gunzip
    * also caps internally, but the early-out avoids holding a multi-MB
    * CDN error page in memory while we look at it. */
   if (data->len > DP_THUMBS_PF_INDEX_MAX_BYTES)
   {
      err = "response too large";
      goto finish;
   }
   /* Decompress to JSON, parse straight into the binary `.idx` form,
    * write that to disk.  JSON never touches the cache. */
   json = dp_gunzip((const uint8_t*)data->data, (size_t)data->len, &json_len);
   if (!json || json_len == 0)
   {
      err = "gunzip failed";
      goto finish;
   }
   if (!dp_idx_parse_json_to_buffer((const char*)json, json_len,
            &idx_buf, &idx_len))
   {
      err = "parse failed";
      goto finish;
   }

   werr = dp_atomic_write_idx(transf->path, idx_buf, idx_len);
   if (werr)
   {
      err = werr;
      goto finish;
   }

finish:
   if (err && *err)
      RARCH_WARN("[Downplay] thumbs index \"%s\" failed: %s\n",
            transf->path, err);
   else
      RARCH_LOG("[Downplay] thumbs index -> %s\n", transf->path);
   free(json);
   free(idx_buf);
   free(transf);
}

static void dp_cb_image_download(retro_task_t *task, void *task_data,
      void *user_data, const char *err)
{
   http_transfer_data_t *data   = (http_transfer_data_t*)task_data;
   file_transfer_t      *transf = (file_transfer_t*)user_data;
   char                  output_dir[DP_THUMBS_PATH_MAX];
   char                  tmp_path[DP_THUMBS_PATH_MAX];
   (void)task;

   if (!transf)
      return;
   if (!data || !data->data || !*transf->path)
      goto finish;
   if (data->status != 200)
   {
      err = "non-200";
      goto finish;
   }
   /* Reject payloads whose magic bytes don't match the requested
    * format.  A 200 OK with HTML or zero bytes (CDN error page,
    * typo-squat, content-type confusion) would otherwise get cached
    * and handed to the image decoder on every frame for a week
    * (cache TTL).  Path extension picks which sniff to apply:
    *   .jpg  → SOI must be FF D8
    *   .webp → RIFF....WEBP (12-byte container header) */
   {
      const unsigned char *p = (const unsigned char*)data->data;
      const char          *ext = strrchr(transf->path, '.');
      bool ok = false;
      if (ext && data->len >= 12 && !strcmp(ext, ".webp"))
      {
         ok = (p[0] == 'R' && p[1] == 'I' && p[2] == 'F' && p[3] == 'F'
            && p[8] == 'W' && p[9] == 'E' && p[10] == 'B' && p[11] == 'P');
         if (!ok)
            err = "not a WebP";
      }
      else
      {
         ok = (data->len >= 4 && p[0] == 0xFF && p[1] == 0xD8);
         if (!ok)
            err = "not a JPEG";
      }
      if (!ok)
         goto finish;
   }

   strlcpy(output_dir, transf->path, sizeof(output_dir));
   path_basedir_wrapper(output_dir);
   if (!path_mkdir(output_dir))
   {
      err = "mkdir failed";
      goto finish;
   }

   /* Atomic write: .tmp + rename.  Without this, a crash mid-write
    * leaves a truncated JPEG cached at the canonical path; the next
    * pump sees the file-exists, marks ON_DISK, and the bad image
    * stays cached for the full TTL.  Mirrors the index-fetch path. */
   snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", transf->path);
   if (!filestream_write_file(tmp_path, data->data, data->len))
   {
      err = "write failed";
      goto finish;
   }
#ifdef _WIN32
   filestream_delete(transf->path);
#endif
   if (rename(tmp_path, transf->path) != 0)
   {
      filestream_delete(tmp_path);
      err = "rename failed";
      goto finish;
   }

finish:
   if (err && *err)
      RARCH_WARN("[Downplay] thumbs image \"%s\" failed: %s\n",
            transf->path, err);
   free(transf);
}

/* ---- internal: miss log ----
 *
 * Records definitive misses (index loaded, basename matched nothing)
 * to a TSV file under <root>/Downplay/Thumbs/misses.log.  In-memory
 * dedup per manager session avoids per-frame spam on the active row.
 * Diagnostic only — failure to write is silent. */

static bool dp_miss_already_logged(downplay_thumbs_t *t, const char *basename)
{
   size_t i;
   for (i = 0; i < t->logged_misses_count; i++)
   {
      if (!strcmp(t->logged_misses[i], basename))
         return true;
   }
   return false;
}

static void dp_log_miss(downplay_thumbs_t *t, const char *basename)
{
   FILE      *f;
   char     **r;
   size_t     new_cap;
   time_t     now;
   struct tm *tmv;
   char      *copy;

   if (!*t->log_path)
      return;
   if (dp_miss_already_logged(t, basename))
      return;

   /* Grow the dedup set first; if that fails, skip the log entry —
    * keeps memory bounded if some pathological folder has thousands
    * of unmatched ROMs (debug only, no point growing forever). */
   if (t->logged_misses_count == t->logged_misses_cap)
   {
      new_cap = t->logged_misses_cap ? t->logged_misses_cap * 2 : 32;
      if (new_cap > 4096)
         return;
      r = (char**)realloc(t->logged_misses, new_cap * sizeof(*r));
      if (!r)
         return;
      t->logged_misses     = r;
      t->logged_misses_cap = new_cap;
   }
   copy = strdup(basename);
   if (!copy)
      return;
   t->logged_misses[t->logged_misses_count++] = copy;

   /* Append a TSV row.  POSIX fopen("a") rather than libretro-common's
    * filestream API because we want O_APPEND atomic-line semantics
    * and don't need VFS abstraction for a debug log. */
   f = fopen(t->log_path, "a");
   if (!f)
      return;
   now = time(NULL);
   tmv = localtime(&now);
   if (tmv)
      fprintf(f, "%04d-%02d-%02d %02d:%02d:%02d\t%s\t%s\n",
            tmv->tm_year + 1900, tmv->tm_mon + 1, tmv->tm_mday,
            tmv->tm_hour, tmv->tm_min, tmv->tm_sec,
            t->system, basename);
   else
      fprintf(f, "?\t%s\t%s\n", t->system, basename);
   fclose(f);
}

/* ---- internal: state advancement ---- */

/* Compute index file age in seconds; returns -1 if missing.
 * libretro-common's RFILE wrapper has no portable mtime accessor, so
 * fall back to POSIX stat — every Downplay target (Android, macOS,
 * Linux) supports it; on Windows the MS CRT exposes the same API
 * under the same name. */
static int64_t dp_index_age_seconds(const char *path)
{
   struct stat st;
   time_t      now;
   if (!path_is_valid(path))
      return -1;
   if (stat(path, &st) != 0)
      return -1;
   now = time(NULL);
   if (st.st_mtime > now)
      return 0;
   return (int64_t)(now - st.st_mtime);
}

/* Try to load the on-disk binary index into the manager.  Reads the
 * `.idx` file into a malloc'd buffer (single read, no JSON parse),
 * validates it via dp_idx_open, and installs.  Returns true on
 * success (sets load_state=READY).  Leaves state unchanged on
 * failure so caller can decide between FETCHING (still waiting) vs
 * FAILED (give up). */
static bool dp_try_load_local_index(downplay_thumbs_t *t)
{
   int64_t  size = 0;
   void    *buf  = NULL;

   if (!path_is_valid(t->idx_path))
      return false;
   if (!filestream_read_file(t->idx_path, &buf, &size) || !buf || size <= 0)
   {
      free(buf);
      return false;
   }
   /* dp_idx_open takes ownership of buf — frees on validation failure. */
   t->index = dp_idx_open((uint8_t*)buf, (size_t)size);
   if (!t->index)
   {
      RARCH_WARN("[Downplay] thumbs: validate failed for %s\n", t->idx_path);
      return false;
   }
   t->attempt = (uint8_t*)calloc(t->index->entry_count, 1);
   if (!t->attempt)
   {
      downplay_thumbs_index_free(t->index);
      t->index = NULL;
      return false;
   }
   t->load_state = DP_LOAD_READY;
   return true;
}

/* Push the index HTTP fetch.  Caller should set state=FETCHING. */
static void dp_kick_index_fetch(downplay_thumbs_t *t)
{
   file_transfer_t *transf;
   char raw_url[2048];
   char url[2048];

   /* Server hosts a gzipped index alongside the JSON; ~10x smaller
    * over the wire.  Cache writes the decompressed JSON to disk so
    * the parse path is unaffected. */
   snprintf(raw_url, sizeof(raw_url),
         "%s/%s/Named_Boxarts/index.json.gz",
         DP_THUMBS_BASE_URL, t->system);
   net_http_urlencode_full(url, raw_url, sizeof(url));
   if (!*url)
      return;

   transf = (file_transfer_t*)calloc(1, sizeof(*transf));
   if (!transf)
      return;
   transf->enum_idx = MSG_UNKNOWN;
   strlcpy(transf->path, t->idx_path, sizeof(transf->path));

   RARCH_LOG("[Downplay] thumbs index fetch: %s\n", url);
   if (!task_push_http_transfer_file(url, true /* mute */, NULL,
            dp_cb_index_download, transf))
   {
      RARCH_WARN("[Downplay] thumbs index task push failed: %s\n", url);
      free(transf);
   }
   t->index_task_pushed = true;
}

/* Push entry_idx onto the queue if not already there.  pri=1 (active)
 * goes to the head (next-out); pri=0 (prefetch) to the tail.
 *
 * On a full queue we drop the *oldest prefetch entry* (sequentially
 * scanning back from the tail) to make room for an active request;
 * a prefetch overflow is dropped silently.  Naive "decrement tail"
 * would corrupt the contents at the new head — see code-review. */
static void dp_queue_push(downplay_thumbs_t *t, uint32_t entry_idx, uint8_t pri)
{
   int i;
   /* De-dup: skip if already queued. */
   for (i = t->queue_head; i != t->queue_tail;
         i = (i + 1) % DP_THUMBS_QUEUE_CAP)
   {
      if (t->queue[i] == entry_idx)
         return;
   }
   /* Full? */
   if (((t->queue_tail + 1) % DP_THUMBS_QUEUE_CAP) == t->queue_head)
   {
      int j;
      int found;
      if (pri != 1)
         return; /* prefetch overflow — drop. */
      /* Active request, queue full: walk forward from head, find the
       * first prefetch entry, and shift everything after it back by
       * one to overwrite it.  This preserves all active entries and
       * drops one prefetch (the oldest). */
      found = -1;
      for (i = t->queue_head; i != t->queue_tail;
            i = (i + 1) % DP_THUMBS_QUEUE_CAP)
      {
         if (t->queue_pri[i] == 0)
         {
            found = i;
            break;
         }
      }
      if (found < 0)
      {
         /* All entries are active: drop the oldest one (tail-1). */
         t->queue_tail = (t->queue_tail - 1 + DP_THUMBS_QUEUE_CAP)
               % DP_THUMBS_QUEUE_CAP;
      }
      else
      {
         /* Shift [found+1 .. tail) one slot left over `found`. */
         j = found;
         for (;;)
         {
            int next = (j + 1) % DP_THUMBS_QUEUE_CAP;
            if (next == t->queue_tail)
               break;
            t->queue[j]     = t->queue[next];
            t->queue_pri[j] = t->queue_pri[next];
            j = next;
         }
         t->queue_tail = (t->queue_tail - 1 + DP_THUMBS_QUEUE_CAP)
               % DP_THUMBS_QUEUE_CAP;
      }
   }
   if (pri == 1)
   {
      /* Insert at head: rotate head back into the slot we just freed. */
      t->queue_head = (t->queue_head - 1 + DP_THUMBS_QUEUE_CAP)
            % DP_THUMBS_QUEUE_CAP;
      t->queue[t->queue_head]     = entry_idx;
      t->queue_pri[t->queue_head] = 1;
   }
   else
   {
      t->queue[t->queue_tail]     = entry_idx;
      t->queue_pri[t->queue_tail] = 0;
      t->queue_tail = (t->queue_tail + 1) % DP_THUMBS_QUEUE_CAP;
   }
}

/* Stat each currently-fetching entry; if its file has landed,
 * compact the entry out of the fetching[] set and decrement inflight.
 * O(inflight) — bounded to DP_THUMBS_INFLIGHT_MAX, not O(entries). */
static void dp_reap_inflight(downplay_thumbs_t *t)
{
   int  i = 0;
   char path[DP_THUMBS_PATH_MAX];
   while (i < t->inflight)
   {
      uint32_t e_idx = t->fetching[i];
      if (e_idx >= t->index->entry_count)
      {
         /* Defensive: invalid entry — drop. */
         t->fetching[i] = t->fetching[--t->inflight];
         continue;
      }
      dp_build_image_path(t, dp_idx_canonical(t->index, e_idx),
            path, sizeof(path));
      if (path_is_valid(path))
      {
         t->attempt[e_idx] = DP_ATT_ON_DISK;
         t->fetching[i]    = t->fetching[--t->inflight];
         continue;
      }
      i++;
   }
}

/* Drain one queue slot to an in-flight HTTP task if we're below the
 * concurrency cap.  Called from pump.  Caller is responsible for
 * looping (within budget) — this only dispatches at most one task. */
static void dp_drain_queue(downplay_thumbs_t *t)
{
   uint32_t e_idx;
   const char *canonical;
   char path[DP_THUMBS_PATH_MAX];
   char url[2048];
   file_transfer_t *transf;

   if (t->load_state != DP_LOAD_READY)
      return;
   if (t->queue_head == t->queue_tail)
      return;
   if (t->inflight >= DP_THUMBS_INFLIGHT_MAX)
      return;

   e_idx = t->queue[t->queue_head];
   t->queue_head = (t->queue_head + 1) % DP_THUMBS_QUEUE_CAP;

   if (e_idx >= t->index->entry_count)
      return;
   /* Re-check state: row may have transitioned to ON_DISK since enqueue. */
   if (t->attempt[e_idx] != DP_ATT_UNTRIED
       && t->attempt[e_idx] != DP_ATT_FETCHING)
      return;

   /* Lazy on-disk check.  This entry was queued without an upfront
    * stat-sweep proving it was missing, so recheck now — the user
    * may have triggered a fetch for an image already cached on disk
    * (webp landed via a different code path, etc.). */
   if (dp_resolve_local_image(t, e_idx, path, sizeof(path)))
   {
      t->attempt[e_idx] = DP_ATT_ON_DISK;
      return;
   }
   canonical = dp_idx_canonical(t->index, e_idx);
   dp_build_image_url(t, canonical, url, sizeof(url));
   if (!*url)
      return;
   transf = (file_transfer_t*)calloc(1, sizeof(*transf));
   if (!transf)
      return;
   transf->enum_idx = MSG_UNKNOWN;
   strlcpy(transf->path, path, sizeof(transf->path));
   if (!task_push_http_transfer_file(url, true /* mute */, NULL,
            dp_cb_image_download, transf))
   {
      free(transf);
      t->attempt[e_idx] = DP_ATT_FAILED;
      return;
   }
   t->attempt[e_idx]      = DP_ATT_FETCHING;
   t->fetching[t->inflight++] = e_idx;
}

/* ---- index prefetch (boot-time fan-out) ---------------------------
 *
 * Drive a small global queue of <root>/Thumbs/index/<system>.idx
 * fetches at app launch, before the user can navigate.  By the time
 * they enter any system view, the index is already on disk and
 * `_open` skips its own HTTP call.
 *
 * THREADING INVARIANT: every entry below (g_pf_pending, g_pf_inflight,
 * the count ints, and every helper that touches them) is main-thread
 * only.  RA's task callbacks fire from `retro_task_internal_gather`,
 * which is reached only from `task_queue_check`; that in turn is
 * called from main-thread sites (runloop + menu select-handler).
 * Both threaded and non-threaded task modes funnel through this one
 * gather point.  Therefore no locks are required.  If a future
 * maintainer ever moves task_queue_check off the main thread, this
 * block needs an slock — the corruption would otherwise be silent. */

#define DP_THUMBS_PF_PENDING_CAP   128
#define DP_THUMBS_PF_INFLIGHT_MAX  3
/* DP_THUMBS_PF_INDEX_MAX_BYTES lives near the gunzip helper above
 * (the compressed payload cap is also the input to the decompressed
 * cap calculation).  Both callbacks gate on it. */

static char g_pf_pending[DP_THUMBS_PF_PENDING_CAP][256];
static int  g_pf_pending_count;
static bool g_pf_overflow_warned; /* one-shot log when cap is first hit */

static char g_pf_inflight[DP_THUMBS_PF_INFLIGHT_MAX][256];
static int  g_pf_inflight_count;

/* Subtype of file_transfer_t (must be first member): RA's task API
 * takes file_transfer_t* and reads .path on completion; we tail-extend
 * with the canonical system name so the callback can update the
 * in-flight set without re-parsing the path. */
typedef struct
{
   file_transfer_t base;          /* must be first; .path is the on-disk dest */
   char            system[256];   /* canonical name; key for inflight set */
} dp_pf_transfer_t;

/* Reject malformed system strings.  Path-escape (..  /  \) was the
 * obvious vector; NUL / control chars in the middle are a subtler one
 * — they'd silently truncate our `g_pf_inflight` keys (kernel terminates
 * at NUL) and let two concurrent fetches dodge dedup with different
 * effective names but the same ToS prefix. */
static bool dp_pf_system_safe(const char *system)
{
   const unsigned char *p;
   if (!system || !*system)
      return false;
   if (   strstr(system, "..")
       || strchr(system, '/')
       || strchr(system, '\\'))
      return false;
   for (p = (const unsigned char*)system; *p; p++)
      if (*p < 0x20 || *p == 0x7F)
         return false;
   return true;
}

static bool dp_pf_inflight_contains(const char *system)
{
   int i;
   if (!system)
      return false;
   for (i = 0; i < g_pf_inflight_count; i++)
   {
      if (string_is_equal(g_pf_inflight[i], system))
         return true;
   }
   return false;
}

static bool dp_pf_pending_contains(const char *system)
{
   int i;
   if (!system)
      return false;
   for (i = 0; i < g_pf_pending_count; i++)
   {
      if (string_is_equal(g_pf_pending[i], system))
         return true;
   }
   return false;
}

static bool dp_pf_inflight_add(const char *system)
{
   if (g_pf_inflight_count >= DP_THUMBS_PF_INFLIGHT_MAX)
      return false;
   strlcpy(g_pf_inflight[g_pf_inflight_count], system,
         sizeof(g_pf_inflight[0]));
   g_pf_inflight_count++;
   return true;
}

static void dp_pf_inflight_remove(const char *system)
{
   int i;
   if (!system)
      return;
   for (i = 0; i < g_pf_inflight_count; i++)
   {
      if (string_is_equal(g_pf_inflight[i], system))
      {
         /* Compact: move last entry into this slot. */
         g_pf_inflight_count--;
         if (i != g_pf_inflight_count)
            strlcpy(g_pf_inflight[i],
                  g_pf_inflight[g_pf_inflight_count],
                  sizeof(g_pf_inflight[0]));
         g_pf_inflight[g_pf_inflight_count][0] = '\0';
         return;
      }
   }
}

/* Remove `system` from pending if present.  Used when a user-initiated
 * `_open` arrives for a system that was queued but not yet started:
 * we pull it out of pending and kick the fetch directly so the user
 * doesn't wait behind unrelated systems already inflight.  Returns
 * true if the entry was found and removed. */
static bool dp_pf_pending_remove(const char *system)
{
   int i;
   int j;
   if (!system)
      return false;
   for (i = 0; i < g_pf_pending_count; i++)
   {
      if (!string_is_equal(g_pf_pending[i], system))
         continue;
      for (j = i + 1; j < g_pf_pending_count; j++)
         strlcpy(g_pf_pending[j - 1], g_pf_pending[j],
               sizeof(g_pf_pending[0]));
      g_pf_pending_count--;
      g_pf_pending[g_pf_pending_count][0] = '\0';
      return true;
   }
   return false;
}

static bool dp_pf_pending_pop(char *out_system, size_t out_size)
{
   int i;
   if (g_pf_pending_count <= 0)
      return false;
   strlcpy(out_system, g_pf_pending[0], out_size);
   for (i = 1; i < g_pf_pending_count; i++)
      strlcpy(g_pf_pending[i - 1], g_pf_pending[i],
            sizeof(g_pf_pending[0]));
   g_pf_pending_count--;
   g_pf_pending[g_pf_pending_count][0] = '\0';
   return true;
}

static void dp_pf_drain(void); /* fwd */

static void dp_cb_pf_index_download(retro_task_t *task, void *task_data,
      void *user_data, const char *err)
{
   http_transfer_data_t *data    = (http_transfer_data_t*)task_data;
   dp_pf_transfer_t     *pf      = (dp_pf_transfer_t*)user_data;
   uint8_t              *json    = NULL;
   size_t                json_len = 0;
   uint8_t              *idx_buf = NULL;
   size_t                idx_len = 0;
   const char           *werr;
   (void)task;

   retro_assert(task_is_on_main_thread());
   if (!pf)
      return;
   if (!data || !data->data || !*pf->base.path)
      goto finish;
   if (data->status != 200)
   {
      err = "non-200";
      goto finish;
   }
   if (data->len > DP_THUMBS_PF_INDEX_MAX_BYTES)
   {
      err = "response too large";
      goto finish;
   }
   /* Same JSON → .idx path as dp_cb_index_download.  We never write
    * the JSON or any intermediate form to disk — the binary file IS
    * the cache. */
   json = dp_gunzip((const uint8_t*)data->data, (size_t)data->len, &json_len);
   if (!json || json_len == 0)
   {
      err = "gunzip failed";
      goto finish;
   }
   if (!dp_idx_parse_json_to_buffer((const char*)json, json_len,
            &idx_buf, &idx_len))
   {
      err = "parse failed";
      goto finish;
   }
   werr = dp_atomic_write_idx(pf->base.path, idx_buf, idx_len);
   if (werr)
   {
      err = werr;
      goto finish;
   }

finish:
   if (err && *err)
      RARCH_WARN("[Downplay] thumbs prefetch \"%s\" failed: %s\n",
            pf->system, err);
   else
      RARCH_LOG("[Downplay] thumbs prefetch -> %s\n", pf->base.path);
   free(json);
   free(idx_buf);
   dp_pf_inflight_remove(pf->system);
   free(pf);
   dp_pf_drain();
}

/* Drain the pending queue up to the concurrency cap. */
static void dp_pf_drain(void)
{
   retro_assert(task_is_on_main_thread());
   while (g_pf_inflight_count < DP_THUMBS_PF_INFLIGHT_MAX
         && g_pf_pending_count > 0)
   {
      char system[256];
      char idx_path[DP_THUMBS_PATH_MAX];
      char raw_url[2048];
      char url[2048];
      dp_pf_transfer_t *pf;
      int64_t age;

      if (!dp_pf_pending_pop(system, sizeof(system)))
         break;

      /* Re-validate before constructing the URL.  Defence-in-depth:
       * the public-API path filters at enqueue, but if any future
       * code path inserts directly into g_pf_pending this stops a
       * bogus name from reaching the wire. */
      if (!dp_pf_system_safe(system))
         continue;
      /* Re-check freshness right before issuing — the file may have
       * landed in the window between enqueue and drain (e.g. a user
       * `_open` raced ahead of us and won). */
      if (!dp_thumbs_index_path(system, idx_path, sizeof(idx_path)))
         continue;
      age = dp_index_age_seconds(idx_path);
      if (age >= 0 && age < DP_THUMBS_INDEX_TTL_SEC)
         continue;

      snprintf(raw_url, sizeof(raw_url),
            "%s/%s/Named_Boxarts/index.json.gz",
            DP_THUMBS_BASE_URL, system);
      net_http_urlencode_full(url, raw_url, sizeof(url));
      if (!*url)
         continue;

      pf = (dp_pf_transfer_t*)calloc(1, sizeof(*pf));
      if (!pf)
         continue;
      pf->base.enum_idx = MSG_UNKNOWN;
      strlcpy(pf->base.path, idx_path, sizeof(pf->base.path));
      strlcpy(pf->system,    system,   sizeof(pf->system));

      if (!dp_pf_inflight_add(system))
      {
         free(pf);
         continue;
      }
      RARCH_LOG("[Downplay] thumbs prefetch fetch: %s\n", url);
      if (!task_push_http_transfer_file(url, true /* mute */, NULL,
               dp_cb_pf_index_download, &pf->base))
      {
         RARCH_WARN("[Downplay] thumbs prefetch task push failed: %s\n",
               url);
         dp_pf_inflight_remove(system);
         free(pf);
      }
   }
}

void downplay_thumbs_prefetch_indexes(
      const char * const *systems, size_t count)
{
   size_t i;
   retro_assert(task_is_on_main_thread());
   if (!systems || !count)
      return;
   /* Reset the one-shot overflow log: callers are entitled to know
    * about a fresh-call overflow even if a prior call hit the cap. */
   g_pf_overflow_warned = false;
   for (i = 0; i < count; i++)
   {
      const char *system = systems[i];
      char idx_path[DP_THUMBS_PATH_MAX];
      int64_t age;

      if (!dp_pf_system_safe(system))
         continue;
      /* Bound: must fit our fixed-size slot. */
      if (strlen(system) >= sizeof(g_pf_pending[0]))
         continue;
      /* Skip if fresh on disk. */
      if (!dp_thumbs_index_path(system, idx_path, sizeof(idx_path)))
         continue;
      age = dp_index_age_seconds(idx_path);
      if (age >= 0 && age < DP_THUMBS_INDEX_TTL_SEC)
         continue;
      /* Skip if already in flight or already queued. */
      if (dp_pf_inflight_contains(system))
         continue;
      if (dp_pf_pending_contains(system))
         continue;
      if (g_pf_pending_count >= DP_THUMBS_PF_PENDING_CAP)
      {
         /* Capacity is generous (128 systems); a hit means something
          * unusual.  Warn once so we have a bread crumb in logcat. */
         if (!g_pf_overflow_warned)
         {
            RARCH_WARN("[Downplay] thumbs prefetch queue full at %d; "
                  "remaining systems will not be prefetched\n",
                  DP_THUMBS_PF_PENDING_CAP);
            g_pf_overflow_warned = true;
         }
         continue;
      }
      strlcpy(g_pf_pending[g_pf_pending_count], system,
            sizeof(g_pf_pending[0]));
      g_pf_pending_count++;
   }
   dp_pf_drain();
}

/* ---- public manager API ---- */

downplay_thumbs_t *downplay_thumbs_open(const char *system)
{
   downplay_thumbs_t *t;
   char root[DP_THUMBS_PATH_MAX];
   char base[DP_THUMBS_PATH_MAX];

   if (!system || !*system)
      return NULL;
   /* `system` is used as both a URL path component AND a filesystem
    * directory name; reject anything that could escape either.  In
    * the common path it comes from the curated disambig table, but
    * the core_info `database` fallback is third-party content. */
   if (   strstr(system, "..")
       || strchr(system, '/')
       || strchr(system, '\\'))
      return NULL;
   if (!downplay_paths_get_root(root, sizeof(root)))
      return NULL;

   t = (downplay_thumbs_t*)calloc(1, sizeof(*t));
   if (!t)
      return NULL;

   strlcpy(t->system, system, sizeof(t->system));

   /* <root>/Thumbs/ */
   fill_pathname_join_special(base, root, "Thumbs", sizeof(base));
   /* <root>/Thumbs/index/<system>.idx — via shared helper so boot-
    * time prefetch and per-system open agree on the location. */
   if (!dp_thumbs_index_path(system, t->idx_path, sizeof(t->idx_path)))
   {
      free(t);
      return NULL;
   }
   /* <root>/Thumbs/<system>/ */
   fill_pathname_join_special(t->cache_dir, base, system,
         sizeof(t->cache_dir));
   /* <root>/Thumbs/misses.log */
   fill_pathname_join_special(t->log_path, base, "misses.log",
         sizeof(t->log_path));

   t->queue_head = t->queue_tail = 0;

   /* If on-disk index is present and fresh, parse synchronously.
    * Otherwise fire HTTP fetch. */
   {
      int64_t age = dp_index_age_seconds(t->idx_path);
      if (age >= 0 && age < DP_THUMBS_INDEX_TTL_SEC)
      {
         if (dp_try_load_local_index(t))
            return t;
      }
   }
   t->load_state = DP_LOAD_FETCHING;
   /* Priority promotion: if this system is queued in the boot-time
    * prefetch but hasn't started yet, the user shouldn't wait behind
    * unrelated systems already in flight.  Pull it out of pending and
    * kick the fetch ourselves — the prefetch pool's concurrency cap
    * doesn't apply to user-initiated fetches.  If it's already in
    * flight (callback will land soon) we suppress to avoid a duplicate
    * HTTP transfer; both prefetch and direct callbacks atomic-rename
    * to the same `idx_path`, so racing them is correct but wasteful. */
   dp_pf_pending_remove(system);
   if (!dp_pf_inflight_contains(system))
      dp_kick_index_fetch(t);
   return t;
}

void downplay_thumbs_close(downplay_thumbs_t *t)
{
   if (!t)
      return;
   /* In-flight HTTP tasks for this manager's images have user_data
    * that's a self-contained file_transfer_t — they don't reference
    * the manager, so closing is safe. */
   downplay_thumbs_index_free(t->index);
   free(t->attempt);
   {
      size_t i;
      for (i = 0; i < t->logged_misses_count; i++)
         free(t->logged_misses[i]);
      free(t->logged_misses);
   }
   free(t);
}

void downplay_thumbs_request(downplay_thumbs_t *t,
      const char *rom_basename,
      downplay_thumb_result_t *out)
{
   int e_idx;

   if (!out)
      return;
   out->status        = DP_THUMB_UNKNOWN;
   out->local_path[0] = '\0';
   if (!t || !rom_basename || !*rom_basename)
      return;

   /* Maybe the index just landed.  Cheap: stat once. */
   if (t->load_state == DP_LOAD_FETCHING && !t->index
       && path_is_valid(t->idx_path))
   {
      if (!dp_try_load_local_index(t))
      {
         /* File present but parse failed — give up.  A future open()
          * after the cached file is replaced (TTL expiry, manual
          * delete) will retry. */
         t->load_state = DP_LOAD_FAILED;
      }
   }

   if (t->load_state != DP_LOAD_READY)
      return; /* UNKNOWN — caller polls again next frame */

   {
      size_t mi = dp_index_match_idx(t->index, rom_basename);
      if (mi == (size_t)-1)
      {
         out->status = DP_THUMB_MISSING;
         dp_log_miss(t, rom_basename);
         return;
      }
      e_idx = (int)mi;
   }

   if (t->attempt[e_idx] == DP_ATT_ON_DISK)
   {
      dp_build_image_path(t, dp_idx_canonical(t->index, (uint32_t)e_idx),
            out->local_path, sizeof(out->local_path));
      out->status = DP_THUMB_OK;
      return;
   }
   if (dp_resolve_local_image(t, (uint32_t)e_idx,
            out->local_path, sizeof(out->local_path)))
   {
      t->attempt[e_idx] = DP_ATT_ON_DISK;
      out->status = DP_THUMB_OK;
      return;
   }

   if (t->attempt[e_idx] == DP_ATT_FAILED)
   {
      /* We tried to push the HTTP task and the queue/network rejected
       * it.  Surface as MISSING (per-row art will not appear) rather
       * than leaving the row stuck at UNKNOWN forever. */
      out->status = DP_THUMB_MISSING;
      out->local_path[0] = '\0';
      return;
   }

   /* Not on disk and not yet tried; promote to active queue.  pri=1.
    * If currently FETCHING we leave it alone — pump will discover
    * landing on a future frame. */
   if (t->attempt[e_idx] == DP_ATT_UNTRIED)
      dp_queue_push(t, (uint32_t)e_idx, 1);
   /* status stays UNKNOWN — next frame will see ON_DISK once landed. */
}

void downplay_thumbs_prefetch(downplay_thumbs_t *t,
      const char * const *basenames, size_t count)
{
   size_t i;
   if (!t || t->load_state != DP_LOAD_READY || !basenames)
      return;
   for (i = 0; i < count; i++)
   {
      size_t mi;
      if (!basenames[i])
         continue;
      mi = dp_index_match_idx(t->index, basenames[i]);
      if (mi == (size_t)-1)
         continue;
      if (t->attempt[mi] != DP_ATT_UNTRIED)
         continue;
      dp_queue_push(t, (uint32_t)mi, 0 /* prefetch */);
   }
}

void downplay_thumbs_pump(downplay_thumbs_t *t)
{
   int budget;
   if (!t)
      return;
   if (t->load_state == DP_LOAD_FETCHING && !t->index
       && path_is_valid(t->idx_path))
   {
      if (!dp_try_load_local_index(t))
         t->load_state = DP_LOAD_FAILED;
   }
   if (t->load_state != DP_LOAD_READY)
      return;
   /* Reap completed fetches first to free in-flight slots. */
   dp_reap_inflight(t);
   /* Drain queued requests up to the concurrency cap. */
   for (budget = DP_THUMBS_INFLIGHT_MAX; budget > 0; budget--)
   {
      int before_head = t->queue_head;
      if (t->inflight >= DP_THUMBS_INFLIGHT_MAX)
         break;
      dp_drain_queue(t);
      if (t->queue_head == before_head)
         break;
   }
}

/* ---- recents resolver -----------------------------------------
 *
 * Lazy multi-system reader.  Holds at most one parsed binary index
 * per distinct system seen across the recents view.  No HTTP, no
 * pump, no prefetch — if a system's `.idx` isn't on disk yet the
 * row simply renders without art.  Designed so the recents view
 * does the minimum work to surface a thumbnail when one's already
 * cached (i.e. the user has visited that system before).
 *
 * Lifecycle: open on view enter, resolve per row per frame, close
 * on view exit.  Slot table grows by doubling; expected steady-
 * state size is the small set of distinct systems in the user's
 * recently-played list (typically <10). */

typedef struct
{
   char                    *system;     /* heap, owned */
   downplay_thumbs_index_t *index;      /* NULL on miss/parse-fail */
   bool                     tried;      /* don't retry the load every frame */
} dp_recents_slot_t;

struct downplay_thumbs_recents
{
   dp_recents_slot_t *slots;
   size_t             count;
   size_t             cap;
};

downplay_thumbs_recents_t *downplay_thumbs_recents_open(void)
{
   return (downplay_thumbs_recents_t*)calloc(1,
         sizeof(downplay_thumbs_recents_t));
}

void downplay_thumbs_recents_close(downplay_thumbs_recents_t *r)
{
   size_t i;
   if (!r)
      return;
   for (i = 0; i < r->count; i++)
   {
      free(r->slots[i].system);
      downplay_thumbs_index_free(r->slots[i].index);
   }
   free(r->slots);
   free(r);
}

/* Find or insert the slot for `system`.  On insert, sets tried=false
 * so the next resolve attempts a load exactly once.  Linear scan is
 * fine here — distinct-systems count is in the single digits in
 * practice.  Returns NULL on allocation failure. */
static dp_recents_slot_t *dp_recents_get_slot(
      downplay_thumbs_recents_t *r, const char *system)
{
   size_t i;
   dp_recents_slot_t *s;
   for (i = 0; i < r->count; i++)
      if (string_is_equal(r->slots[i].system, system))
         return &r->slots[i];
   if (r->count == r->cap)
   {
      size_t             new_cap = r->cap ? r->cap * 2 : 8;
      dp_recents_slot_t *n       = (dp_recents_slot_t*)realloc(
            r->slots, new_cap * sizeof(*n));
      if (!n)
         return NULL;
      r->slots = n;
      r->cap   = new_cap;
   }
   s = &r->slots[r->count];
   memset(s, 0, sizeof(*s));
   s->system = strdup(system);
   if (!s->system)
      return NULL;
   r->count++;
   return s;
}

bool downplay_thumbs_recents_resolve(downplay_thumbs_recents_t *r,
      const char *system, const char *rom_basename,
      char *out, size_t out_size)
{
   dp_recents_slot_t *slot;
   size_t             mi;
   char               root[DP_THUMBS_PATH_MAX];
   char               base[DP_THUMBS_PATH_MAX];
   char               cache_dir[DP_THUMBS_PATH_MAX];
   char               tmp[DP_THUMBS_PATH_MAX];

   if (!r || !system || !*system || !rom_basename || !*rom_basename
       || !out || out_size == 0)
      return false;
   /* Same path-traversal guards as downplay_thumbs_open. */
   if (strstr(system, "..")
       || strchr(system, '/') || strchr(system, '\\'))
      return false;

   slot = dp_recents_get_slot(r, system);
   if (!slot)
      return false;

   if (!slot->tried)
   {
      char     idx_path[DP_THUMBS_PATH_MAX];
      slot->tried = true;
      if (dp_thumbs_index_path(system, idx_path, sizeof(idx_path))
          && path_is_valid(idx_path))
      {
         int64_t  size = 0;
         void    *buf  = NULL;
         if (filestream_read_file(idx_path, &buf, &size)
               && buf && size > 0)
            slot->index = dp_idx_open((uint8_t*)buf, (size_t)size);
         else
            free(buf);
      }
   }
   if (!slot->index)
      return false;
   mi = dp_index_match_idx(slot->index, rom_basename);
   if (mi == (size_t)-1)
      return false;

   if (!downplay_paths_get_root(root, sizeof(root)))
      return false;
   fill_pathname_join_special(base, root, "Thumbs", sizeof(base));
   fill_pathname_join_special(cache_dir, base, system, sizeof(cache_dir));
   fill_pathname_join_special(tmp, cache_dir,
         dp_idx_canonical(slot->index, (uint32_t)mi), sizeof(tmp));
   snprintf(out, out_size, "%s.webp", tmp);
   return path_is_valid(out);
}

#else /* DOWNPLAY_THUMBS_TEST_BUILD */

/* Manager API stubs for the unit-test build — tests only exercise the
 * pure match cascade, not the HTTP/IO manager. */
downplay_thumbs_t *downplay_thumbs_open(const char *system)
{ (void)system; return NULL; }
void downplay_thumbs_close(downplay_thumbs_t *t) { (void)t; }
void downplay_thumbs_request(downplay_thumbs_t *t,
      const char *rom_basename, downplay_thumb_result_t *out)
{
   (void)t; (void)rom_basename;
   if (out) { out->status = DP_THUMB_UNKNOWN; out->local_path[0] = '\0'; }
}
void downplay_thumbs_prefetch(downplay_thumbs_t *t,
      const char * const *basenames, size_t count)
{ (void)t; (void)basenames; (void)count; }
void downplay_thumbs_pump(downplay_thumbs_t *t) { (void)t; }
void downplay_thumbs_prefetch_indexes(
      const char * const *systems, size_t count)
{ (void)systems; (void)count; }
downplay_thumbs_recents_t *downplay_thumbs_recents_open(void) { return NULL; }
bool downplay_thumbs_recents_resolve(downplay_thumbs_recents_t *r,
      const char *system, const char *rom_basename,
      char *out, size_t out_size)
{
   (void)r; (void)system; (void)rom_basename;
   if (out && out_size) out[0] = '\0';
   return false;
}
void downplay_thumbs_recents_close(downplay_thumbs_recents_t *r) { (void)r; }

#endif /* DOWNPLAY_THUMBS_TEST_BUILD */
