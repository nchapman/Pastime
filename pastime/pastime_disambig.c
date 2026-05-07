/* See pastime_disambig.h for intent. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pastime_disambig.h"

/* Length of the longest common prefix shared by every tag at the given
 * row indices, trimmed back to a bracket-block boundary so we never
 * split mid-token.  For tags ["(USA) (Rev 1)", "(USA) (Rev 2)"] the
 * raw common prefix is "(USA) (Rev ", which trims to "(USA) " and
 * yields differentials "(Rev 1)" / "(Rev 2)".
 *
 * Limitation: the boundary trim walks left for ANY ')' or ']', so a
 * tag with nested brackets like "(USA) (some [bracket] thing)" could
 * split mid-block on the inner ']'.  No-Intro and Redump don't nest,
 * so this is theoretical; revisit only if a real example appears. */
static size_t pd_common_tag_prefix_len(pastime_disambig_row_t *rows,
      const size_t *idx, size_t n)
{
   const char *first;
   size_t      max;
   size_t      i, k;

   if (n < 2)
      return 0;
   first = rows[idx[0]].tag ? rows[idx[0]].tag : "";
   max   = strlen(first);
   if (max == 0)
      return 0;

   /* Reduce `max` to the longest character-prefix shared by all rows.
    * An empty tag anywhere collapses the prefix to zero immediately. */
   for (i = 1; i < n; i++)
   {
      const char *t = rows[idx[i]].tag ? rows[idx[i]].tag : "";
      size_t      tlen = strlen(t);
      size_t      j;
      if (tlen == 0)
         return 0;
      if (tlen < max)
         max = tlen;
      for (j = 0; j < max; j++)
      {
         if (first[j] != t[j])
         {
            max = j;
            break;
         }
      }
      if (max == 0)
         return 0;
   }

   /* Trim back to a bracket boundary so we don't split a block.  Walk
    * left from `max` until we find ')' or ']' (end of a block); skip
    * trailing whitespace after that boundary so the differential
    * starts cleanly at the next '(' / '['. */
   k = max;
   while (k > 0 && first[k - 1] != ')' && first[k - 1] != ']')
      k--;
   while (k < max && (first[k] == ' ' || first[k] == '\t'))
      k++;
   return k;
}

/* Append `qual` to *row->display_name as " (qual)" (wrapping when
 * `qual` doesn't already start with '(') or " qual" (when it does).
 * Allocates a new buffer and frees the old display_name.  Silent
 * no-op on OOM or empty inputs — the caller's prior label survives. */
static void pd_label_append_qualifier(pastime_disambig_row_t *row,
      const char *qual)
{
   char  *new_label;
   const char *base;
   size_t base_len, qual_len, total;
   bool   wrap;

   if (!row || !row->display_name || !*row->display_name
         || !qual || !*qual)
      return;
   base     = *row->display_name;
   base_len = strlen(base);
   qual_len = strlen(qual);
   wrap     = (qual[0] != '(');
   /* "Display Name (qual)" → base + space + ['('] + qual + [')'] + NUL. */
   total    = base_len + 1 + (wrap ? 2 : 0) + qual_len + 1;
   if (!(new_label = (char*)malloc(total)))
      return;
   if (wrap)
      snprintf(new_label, total, "%s (%s)", base, qual);
   else
      snprintf(new_label, total, "%s %s",  base, qual);
   free(*row->display_name);
   *row->display_name = new_label;
}

/* Compute the longest common (boundary-trimmed) tag prefix across the
 * given rows, then for each row append its tail-past-prefix as a
 * qualifier.  Rows whose tail is empty stay bare — siblings carry the
 * distinguishing tail, so context makes the bare row read as "the
 * default". */
static void pd_apply_tag_differential(pastime_disambig_row_t *rows,
      const size_t *idx, size_t n)
{
   size_t common;
   size_t i;
   if (n < 2)
      return;
   common = pd_common_tag_prefix_len(rows, idx, n);
   for (i = 0; i < n; i++)
   {
      pastime_disambig_row_t *r = &rows[idx[i]];
      const char             *tail;
      if (!r->tag)
         continue;
      tail = r->tag + common;
      while (*tail == ' ' || *tail == '\t')
         tail++;
      if (!*tail)
         continue;
      pd_label_append_qualifier(r, tail);
   }
}

void pastime_disambig_run(pastime_disambig_row_t *rows, size_t n,
      pastime_disambig_source_label_fn resolve_source_label,
      void *user)
{
   size_t run_start = 0;
   size_t i;

   if (!rows || n < 2)
      return;

   for (i = 1; i <= n; i++)
   {
      bool    end_of_run;
      size_t  r, run_len;
      bool    sources_differ;
      size_t *idx;
      uint8_t first_src;

      if (i < n)
      {
         /* Two adjacent rows are in the same run iff their currently-
          * pointed-to display_name strings are equal (caller's pre-sort
          * contract guarantees equal labels are contiguous). */
         const char *a = rows[run_start].display_name
            ? *rows[run_start].display_name : "";
         const char *b = rows[i].display_name
            ? *rows[i].display_name : "";
         end_of_run = strcmp(a, b) != 0;
      }
      else
         end_of_run = true;

      if (!end_of_run)
         continue;
      run_len = i - run_start;
      if (run_len < 2)
      {
         run_start = i;
         continue;
      }

      sources_differ = false;
      first_src      = rows[run_start].source_idx;
      for (r = run_start + 1; r < i; r++)
      {
         if (rows[r].source_idx != first_src)
         {
            sources_differ = true;
            break;
         }
      }

      /* Index buffer reused for the intra-source case (full run) and
       * for per-source sub-runs in the mixed case. */
      idx = (size_t*)malloc(run_len * sizeof(*idx));
      if (!idx)
      {
         run_start = i;
         continue;
      }

      if (!sources_differ)
      {
         for (r = 0; r < run_len; r++)
            idx[r] = run_start + r;
         pd_apply_tag_differential(rows, idx, run_len);
      }
      else
      {
         /* Mixed run: every row gets a source label first.  Then any
          * same-source sub-run still containing duplicates needs a
          * second-level tag-tail diff. */
         if (resolve_source_label)
         {
            for (r = run_start; r < i; r++)
            {
               const char *qual = resolve_source_label(
                     rows[r].source_idx, user);
               if (qual && *qual)
                  pd_label_append_qualifier(&rows[r], qual);
            }
         }
         /* Group by source.  O(K^2) where K = run_len; K is tiny
          * (handful of duplicates), so the simple double-walk is
          * fine — no hash needed. */
         for (r = run_start; r < i; r++)
         {
            size_t  sub_n = 0;
            size_t  s;
            uint8_t this_src = rows[r].source_idx;
            bool    is_first = true;
            /* Only start a sub-group at the first row holding this
             * source within the run. */
            for (s = run_start; s < r; s++)
            {
               if (rows[s].source_idx == this_src)
               {
                  is_first = false;
                  break;
               }
            }
            if (!is_first)
               continue;
            for (s = r; s < i; s++)
            {
               if (rows[s].source_idx == this_src)
                  idx[sub_n++] = s;
            }
            if (sub_n >= 2)
               pd_apply_tag_differential(rows, idx, sub_n);
         }
      }

      free(idx);
      run_start = i;
   }
}
