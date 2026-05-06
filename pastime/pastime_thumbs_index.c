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

/* Pure parse + match side of the thumbnail index.  See
 * pastime_thumbs.h for the public intent and pastime_thumbs.c for
 * the HTTP/IO manager that consumes this code.
 *
 * Module layout:
 *
 *   Section 1: index entries + normalization + tiebreaks
 *     - dp_thumb_entry_t
 *     - region scoring, disc/rev extraction, latin-folding
 *
 *   Section 2: JSON parse → packed binary buffer
 *     - rjson event handlers populating a transient builder
 *     - string pool + sort comparators + buffer emit
 *
 *   Section 3: binary index struct + match cascade (T0..T4)
 *     - pastime_thumbs_index_open / _free / _count / _match
 *
 * This file is also linked directly into the unit-test binary
 * (see pastime/tests/run_tests.sh): no HTTP, no log, no manager
 * dependencies — purely deterministic string code.  Coding rules:
 * C89-style declarations, Allman braces, libretro-common helpers
 * for any cross-platform string/path work. */

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>

#include <boolean.h>
#include <compat/strl.h>
#include <formats/rjson.h>
#include <string/stdstring.h>

#include "pastime_thumbs.h"
#include "pastime_thumbs_internal.h"

/* Hard upper bound on parsed entries.  Real No-Intro / Redump
 * indexes are 5k–20k entries per system; 100k gives generous
 * headroom while preventing a hostile 256 MB index from OOM-killing
 * the launcher.  Also caps the binary-format `entry_count` field at
 * open time before we multiply it out — see dp_idx_open. */
#define DP_THUMBS_MAX_ENTRIES 100000u

/* On-disk binary index format.  Replaces gzipped JSON as the cached
 * representation: parse JSON once on download, emit a packed binary
 * file, then every subsequent open is a `pread` + 40-byte header
 * validation — no JSON, no allocations beyond the buffer itself.
 *
 * Layout (little-endian, 4-byte aligned sections):
 *
 *   [HEADER 40B]
 *     u32 magic           = 'DPTH'
 *     u32 version         = 2
 *     u32 entry_count
 *     u32 strings_size
 *     u32 thumbhash_size  -- bytes in the thumbhash pool
 *     u32 entries_off     -- always 40
 *     u32 by_canonical_off
 *     u32 by_heavy_off
 *     u32 strings_off
 *     u32 thumbhash_off   -- pool sits after strings
 *
 *   [ENTRIES, 16 bytes each, original load order]
 *     u32 canonical_off   -- offset into string pool
 *     u32 heavy_off
 *     u16 width           -- 0 if unknown (server omitted dims)
 *     u16 height
 *     u32 thumbhash_off   -- 1-based pool offset; 0 == "no thumbhash"
 *
 *   [BY_CANONICAL, u32 entry indices, sorted by strcmp(canonical)]
 *
 *   [BY_HEAVY, {u32 hash, u32 entry_idx} pairs, sorted by
 *    (hash, then strcmp(heavy) for stability across collisions)]
 *
 *   [STRINGS, packed NUL-terminated string pool, dedup'd]
 *
 *   [THUMBHASH POOL, packed (u8 len; u8 bytes[len]) records]
 *     Variable-length, length-prefixed.  Offsets stored in entries
 *     are 1-based so 0 cleanly indicates "absent".  No dedup pass —
 *     alt-name expansion (one canonical → multiple entries with the
 *     same image) does duplicate the bytes, but the per-entry payload
 *     is ~25 bytes and bundles are rare; not worth the dedup hash.
 *
 *   [FOOTER 12B]
 *     u32 magic_repeat
 *     u32 version_repeat
 *     u32 entry_count_repeat
 *
 * Tiebreak fields (region / rev / bad_dump / disc) are NOT stored —
 * they're derived from the canonical name on demand inside the
 * cascade.  Per-query cost is bounded (cascade walks 1-5 candidates;
 * helpers are ~50 ns each).  Net win: zero per-record metadata
 * overhead for tiebreaks, no parse-time computation, single source of
 * truth.  Width / height / thumbhash, by contrast, ARE stored: they
 * can't be derived from the canonical, and the menu driver needs them
 * synchronously at row-layout time (before any image fetch) to pick
 * per-row text widths.
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
#define DP_IDX_VERSION    2u
#define DP_IDX_HEADER_SZ  40u
#define DP_IDX_FOOTER_SZ  12u
#define DP_IDX_REC_SZ     16u          /* canon_off + heavy_off + w + h + th_off */
#define DP_IDX_BCAN_SZ    4u           /* entry_idx */
#define DP_IDX_BHEV_SZ    8u           /* hash + entry_idx */
/* Thumbhash bytes are the binary form (post-base64-decode).  evanw's
 * encoder produces 5..25 bytes; pad the cap to 32 to give the format
 * room without bumping the length-byte to u16 if they ever extend the
 * spec. */
#define DP_IDX_THUMBHASH_MAX 32u

/* ---------------------------------------------------------------- */
/* Section 1: parse-time entry struct + normalization + tiebreaks   */
/* ---------------------------------------------------------------- */

/* Transient struct used during JSON parse only.  Does NOT survive
 * past `pastime_thumbs_index_parse` — we emit a binary buffer from
 * a temp array of these and free them.  Tiebreak data is derived
 * from `canonical` on demand at cascade time; width / height /
 * thumbhash, by contrast, are persisted to the binary format because
 * the menu driver needs them at row-layout time. */
typedef struct
{
   char    *canonical;     /* exactly the index.json key */
   char    *heavy;          /* normalized form for the bsearch index */
   uint8_t *thumbhash;      /* binary thumbhash bytes, post-base64-decode (NULL if absent) */
   uint16_t width;          /* 0 if absent in JSON */
   uint16_t height;
   uint8_t  thumbhash_len;  /* 0 if absent */
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
 * as the brackets-strip in pastime_display_name.c but inlined here
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
 * `pastime_thumbs_index_parse` call: rjson handlers populate it,
 * then dp_idx_emit_buffer serialises it to the on-disk binary
 * format and frees it.  The public-API `pastime_thumbs_index_t`
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

   /* Accumulators for the entry currently being parsed.  Reset on
    * every new entry-key seen at DP_PS_AT_FILES depth; consumed and
    * reset by dp_commit_entry on entry close.  cur_canonical is the
    * sentinel — NULL between entries, strdup'd while inside one. */
   char    *cur_canonical;
   uint8_t *cur_thumbhash;
   uint16_t cur_width;
   uint16_t cur_height;
   uint8_t  cur_thumbhash_len;

   /* Last seen object-member name; consumed by the next value handler. */
   char  last_member[256];

   bool  in_files;     /* did we see "files" at top level? */
   bool  saw_files_obj;

   bool  oom;
} dp_parse_ctx_t;

/* Reset the entry-scoped accumulators in the parse context so a new
 * entry starts clean.  Leaves last_member / state stack alone — those
 * are managed by the caller.  Defined here so dp_commit_entry can
 * call it; the JSON value handlers further down do too. */
static void dp_parse_ctx_reset_entry(dp_parse_ctx_t *c)
{
   free(c->cur_canonical);
   free(c->cur_thumbhash);
   c->cur_canonical     = NULL;
   c->cur_thumbhash     = NULL;
   c->cur_thumbhash_len = 0;
   c->cur_width         = 0;
   c->cur_height        = 0;
}

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
      const char *canonical, const char *heavy,
      uint16_t width, uint16_t height,
      const uint8_t *thumbhash, uint8_t thumbhash_len,
      bool *oom)
{
   dp_thumb_entry_t *e;
   if (idx->entries_count == idx->entries_cap && !dp_idx_grow(idx))
   {
      *oom = true;
      return false;
   }
   e = &idx->entries[idx->entries_count];
   memset(e, 0, sizeof(*e));
   e->canonical = strdup(canonical);
   e->heavy     = strdup(heavy);
   if (!e->canonical || !e->heavy)
      goto fail;
   /* Alt-name expansion produces multiple entries that share the same
    * canonical key (and image).  Each gets its own copy of the
    * thumbhash bytes — no per-entry refcount; bundles are rare and the
    * payload is ~25 bytes, not worth the ledger. */
   if (thumbhash && thumbhash_len > 0)
   {
      e->thumbhash = (uint8_t*)malloc(thumbhash_len);
      if (!e->thumbhash)
         goto fail;
      memcpy(e->thumbhash, thumbhash, thumbhash_len);
      e->thumbhash_len = thumbhash_len;
   }
   e->width  = width;
   e->height = height;
   idx->entries_count++;
   return true;

fail:
   free(e->canonical);
   free(e->heavy);
   free(e->thumbhash);
   memset(e, 0, sizeof(*e));
   *oom = true;
   return false;
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
      if (!dp_append_entry(idx, c->cur_canonical, heavy_buf,
               c->cur_width, c->cur_height,
               c->cur_thumbhash, c->cur_thumbhash_len, &c->oom))
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
                           heavy_buf,
                           c->cur_width, c->cur_height,
                           c->cur_thumbhash, c->cur_thumbhash_len,
                           &c->oom))
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

   dp_parse_ctx_reset_entry(c);
   (void)any_committed;
   return true;

fail:
   dp_parse_ctx_reset_entry(c);
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

/* Decode standard base64 (with optional '=' padding and whitespace).
 * Writes binary bytes to `out`; returns decoded length on success or 0
 * on malformed input (caller treats absent thumbhash as 0-length).
 * Used only for the per-entry "thumbhash" field — small inputs (~32
 * b64 chars), bounded by the caller's `out_cap`. */
static int dp_base64_byte(unsigned char c)
{
   if (c >= 'A' && c <= 'Z') return (int)(c - 'A');
   if (c >= 'a' && c <= 'z') return (int)(c - 'a') + 26;
   if (c >= '0' && c <= '9') return (int)(c - '0') + 52;
   if (c == '+' || c == '-') return 62;  /* '-' = url-safe alphabet */
   if (c == '/' || c == '_') return 63;  /* '_' = url-safe alphabet */
   return -1;
}

static size_t dp_base64_decode(const char *in, size_t in_len,
      uint8_t *out, size_t out_cap)
{
   size_t i, o = 0;
   int    quad[4];
   size_t qi = 0;
   for (i = 0; i < in_len; i++)
   {
      unsigned char ch = (unsigned char)in[i];
      int           v;
      if (ch == '=' || ch == ' ' || ch == '\n' || ch == '\r' || ch == '\t')
         continue;
      v = dp_base64_byte(ch);
      if (v < 0)
         return 0;
      quad[qi++] = v;
      if (qi == 4)
      {
         if (o + 3 > out_cap) return 0;
         out[o++] = (uint8_t)((quad[0] << 2) | (quad[1] >> 4));
         out[o++] = (uint8_t)(((quad[1] & 0xf) << 4) | (quad[2] >> 2));
         out[o++] = (uint8_t)(((quad[2] & 0x3) << 6) | quad[3]);
         qi = 0;
      }
   }
   if (qi == 1)
      return 0;  /* trailing 6 bits is malformed */
   if (qi == 2)
   {
      if (o + 1 > out_cap) return 0;
      out[o++] = (uint8_t)((quad[0] << 2) | (quad[1] >> 4));
   }
   else if (qi == 3)
   {
      if (o + 2 > out_cap) return 0;
      out[o++] = (uint8_t)((quad[0] << 2) | (quad[1] >> 4));
      out[o++] = (uint8_t)(((quad[1] & 0xf) << 4) | (quad[2] >> 2));
   }
   return o;
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
      dp_parse_ctx_reset_entry(c);
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

/* Parse a JSON-emitted unsigned integer string into a u16-bounded int.
 * Returns -1 if `s` isn't a clean ASCII decimal in [0, 65535] —
 * negative, too long, sign chars, decimals, or exponent forms all
 * fall through to "not a width/height we can use" and are silently
 * dropped (entry will record dims as 0 / "absent"). */
static int dp_parse_uint_str(const char *s, size_t len)
{
   size_t i;
   long   v = 0;
   if (len == 0 || len > 5)
      return -1;
   for (i = 0; i < len; i++)
   {
      if (s[i] < '0' || s[i] > '9')
         return -1;
      v = v * 10 + (s[i] - '0');
      if (v > 65535)
         return -1;
   }
   return (int)v;
}

static bool dp_h_number(void *ctx, const char *str, size_t len)
{
   dp_parse_ctx_t *c = (dp_parse_ctx_t*)ctx;
   /* `formats.{jpg,webp}` and `source.size` numbers fall through —
    * webp is always available, sizes aren't persisted.  We capture
    * width/height inside an entry; the binary format stores both. */
   if (   c->depth >= 1
       && c->state_stack[c->depth - 1] == DP_PS_IN_ENTRY
       && c->cur_canonical)
   {
      if (!strcmp(c->last_member, "width"))
      {
         int v = dp_parse_uint_str(str, len);
         if (v >= 0)
            c->cur_width = (uint16_t)v;
      }
      else if (!strcmp(c->last_member, "height"))
      {
         int v = dp_parse_uint_str(str, len);
         if (v >= 0)
            c->cur_height = (uint16_t)v;
      }
   }
   c->last_member[0] = '\0';
   return true;
}

static bool dp_h_string(void *ctx, const char *str, size_t len)
{
   dp_parse_ctx_t *c = (dp_parse_ctx_t*)ctx;
   if (   c->depth >= 1
       && c->state_stack[c->depth - 1] == DP_PS_IN_ENTRY
       && c->cur_canonical
       && !strcmp(c->last_member, "thumbhash"))
   {
      uint8_t  scratch[DP_IDX_THUMBHASH_MAX];
      size_t   n;
      /* Bounded base64 input.  Conservative cap: a 32-byte binary
       * thumbhash needs at most 44 b64 chars (with padding); accept
       * up to 64 to tolerate whitespace, then reject if the decoded
       * length exceeds the format cap. */
      if (len == 0 || len > 64)
         goto consume;
      n = dp_base64_decode(str, len, scratch, sizeof(scratch));
      if (n == 0 || n > DP_IDX_THUMBHASH_MAX)
         goto consume;
      free(c->cur_thumbhash);
      c->cur_thumbhash = (uint8_t*)malloc(n);
      if (!c->cur_thumbhash)
      {
         c->oom = true;
         return false;
      }
      memcpy(c->cur_thumbhash, scratch, n);
      c->cur_thumbhash_len = (uint8_t)n;
   }
consume:
   c->last_member[0] = '\0';
   return true;
}

static bool dp_h_bool(void *ctx, bool v) { dp_parse_ctx_t *c = (dp_parse_ctx_t*)ctx; (void)v; c->last_member[0] = '\0'; return true; }
static bool dp_h_null(void *ctx) { dp_parse_ctx_t *c = (dp_parse_ctx_t*)ctx; c->last_member[0] = '\0'; return true; }

#ifdef PASTIME_THUMBS_TEST_BUILD
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

/* qsort comparators carry every field they need inside the element
 * itself.  No file-static base pointer — that would race when two
 * worker threads emit indexes concurrently (boot prefetch fan-out
 * spawns up to DP_THUMBS_PF_INFLIGHT_MAX workers in flight).  The
 * extra ~16 bytes/entry of transient sort memory is the price of
 * keeping these comparators pure. */
typedef struct
{
   const char *canonical;
   uint32_t    idx;
} dp_sort_canon_t;

static int dp_cmp_canonical_kv(const void *a, const void *b)
{
   const dp_sort_canon_t *pa = (const dp_sort_canon_t*)a;
   const dp_sort_canon_t *pb = (const dp_sort_canon_t*)b;
   return strcmp(pa->canonical, pb->canonical);
}

typedef struct
{
   uint32_t    hash;
   const char *heavy;
   const char *canonical;
   uint32_t    idx;
} dp_sort_heavy_t;

static int dp_cmp_heavy_kv(const void *a, const void *b)
{
   const dp_sort_heavy_t *pa = (const dp_sort_heavy_t*)a;
   const dp_sort_heavy_t *pb = (const dp_sort_heavy_t*)b;
   int rv;
   if (pa->hash != pb->hash)
      return pa->hash < pb->hash ? -1 : 1;
   /* On hash collision, sort by heavy strcmp so equal-heavy entries
    * stay contiguous within an equal-hash run.  Then by canonical for
    * full determinism even when two entries share an identical heavy. */
   rv = strcmp(pa->heavy, pb->heavy);
   if (rv != 0)
      return rv;
   return strcmp(pa->canonical, pb->canonical);
}

/* Serialise the parsed entry array into the on-disk binary format.
 * On success transfers ownership of the buffer to `*out_buf` (caller
 * frees) and writes its length to `*out_len`.  Returns false on
 * allocation failure or if `n` exceeds the entry-count cap. */
static bool dp_idx_emit_buffer(const dp_thumb_entry_t *entries, size_t n,
      uint8_t **out_buf, size_t *out_len)
{
   uint8_t          *buf      = NULL;
   uint32_t         *can_offs = NULL;
   uint32_t         *hev_offs = NULL;
   uint32_t         *th_offs  = NULL;     /* 1-based pool offsets, 0 == absent */
   dp_sort_canon_t  *bcanon   = NULL;
   dp_sort_heavy_t  *bheavy   = NULL;
   dp_strpool_t      pool;
   size_t          i;
   size_t          entries_off, bcanon_off, bheavy_off, strings_off;
   size_t          thumbhash_off, footer_off;
   size_t          entries_sz, bcanon_sz, bheavy_sz, thumbhash_size, total;
   bool            ok = false;

   memset(&pool, 0, sizeof(pool));
   if (n > DP_THUMBS_MAX_ENTRIES)
      return false;

   if (n > 0)
   {
      can_offs = (uint32_t*)malloc(n * sizeof(*can_offs));
      hev_offs = (uint32_t*)malloc(n * sizeof(*hev_offs));
      th_offs  = (uint32_t*)calloc(n, sizeof(*th_offs));
      if (!can_offs || !hev_offs || !th_offs)
         goto out;
   }
   for (i = 0; i < n; i++)
   {
      if (!dp_strpool_intern(&pool, entries[i].canonical, &can_offs[i]))
         goto out;
      if (!dp_strpool_intern(&pool, entries[i].heavy,     &hev_offs[i]))
         goto out;
   }

   /* Compute thumbhash pool size and assign 1-based offsets.  No dedup
    * pass: most canonicals produce one entry; alt-name expansion does
    * duplicate ~25 bytes per extra segment, but bundles are rare. */
   thumbhash_size = 0;
   for (i = 0; i < n; i++)
   {
      uint8_t tlen = entries[i].thumbhash_len;
      if (tlen == 0 || !entries[i].thumbhash)
         continue;
      th_offs[i] = (uint32_t)(thumbhash_size + 1);  /* 1-based */
      thumbhash_size += 1u + (size_t)tlen;          /* len byte + bytes */
   }

   entries_off    = DP_IDX_HEADER_SZ;
   entries_sz     = n * DP_IDX_REC_SZ;
   bcanon_off     = entries_off + entries_sz;
   bcanon_sz      = n * DP_IDX_BCAN_SZ;
   bheavy_off     = bcanon_off + bcanon_sz;
   bheavy_sz      = n * DP_IDX_BHEV_SZ;
   strings_off    = bheavy_off + bheavy_sz;
   thumbhash_off  = strings_off + pool.size;
   footer_off     = thumbhash_off + thumbhash_size;
   total          = footer_off + DP_IDX_FOOTER_SZ;

   /* All section offsets are written into the header as u32.  Today
    * the entry-count cap (100k) and the string-pool cap (1 GB) keep
    * us comfortably under 4 GB, but a future cap bump that pushed
    * any offset past UINT32_MAX would silently produce a truncated
    * header that subsequent dp_idx_open calls would fail with no
    * useful error.  Fail loudly here instead. */
   if (footer_off > UINT32_MAX
         || pool.size > UINT32_MAX
         || thumbhash_size > UINT32_MAX)
      goto out;

   buf = (uint8_t*)calloc(total, 1);
   if (!buf)
      goto out;

   /* Header.  All multibyte fields written as little-endian via
    * memcpy of host u32 — Pastime ships only on LE targets. */
   {
      uint32_t v;
      v = DP_IDX_MAGIC;              memcpy(buf +  0, &v, 4);
      v = DP_IDX_VERSION;            memcpy(buf +  4, &v, 4);
      v = (uint32_t)n;               memcpy(buf +  8, &v, 4);
      v = (uint32_t)pool.size;       memcpy(buf + 12, &v, 4);
      v = (uint32_t)thumbhash_size;  memcpy(buf + 16, &v, 4);
      v = (uint32_t)entries_off;     memcpy(buf + 20, &v, 4);
      v = (uint32_t)bcanon_off;      memcpy(buf + 24, &v, 4);
      v = (uint32_t)bheavy_off;      memcpy(buf + 28, &v, 4);
      v = (uint32_t)strings_off;     memcpy(buf + 32, &v, 4);
      v = (uint32_t)thumbhash_off;   memcpy(buf + 36, &v, 4);
   }

   /* ENTRIES section: u32 canonical_off, u32 heavy_off, u16 width,
    * u16 height, u32 thumbhash_off (1-based; 0 = absent). */
   for (i = 0; i < n; i++)
   {
      uint8_t  *rec = buf + entries_off + i * DP_IDX_REC_SZ;
      uint16_t  w   = entries[i].width;
      uint16_t  h   = entries[i].height;
      memcpy(rec +  0, &can_offs[i], 4);
      memcpy(rec +  4, &hev_offs[i], 4);
      memcpy(rec +  8, &w,           2);
      memcpy(rec + 10, &h,           2);
      memcpy(rec + 12, &th_offs[i],  4);
   }

   /* BY_CANONICAL: u32 entry indices sorted by strcmp(canonical).
    * The sort tuples carry the canonical pointer inline so the
    * comparator is pure — multiple workers can emit indexes
    * concurrently without a file-static base pointer. */
   if (n > 0)
   {
      bcanon = (dp_sort_canon_t*)malloc(n * sizeof(*bcanon));
      if (!bcanon)
         goto out;
      for (i = 0; i < n; i++)
      {
         bcanon[i].canonical = entries[i].canonical;
         bcanon[i].idx       = (uint32_t)i;
      }
      qsort(bcanon, n, sizeof(*bcanon), dp_cmp_canonical_kv);
      for (i = 0; i < n; i++)
         memcpy(buf + bcanon_off + i * DP_IDX_BCAN_SZ, &bcanon[i].idx, 4);
   }

   /* BY_HEAVY: {u32 hash, u32 idx} sorted by (hash, strcmp(heavy),
    * strcmp(canonical)).  Same rationale for inline tuple data. */
   if (n > 0)
   {
      bheavy = (dp_sort_heavy_t*)malloc(n * sizeof(*bheavy));
      if (!bheavy)
         goto out;
      for (i = 0; i < n; i++)
      {
         bheavy[i].hash      = dp_fnv1a32(entries[i].heavy);
         bheavy[i].heavy     = entries[i].heavy;
         bheavy[i].canonical = entries[i].canonical;
         bheavy[i].idx       = (uint32_t)i;
      }
      qsort(bheavy, n, sizeof(*bheavy), dp_cmp_heavy_kv);
      for (i = 0; i < n; i++)
      {
         memcpy(buf + bheavy_off + i * DP_IDX_BHEV_SZ + 0, &bheavy[i].hash, 4);
         memcpy(buf + bheavy_off + i * DP_IDX_BHEV_SZ + 4, &bheavy[i].idx,  4);
      }
   }

   /* String pool. */
   if (pool.size > 0)
      memcpy(buf + strings_off, pool.data, pool.size);

   /* Thumbhash pool: walk entries in load order, writing
    * (u8 len; u8 bytes[len]) at the offset assigned earlier.  Skip
    * entries with no thumbhash (th_offs[i] == 0). */
   for (i = 0; i < n; i++)
   {
      uint8_t tlen = entries[i].thumbhash_len;
      uint8_t *dst;
      if (th_offs[i] == 0)
         continue;
      dst = buf + thumbhash_off + (th_offs[i] - 1);
      *dst++ = tlen;
      memcpy(dst, entries[i].thumbhash, tlen);
   }

   /* Footer: magic + version + entry_count repeat.  Version mirror
    * gives belt-and-braces detection of v1-binary-with-hand-edited-
    * version-byte attacks; the layout check would already catch them
    * on record-size mismatch, but this is one more u32 of compare. */
   {
      uint32_t v;
      v = DP_IDX_MAGIC;   memcpy(buf + footer_off + 0, &v, 4);
      v = DP_IDX_VERSION; memcpy(buf + footer_off + 4, &v, 4);
      v = (uint32_t)n;    memcpy(buf + footer_off + 8, &v, 4);
   }

   *out_buf = buf;
   *out_len = total;
   buf      = NULL; /* ownership transferred */
   ok       = true;

out:
   dp_strpool_free(&pool);
   free(can_offs);
   free(hev_offs);
   free(th_offs);
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
struct pastime_thumbs_index
{
   uint8_t  *buf;          /* owned, freed on _index_free */
   size_t    buf_len;
   uint32_t  entry_count;
   uint32_t  entries_off;
   uint32_t  bcanon_off;
   uint32_t  bheavy_off;
   uint32_t  strings_off;
   uint32_t  strings_size;
   uint32_t  thumbhash_off;
   uint32_t  thumbhash_size;
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
pastime_thumbs_index_t *dp_idx_open(uint8_t *buf, size_t buf_len)
{
   pastime_thumbs_index_t *idx;
   uint32_t magic, version, entry_count, strings_size, thumbhash_size;
   uint32_t entries_off, bcanon_off, bheavy_off, strings_off, thumbhash_off;
   uint64_t entries_sz, bcanon_sz, bheavy_sz, footer_off, expected;
   uint32_t fmagic, fversion, fcount;

   if (!buf || buf_len < (size_t)(DP_IDX_HEADER_SZ + DP_IDX_FOOTER_SZ))
      goto fail;

   magic          = dp_idx_read_u32(buf,  0);
   version        = dp_idx_read_u32(buf,  4);
   entry_count    = dp_idx_read_u32(buf,  8);
   strings_size   = dp_idx_read_u32(buf, 12);
   thumbhash_size = dp_idx_read_u32(buf, 16);
   entries_off    = dp_idx_read_u32(buf, 20);
   bcanon_off     = dp_idx_read_u32(buf, 24);
   bheavy_off     = dp_idx_read_u32(buf, 28);
   strings_off    = dp_idx_read_u32(buf, 32);
   thumbhash_off  = dp_idx_read_u32(buf, 36);

   if (magic != DP_IDX_MAGIC)               goto fail;
   if (version != DP_IDX_VERSION)           goto fail;
   if (entry_count > DP_THUMBS_MAX_ENTRIES) goto fail;

   /* Layout must be exact.  uint64_t math makes overflow impossible
    * since entry_count is already capped above. */
   entries_sz = (uint64_t)entry_count * DP_IDX_REC_SZ;
   bcanon_sz  = (uint64_t)entry_count * DP_IDX_BCAN_SZ;
   bheavy_sz  = (uint64_t)entry_count * DP_IDX_BHEV_SZ;
   if (entries_off    != DP_IDX_HEADER_SZ)                              goto fail;
   if ((uint64_t)bcanon_off     != (uint64_t)entries_off + entries_sz)  goto fail;
   if ((uint64_t)bheavy_off     != (uint64_t)bcanon_off  + bcanon_sz)   goto fail;
   if ((uint64_t)strings_off    != (uint64_t)bheavy_off  + bheavy_sz)   goto fail;
   if ((uint64_t)thumbhash_off  != (uint64_t)strings_off + strings_size) goto fail;

   footer_off = (uint64_t)thumbhash_off + thumbhash_size;
   expected   = footer_off + DP_IDX_FOOTER_SZ;
   if ((uint64_t)buf_len != expected)
      goto fail;

   /* String pool must end with NUL so strcmp inside the pool needs no
    * per-call bounds check — anything off-by-one would walk straight
    * into the thumbhash pool / footer. */
   if (strings_size > 0 && buf[strings_off + strings_size - 1] != '\0')
      goto fail;

   /* Footer mirrors the header — partial-write + version-tampering
    * detection.  Includes the version field since the layout check
    * above would otherwise be the only guard against a v1 binary
    * with its version byte hand-edited to 2. */
   fmagic   = dp_idx_read_u32(buf, (size_t)footer_off);
   fversion = dp_idx_read_u32(buf, (size_t)footer_off + 4);
   fcount   = dp_idx_read_u32(buf, (size_t)footer_off + 8);
   if (   fmagic   != DP_IDX_MAGIC
       || fversion != DP_IDX_VERSION
       || fcount   != entry_count)
      goto fail;

   idx = (pastime_thumbs_index_t*)calloc(1, sizeof(*idx));
   if (!idx)
      goto fail;
   idx->buf            = buf;
   idx->buf_len        = buf_len;
   idx->entry_count    = entry_count;
   idx->entries_off    = entries_off;
   idx->bcanon_off     = bcanon_off;
   idx->bheavy_off     = bheavy_off;
   idx->strings_off    = strings_off;
   idx->strings_size   = strings_size;
   idx->thumbhash_off  = thumbhash_off;
   idx->thumbhash_size = thumbhash_size;
   return idx;

fail:
   free(buf);
   return NULL;
}

void pastime_thumbs_index_free(pastime_thumbs_index_t *idx)
{
   if (!idx)
      return;
   free(idx->buf);
   free(idx);
}

size_t pastime_thumbs_index_count(const pastime_thumbs_index_t *idx)
{
   return idx ? idx->entry_count : 0;
}

/* String pool resolver.  Validation guarantees `off < strings_size`
 * for any offset the format itself produced, and that the pool is
 * NUL-terminated, so strcmp on the returned pointer is safe.  Out-of-
 * bounds offsets (post-validation memory corruption — not reachable
 * via the file format) return "" defensively. */
static const char *dp_idx_str(const pastime_thumbs_index_t *idx, uint32_t off)
{
   if (off >= idx->strings_size)
      return "";
   return (const char*)(idx->buf + idx->strings_off + off);
}

const char *dp_idx_canonical(
      const pastime_thumbs_index_t *idx, uint32_t e)
{
   return dp_idx_str(idx, dp_idx_read_u32(idx->buf,
         idx->entries_off + (size_t)e * DP_IDX_REC_SZ + 0));
}

static const char *dp_idx_heavy(
      const pastime_thumbs_index_t *idx, uint32_t e)
{
   return dp_idx_str(idx, dp_idx_read_u32(idx->buf,
         idx->entries_off + (size_t)e * DP_IDX_REC_SZ + 4));
}

void dp_idx_dims(const pastime_thumbs_index_t *idx, uint32_t e,
      uint16_t *out_w, uint16_t *out_h)
{
   const uint8_t *rec;
   if (e >= idx->entry_count)
   {
      *out_w = 0;
      *out_h = 0;
      return;
   }
   rec = idx->buf + idx->entries_off + (size_t)e * DP_IDX_REC_SZ;
   memcpy(out_w, rec +  8, 2);
   memcpy(out_h, rec + 10, 2);
}

/* Resolve an entry's thumbhash bytes.  On hit returns a pointer into
 * the index buffer (caller must NOT free) and writes the byte length;
 * on absence (or post-validation corruption that produces an OOB
 * offset) returns NULL with *out_len = 0.  Centralises the 1-based
 * offset math so call sites never open-code the `-1`. */
void dp_idx_thumbhash(const pastime_thumbs_index_t *idx, uint32_t e,
      const uint8_t **out_ptr, size_t *out_len)
{
   uint32_t       off;
   uint8_t        len;
   const uint8_t *base;
   *out_ptr = NULL;
   *out_len = 0;
   if (e >= idx->entry_count)
      return;
   off = dp_idx_read_u32(idx->buf,
         idx->entries_off + (size_t)e * DP_IDX_REC_SZ + 12);
   if (off == 0)
      return;
   /* off is 1-based.  Need at least one length byte at (off - 1) plus
    * `len` bytes of payload, all within thumbhash_size. */
   if ((size_t)off > (size_t)idx->thumbhash_size)
      return;
   base = idx->buf + idx->thumbhash_off;
   len  = base[off - 1];
   if ((size_t)off + len > (size_t)idx->thumbhash_size)
      return;
   if (len == 0)
      return;
   *out_ptr = base + off; /* skip the length byte */
   *out_len = len;
}

static uint32_t dp_idx_bcanon_at(
      const pastime_thumbs_index_t *idx, size_t i)
{
   return dp_idx_read_u32(idx->buf,
         idx->bcanon_off + i * DP_IDX_BCAN_SZ);
}

static void dp_idx_bheavy_at(const pastime_thumbs_index_t *idx, size_t i,
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
      const pastime_thumbs_index_t *idx, const char *stem)
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
static size_t dp_lower_bound_heavy(const pastime_thumbs_index_t *idx,
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
static size_t dp_pick_best(const pastime_thumbs_index_t *idx,
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
size_t dp_idx_match(
      const pastime_thumbs_index_t *idx,
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

const char *pastime_thumbs_index_match(
      const pastime_thumbs_index_t *idx,
      const char *rom_basename)
{
   size_t hit_idx = dp_idx_match(idx, rom_basename);
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
      free(idx->entries[i].thumbhash);
   }
   free(idx->entries);
   free(idx);
}

/* Parse JSON into a serialised binary `.idx` buffer.  Shared between
 * the public test entrypoint (which then opens+validates) and the
 * download callbacks (which write the buffer to disk).  On success
 * the caller owns `*out_buf` and frees with free(). */
bool dp_idx_parse_json_to_buffer(const char *json, size_t json_len,
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
#ifdef PASTIME_THUMBS_TEST_BUILD
         dp_h_error
#else
         NULL /* error_handler */
#endif
         );
   /* Free leftover parse-context state regardless of outcome.  An
    * abort mid-entry can leave cur_canonical / cur_thumbhash live;
    * dp_parse_ctx_reset_entry covers both. */
   dp_parse_ctx_reset_entry(&ctx);

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

pastime_thumbs_index_t *pastime_thumbs_index_parse(
      const char *json, size_t json_len)
{
   uint8_t *buf     = NULL;
   size_t   buf_len = 0;
   if (!dp_idx_parse_json_to_buffer(json, json_len, &buf, &buf_len))
      return NULL;
   /* dp_idx_open takes ownership of buf — frees on validation failure. */
   return dp_idx_open(buf, buf_len);
}
