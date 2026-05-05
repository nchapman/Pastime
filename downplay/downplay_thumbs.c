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

#include <boolean.h>
#include <compat/strl.h>
#include <file/file_path.h>
#include <formats/rjson.h>
#include <net/net_http.h>
#include <streams/file_stream.h>
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
 * the launcher (each entry costs ~500 B of heap once strdup'd). */
#define DP_THUMBS_MAX_ENTRIES 100000u

/* ---------------------------------------------------------------- */
/* Section 1: entry struct + normalization + tiebreak metadata      */
/* ---------------------------------------------------------------- */

typedef struct
{
   char *canonical;     /* heap, exactly the index.json key */
   char *heavy;         /* aggressively-normalized, parens stripped */
   char  disc_token[16];/* "Disc 2" / "CD 1" / "Side B" / "" */
   int   jpg_size;      /* bytes; 0 if .jpg absent in formats */
   int   webp_size;     /* bytes; 0 if .webp absent in formats */
   int   region_score;  /* lower = better; only used on multi-candidate */
   int   rev_num;       /* (Rev N) parsed; 0 if absent */
   bool  bad_dump;      /* (Beta|Proto|Demo|Sample|Pirate|Unl|Hack|...) */
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

/* Comparator for the single (heavy, canonical) sort.  qsort isn't
 * stable; chaining canonical as a secondary key makes the order
 * fully determined by data, so identical inputs always produce the
 * same lookup result across runs and platforms. */
static const dp_thumb_entry_t *g_dp_cmp_base;

static int dp_cmp_heavy(const void *a, const void *b)
{
   uint32_t ia = *(const uint32_t*)a, ib = *(const uint32_t*)b;
   int rv = strcmp(g_dp_cmp_base[ia].heavy, g_dp_cmp_base[ib].heavy);
   if (rv != 0)
      return rv;
   return strcmp(g_dp_cmp_base[ia].canonical, g_dp_cmp_base[ib].canonical);
}

static int dp_cmp_canonical(const void *a, const void *b)
{
   uint32_t ia = *(const uint32_t*)a, ib = *(const uint32_t*)b;
   return strcmp(g_dp_cmp_base[ia].canonical, g_dp_cmp_base[ib].canonical);
}

/* ---------------------------------------------------------------- */
/* Section 2: JSON parse → downplay_thumbs_index_t                  */
/* ---------------------------------------------------------------- */

struct downplay_thumbs_index
{
   dp_thumb_entry_t *entries;
   size_t            entries_count;
   size_t            entries_cap;

   /* Permutation arrays: entries[] indices sorted by the named key.
    * `by_heavy` is the primary lookup index; `by_canonical` lets us
    * answer the exact-canonical fast path in O(log N).  Both use
    * `canonical` as a stable secondary sort for full determinism. */
   uint32_t *by_heavy;
   uint32_t *by_canonical;
};

/* JSON parse state machine.  The input shape:
 *   { "system": "...", "image_type": "...", "files": {
 *       "Title (Region)": { "formats": { "jpg": N, "webp": N }, ... },
 *       ...
 *     }
 *   }
 *
 * We only care about: each key inside "files" (= entry canonical), and
 * its inner formats.jpg number.  Everything else is skipped.
 *
 * State machine markers track depth so we know whether the current
 * member belongs to top-level, "files", an entry object, or its
 * "formats" sub-object. */
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
   downplay_thumbs_index_t *idx;

   dp_parse_state_t state_stack[16];
   int              depth;            /* current depth, 0 == before root */

   /* Accumulator for the entry currently being parsed (in DP_PS_IN_ENTRY
    * or DP_PS_IN_FORMATS).  Strings strdup'd, NULL when no entry. */
   char *cur_canonical;
   int   cur_jpg_size;
   int   cur_webp_size;

   /* Last seen object-member name; consumed by the next value handler. */
   char  last_member[256];

   bool  in_files;     /* did we see "files" at top level? */
   bool  saw_files_obj;

   bool  oom;
} dp_parse_ctx_t;

static bool dp_idx_grow(downplay_thumbs_index_t *idx)
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

/* Append one entry to entries[].  `canonical` is strdup'd; metadata
 * (region/rev/bad_dump/disc) is computed from it.  `heavy` is taken
 * from the supplied buffer so callers can synthesise alt-name keys
 * without re-running normalize on the same canonical.  Returns
 * false on OOM (caller propagates). */
static bool dp_append_entry(downplay_thumbs_index_t *idx,
      const char *canonical, const char *heavy,
      int jpg_size, int webp_size, bool *oom)
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
   e->jpg_size     = jpg_size;
   e->webp_size    = webp_size;
   e->region_score = dp_score_region(canonical);
   e->rev_num      = dp_extract_rev(canonical);
   e->bad_dump     = dp_detect_bad_dump(canonical);
   dp_extract_disc_token(canonical, e->disc_token, sizeof(e->disc_token));
   idx->entries_count++;
   return true;
}

/* Commit cur_canonical / cur_jpg_size as one or more entries.  Most
 * canonicals produce a single entry.  Canonicals that contain ` _ `
 * (libretro's alt-name separator after & or `/` sanitization) are
 * recognised as multi-name bundles like
 *
 *   "F-16 Fighting Falcon _ F-16 Fighter _ F16 Falcon Fighter (USA)"
 *
 * and produce one entry per segment, all pointing at the same
 * canonical key (and therefore the same on-disk JPG path).  This
 * lets a user filename like "F-16 Fighter (USA).zip" match the
 * bundle via its second alt name. */
static bool dp_commit_entry(dp_parse_ctx_t *c)
{
   downplay_thumbs_index_t *idx = c->idx;
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
      if (!dp_append_entry(idx, c->cur_canonical, heavy_buf,
               c->cur_jpg_size, c->cur_webp_size, &c->oom))
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
                           heavy_buf, c->cur_jpg_size,
                           c->cur_webp_size, &c->oom))
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
   c->cur_jpg_size  = 0;
   c->cur_webp_size = 0;
   (void)any_committed;
   return true;

fail:
   free(c->cur_canonical);
   c->cur_canonical = NULL;
   c->cur_jpg_size  = 0;
   c->cur_webp_size = 0;
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
      c->cur_jpg_size  = 0;
      c->cur_webp_size = 0;
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
   (void)len;
   if (c->depth > 0
       && c->state_stack[c->depth - 1] == DP_PS_IN_FORMATS)
   {
      /* Clamp negatives — atoi("-1") would otherwise survive into
       * dp_pick_ext's `> 0` test as a falsy value but propagate as a
       * weird int elsewhere.  Hostile or malformed indexes only. */
      int n = atoi(str);
      if (n < 0)
         n = 0;
      if (!strcmp(c->last_member, "jpg"))
         c->cur_jpg_size = n;
      else if (!strcmp(c->last_member, "webp"))
         c->cur_webp_size = n;
   }
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

/* Build the (heavy, canonical) and (canonical) permutation arrays. */
static bool dp_idx_finalize_sort(downplay_thumbs_index_t *idx)
{
   size_t    i;
   uint32_t *a;
   uint32_t *b;
   if (idx->entries_count == 0)
      return true;
   a = (uint32_t*)malloc(idx->entries_count * sizeof(*a));
   b = (uint32_t*)malloc(idx->entries_count * sizeof(*b));
   if (!a || !b)
   {
      free(a);
      free(b);
      return false;
   }
   for (i = 0; i < idx->entries_count; i++)
      a[i] = b[i] = (uint32_t)i;
   /* g_dp_cmp_base: file-static used by the qsort comparator since
    * qsort takes no user-data param.  Single-threaded by construction
    * — only set during this finalize call, on the single UI thread. */
   g_dp_cmp_base = idx->entries;
   qsort(a, idx->entries_count, sizeof(*a), dp_cmp_heavy);
   qsort(b, idx->entries_count, sizeof(*b), dp_cmp_canonical);
   g_dp_cmp_base = NULL;
   idx->by_heavy     = a;
   idx->by_canonical = b;
   return true;
}

downplay_thumbs_index_t *downplay_thumbs_index_parse(
      const char *json, size_t json_len)
{
   downplay_thumbs_index_t *idx;
   dp_parse_ctx_t           ctx;
   bool                     ok;

   if (!json || json_len == 0)
      return NULL;
   idx = (downplay_thumbs_index_t*)calloc(1, sizeof(*idx));
   if (!idx)
      return NULL;

   memset(&ctx, 0, sizeof(ctx));
   ctx.idx = idx;

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
   /* Tolerate trailing-state weirdness: as long as we got at least one
    * entry committed and didn't hit OOM, we return the index. */
   if (ctx.oom || !ctx.saw_files_obj || !ok)
   {
      free(ctx.cur_canonical);
      downplay_thumbs_index_free(idx);
      return NULL;
   }

   if (!dp_idx_finalize_sort(idx))
   {
      downplay_thumbs_index_free(idx);
      return NULL;
   }
   return idx;
}

void downplay_thumbs_index_free(downplay_thumbs_index_t *idx)
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
   free(idx->by_heavy);
   free(idx->by_canonical);
   free(idx);
}

size_t downplay_thumbs_index_count(const downplay_thumbs_index_t *idx)
{
   return idx ? idx->entries_count : 0;
}

/* ---------------------------------------------------------------- */
/* Section 3: lookup                                                */
/* ---------------------------------------------------------------- */

/* Strip `.ext` from `in` into `out`.  Returns out for chaining. */
static char *dp_strip_ext(const char *in, char *out, size_t out_size)
{
   const char *dot;
   if (out_size == 0) return out;
   strlcpy(out, in ? in : "", out_size);
   /* Caller passes a basename (no path separators), so plain strrchr
    * on the buffer is correct.  Skip files starting with '.' — they
    * have no real extension to strip ("." at index 0). */
   dot = strrchr(out, '.');
   if (dot && dot != out)
      out[dot - out] = '\0';
   return out;
}

/* Lower bound bsearch on `by_heavy`, comparing by entry's `heavy`
 * string.  Returns the smallest index in `by_heavy` whose entry's
 * heavy is >= `needle`, or `entries_count` if none. */
static size_t dp_lower_bound_heavy(const downplay_thumbs_index_t *idx,
      const char *needle)
{
   size_t lo = 0, hi = idx->entries_count;
   while (lo < hi)
   {
      size_t mid = lo + (hi - lo) / 2;
      const dp_thumb_entry_t *e = &idx->entries[idx->by_heavy[mid]];
      if (strcmp(e->heavy, needle) < 0)
         lo = mid + 1;
      else
         hi = mid;
   }
   return lo;
}

/* Compute a tiebreak score for an entry.  Lower wins.
 *
 * Layered, in order of significance (each layer is decisive on its
 * own — region_score doesn't matter if the disc bands differ):
 *   - bad_dump:    +1,000,000 if true
 *   - disc match:  ±100,000 (matches user_disc → bonus, mismatches → penalty)
 *   - region_score (×100, range 0..900)
 *   - -rev_num     (negative so newer rev wins)
 *
 * A canonical-lex tiebreak is applied at the comparison site (not
 * scored — strings can't be folded into a single int). */
static int dp_score_entry(const dp_thumb_entry_t *e, const char *user_disc)
{
   int sc = 0;
   if (e->bad_dump)
      sc += 1000000;
   if (user_disc && *user_disc)
   {
      if (*e->disc_token && !strcmp(e->disc_token, user_disc))
         sc -= 100000;
      else if (*e->disc_token)
         sc += 100000;
      /* user named a disc but entry has none → neutral (0). */
   }
   sc += e->region_score * 100;
   sc -= e->rev_num;
   return sc;
}

/* Among the [start..) range of `by_heavy` whose `heavy` equals
 * `needle`, pick the best candidate by `dp_score_entry`.  Canonical
 * lex order breaks score ties for full determinism.  Returns
 * SIZE_MAX if no candidate matched. */
static size_t dp_pick_best(const downplay_thumbs_index_t *idx,
      const char *needle, size_t start, const char *user_disc)
{
   size_t i;
   size_t best       = (size_t)-1;
   const dp_thumb_entry_t *best_e = NULL;
   int    best_sc    = 0x7fffffff;
   for (i = start; i < idx->entries_count; i++)
   {
      const dp_thumb_entry_t *e = &idx->entries[idx->by_heavy[i]];
      int sc;
      if (strcmp(e->heavy, needle) != 0)
         break;
      sc = dp_score_entry(e, user_disc);
      if (sc < best_sc
            || (sc == best_sc && best_e
                  && strcmp(e->canonical, best_e->canonical) < 0))
      {
         best_sc = sc;
         best_e  = e;
         best    = idx->by_heavy[i];
      }
   }
   return best;
}

/* Bsearch on by_canonical for an exact match.  Returns the entry
 * index, or SIZE_MAX on miss. */
static size_t dp_lookup_exact_canonical(
      const downplay_thumbs_index_t *idx, const char *stem)
{
   size_t lo = 0, hi = idx->entries_count;
   while (lo < hi)
   {
      size_t mid = lo + (hi - lo) / 2;
      const dp_thumb_entry_t *e = &idx->entries[idx->by_canonical[mid]];
      int rv = strcmp(e->canonical, stem);
      if (rv == 0)
         return idx->by_canonical[mid];
      if (rv < 0)
         lo = mid + 1;
      else
         hi = mid;
   }
   return (size_t)-1;
}

/* Internal lookup returning the entry index (or SIZE_MAX).  Spares
 * callers a second linear scan to recover the entry from a returned
 * canonical pointer.  Used directly by the manager; the public
 * `_match` wraps it for tests. */
static size_t dp_index_match_idx(
      const downplay_thumbs_index_t *idx,
      const char *rom_basename)
{
   char        stem[512];
   char        heavy_user[512];
   char        user_disc[16];
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

   lo = dp_lower_bound_heavy(idx, heavy_user);
   if (lo >= idx->entries_count)
      return (size_t)-1;
   if (strcmp(idx->entries[idx->by_heavy[lo]].heavy, heavy_user) != 0)
      return (size_t)-1;

   dp_extract_disc_token(stem, user_disc, sizeof(user_disc));
   return dp_pick_best(idx, heavy_user, lo, user_disc);
}

const char *downplay_thumbs_index_match(
      const downplay_thumbs_index_t *idx,
      const char *rom_basename)
{
   size_t hit_idx = dp_index_match_idx(idx, rom_basename);
   if (hit_idx == (size_t)-1)
      return NULL;
   return idx->entries[hit_idx].canonical;
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

   /* Per-canonical fetch tracking.  Parallel to index->entries; an
    * entry's status drives _request return values + de-dups in-flight
    * downloads.  Allocated on index ready. */
   uint8_t *attempt;        /* DP_ATT_* values, length entries_count */

   /* Image fetch ring buffer of canonical-key indices into index->entries. */
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

/* Compute <root>/Thumbs/index/<system>.index.json into `out`.
 * Single source of truth for the on-disk index location: both
 * `downplay_thumbs_open` and the boot-time prefetch use this so they
 * cannot disagree on where the file lives.  Returns false if the
 * filesystem root can't be resolved. */
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
   snprintf(fname, sizeof(fname), "%s.index.json", system);
   fill_pathname_join_special(out, idx_dir, fname, out_size);
   return true;
}

/* Pick the on-disk + remote extension for an entry.  Webp wins when
 * available — it's typically ~40% smaller than the matching jpg with
 * indistinguishable visual quality at thumbnail resolution. */
static const char *dp_pick_ext(const dp_thumb_entry_t *e)
{
#ifdef HAVE_DOWNPLAY_WEBP
   if (e && e->webp_size > 0)
      return ".webp";
#else
   (void)e;
#endif
   return ".jpg";
}

/* Build the on-disk path for an image of canonical key `canon`.  We
 * use the canonical key verbatim as the filename — it's already the
 * No-Intro safe form (no path separators since it's a No-Intro game
 * title), with the entry's preferred extension appended. */
static void dp_build_image_path(downplay_thumbs_t *t,
      const dp_thumb_entry_t *e, char *out, size_t out_size)
{
   char tmp[DP_THUMBS_PATH_MAX];
   fill_pathname_join_special(tmp, t->cache_dir, e->canonical, sizeof(tmp));
   snprintf(out, out_size, "%s%s", tmp, dp_pick_ext(e));
}

/* Build the remote URL for an image. */
static void dp_build_image_url(downplay_thumbs_t *t,
      const dp_thumb_entry_t *e, char *out, size_t out_size)
{
   char raw[2048];
   snprintf(raw, sizeof(raw), "%s/%s/Named_Boxarts/%s%s",
         DP_THUMBS_BASE_URL, t->system, e->canonical, dp_pick_ext(e));
   net_http_urlencode_full(out, raw, out_size);
}

/* ---- internal: HTTP callbacks (detached from manager) ----
 *
 * Both callbacks run on the main thread (RA's task system dispatches
 * `t->callback` from `task_queue_check`).  They write the file then
 * free their context — they never touch the manager.  The manager
 * discovers completion by stat'ing the path on the next pump/request. */

static void dp_cb_index_download(retro_task_t *task, void *task_data,
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

   strlcpy(output_dir, transf->path, sizeof(output_dir));
   path_basedir_wrapper(output_dir);
   if (!path_mkdir(output_dir))
   {
      err = "mkdir failed";
      goto finish;
   }

   /* Write to .tmp, rename — index parse must see all-or-nothing. */
   snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", transf->path);
   if (!filestream_write_file(tmp_path, data->data, data->len))
   {
      err = "write failed";
      goto finish;
   }
#ifdef _WIN32
   /* rename() refuses to clobber on Windows; remove first. */
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
      RARCH_WARN("[Downplay] thumbs index \"%s\" failed: %s\n",
            transf->path, err);
   else
      RARCH_LOG("[Downplay] thumbs index -> %s\n", transf->path);
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

/* Try to load + parse the on-disk index into the manager.  Returns
 * true on success (sets load_state=READY).  Leaves state unchanged on
 * failure so caller can decide between FETCHING (still waiting) vs
 * FAILED (give up). */
static bool dp_try_load_local_index(downplay_thumbs_t *t)
{
   int64_t  size      = 0;
   void    *buf       = NULL;
   bool     ok        = false;

   if (!path_is_valid(t->idx_path))
      return false;
   if (!filestream_read_file(t->idx_path, &buf, &size) || !buf || size <= 0)
   {
      free(buf);
      return false;
   }
   t->index = downplay_thumbs_index_parse((const char*)buf, (size_t)size);
   free(buf);
   if (!t->index)
   {
      RARCH_WARN("[Downplay] thumbs: parse failed for %s\n", t->idx_path);
      return false;
   }
   t->attempt = (uint8_t*)calloc(t->index->entries_count, 1);
   if (!t->attempt)
   {
      downplay_thumbs_index_free(t->index);
      t->index = NULL;
      return false;
   }
   /* Pre-mark on-disk images so we don't spuriously refetch.  Cheap:
    * a stat() per cached file at open time, not per frame.  When an
    * entry has webp_size>0 (preferred) but only the .jpg sibling is
    * on disk — typical after a Downplay upgrade where the previous
    * version cached jpg only — clear webp_size so the entry stays
    * on the cached jpg until natural TTL refresh.  Avoids a forced
    * full re-download on every existing user's first launch. */
   {
      size_t i;
      char img_path[DP_THUMBS_PATH_MAX];
      char jpg_path[DP_THUMBS_PATH_MAX];
      for (i = 0; i < t->index->entries_count; i++)
      {
         dp_thumb_entry_t *e = &t->index->entries[i];
         dp_build_image_path(t, e, img_path, sizeof(img_path));
         if (path_is_valid(img_path))
         {
            t->attempt[i] = DP_ATT_ON_DISK;
            continue;
         }
         if (e->webp_size > 0)
         {
            char tmp[DP_THUMBS_PATH_MAX];
            fill_pathname_join_special(tmp, t->cache_dir,
                  e->canonical, sizeof(tmp));
            snprintf(jpg_path, sizeof(jpg_path), "%s.jpg", tmp);
            if (path_is_valid(jpg_path))
            {
               e->webp_size = 0; /* fall through to .jpg in dp_pick_ext */
               t->attempt[i] = DP_ATT_ON_DISK;
            }
         }
      }
   }
   t->load_state = DP_LOAD_READY;
   ok = true;
   return ok;
}

/* Push the index HTTP fetch.  Caller should set state=FETCHING. */
static void dp_kick_index_fetch(downplay_thumbs_t *t)
{
   file_transfer_t *transf;
   char raw_url[2048];
   char url[2048];

   snprintf(raw_url, sizeof(raw_url),
         "%s/%s/Named_Boxarts/index.json",
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
      if (e_idx >= t->index->entries_count)
      {
         /* Defensive: invalid entry — drop. */
         t->fetching[i] = t->fetching[--t->inflight];
         continue;
      }
      dp_build_image_path(t, &t->index->entries[e_idx],
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
   const dp_thumb_entry_t *e;
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

   if (e_idx >= t->index->entries_count)
      return;
   /* Re-check state: row may have transitioned to ON_DISK since enqueue. */
   if (t->attempt[e_idx] != DP_ATT_UNTRIED
       && t->attempt[e_idx] != DP_ATT_FETCHING)
      return;

   e = &t->index->entries[e_idx];
   dp_build_image_path(t, e, path, sizeof(path));
   if (path_is_valid(path))
   {
      t->attempt[e_idx] = DP_ATT_ON_DISK;
      return;
   }
   dp_build_image_url(t, e, url, sizeof(url));
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
 * Drive a small global queue of <root>/Thumbs/index/<system>.index.json
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
/* Hostile / misconfigured server can't blow up our cache.  Real
 * indexes are tens-to-hundreds of KB; 1 MB leaves real headroom for
 * the largest No-Intro systems (PSX, NDS, etc.) with their alt-name
 * bundles, while still capping a runaway response at a safe size. */
#define DP_THUMBS_PF_INDEX_MAX_BYTES (1024u * 1024u)

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
   http_transfer_data_t *data = (http_transfer_data_t*)task_data;
   dp_pf_transfer_t     *pf   = (dp_pf_transfer_t*)user_data;
   char                  output_dir[DP_THUMBS_PATH_MAX];
   char                  tmp_path[DP_THUMBS_PATH_MAX];
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
   strlcpy(output_dir, pf->base.path, sizeof(output_dir));
   path_basedir_wrapper(output_dir);
   if (!path_mkdir(output_dir))
   {
      err = "mkdir failed";
      goto finish;
   }
   /* Atomic write: .tmp + rename — consistent with dp_cb_index_download. */
   snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", pf->base.path);
   if (!filestream_write_file(tmp_path, data->data, data->len))
   {
      err = "write failed";
      goto finish;
   }
#ifdef _WIN32
   filestream_delete(pf->base.path);
#endif
   if (rename(tmp_path, pf->base.path) != 0)
   {
      filestream_delete(tmp_path);
      err = "rename failed";
      goto finish;
   }

finish:
   if (err && *err)
      RARCH_WARN("[Downplay] thumbs prefetch \"%s\" failed: %s\n",
            pf->system, err);
   else
      RARCH_LOG("[Downplay] thumbs prefetch -> %s\n", pf->base.path);
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
            "%s/%s/Named_Boxarts/index.json",
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
   /* <root>/Thumbs/index/<system>.index.json — via shared helper so
    * boot-time prefetch and per-system open agree on the location. */
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
   /* Suppress duplicate HTTP work if a boot-time prefetch is already
    * in flight OR queued for this system; the per-frame pump in
    * _request / _pump will pick up the file when it lands.  Pending
    * matters here because the user can navigate before dp_pf_drain
    * has had a chance to promote the entry to inflight. */
   if (!dp_pf_inflight_contains(system) && !dp_pf_pending_contains(system))
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
   const dp_thumb_entry_t *e;
   int                     e_idx;

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
   e = &t->index->entries[e_idx];

   dp_build_image_path(t, e, out->local_path, sizeof(out->local_path));

   if (t->attempt[e_idx] == DP_ATT_ON_DISK
       || path_is_valid(out->local_path))
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

#endif /* DOWNPLAY_THUMBS_TEST_BUILD */
