/* See pastime_display_name.h for intent. */

#include <ctype.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "pastime_display_name.h"

/* Only called after dp_strip_brackets.  Trims trailing whitespace, plus
 * a single trailing ',' or '-' if it sits immediately after whitespace
 * — the dangling-punct shape produced by stripping a "(...)" tail off
 * names like "Game, The (USA)" or "Game - (USA)".  Will not eat a
 * legitimate trailing comma/dash that the title itself ends with. */
static void dp_rstrip(char *s)
{
   size_t n = strlen(s);
   /* Phase 1: pure whitespace. */
   while (n > 0)
   {
      unsigned char c = (unsigned char)s[n - 1];
      if (c == ' ' || c == '\t')
         s[--n] = '\0';
      else
         break;
   }
   /* Phase 2: at most one ',' or '-' that was followed by the
    * whitespace we just removed (i.e. preceded by whitespace in the
    * original — bracket-strip residue, never the title's own punct). */
   if (n >= 2)
   {
      unsigned char last = (unsigned char)s[n - 1];
      unsigned char prev = (unsigned char)s[n - 2];
      if ((last == ',' || last == '-')
            && (prev == ' ' || prev == '\t'))
      {
         s[n - 1] = '\0';
         /* Re-trim whitespace exposed by dropping the punct. */
         n--;
         while (n > 0
               && (s[n - 1] == ' ' || s[n - 1] == '\t'))
            s[--n] = '\0';
      }
   }
}

/* No-Intro stores articles trailing so the filename sorts naturally:
 *
 *   "Legend of Zelda, The - Oracle of Ages"
 *   "Bug's Life, A"
 *
 * Rotate the article to the front for display.  The ", <article>"
 * fragment may be at end-of-string OR followed by a subtitle separator
 * (" - ..."), so we look for the comma+space+article pattern and a
 * boundary after it.  Article casing is taken from the canonical
 * table, not the source — guarantees "The" not "the". */
static void dp_rotate_article(char *s, size_t buf_size)
{
   static const char *articles[] = { "The", "An", "A" };
   size_t n;
   size_t i;

   if (buf_size == 0)
      return;
   n = strlen(s);
   for (i = 0; i + 2 < n; i++)
   {
      size_t ai;
      if (s[i] != ',' || s[i + 1] != ' ')
         continue;
      for (ai = 0; ai < sizeof(articles) / sizeof(articles[0]); ai++)
      {
         const char *art  = articles[ai];
         size_t      alen = strlen(art);
         size_t      end  = i + 2 + alen;
         size_t      prefix_len;
         size_t      suffix_len;
         size_t      need;

         if (end > n)
            continue;
         if (strncasecmp(s + i + 2, art, alen) != 0)
            continue;
         /* Boundary check: end-of-string or whitespace.  Stops us
          * from rewriting "Foo, Antonio" as "An Foo, tonio". */
         if (end < n && s[end] != ' ' && s[end] != '\t')
            continue;

         prefix_len = i;
         suffix_len = n - end;
         /* article + ' ' + prefix + suffix + NUL.  Skip the rotation
          * entirely if the rewrite would overflow the caller's buffer
          * — leave the comma form alone rather than truncating. */
         need = alen + 1 + prefix_len + suffix_len + 1;
         if (need > buf_size)
            return;
         /* Use memmove for the prefix copy: source and dest overlap
          * (we're prepending to the same buffer). */
         memmove(s + alen + 1, s, prefix_len);
         memcpy(s, art, alen);
         s[alen] = ' ';
         memmove(s + alen + 1 + prefix_len, s + end, suffix_len);
         s[alen + 1 + prefix_len + suffix_len] = '\0';
         return;
      }
   }
}

/* Strip every "(...)" and "[...]" block in `s`, in place.  Doesn't
 * handle nesting (No-Intro / Redump don't nest).  Adjacent runs like
 * "Foo (USA) (Rev 1)" collapse cleanly because we re-scan from the
 * write head after each close. */
static void dp_strip_brackets(char *s)
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

void pastime_display_name_strip_rom_extension(char *name)
{
   size_t n;
   size_t i;
   /* Find the last '.' after the last path separator (defensive — the
    * caller passes a basename, but stay safe in case of stray slashes).
    * Truncate there; that handles the common single-extension case
    * (.smc, .nes, .p8, .png, ...). */
   if (!name || !*name)
      return;
   n = strlen(name);
   for (i = n; i > 0; i--)
   {
      char c = name[i - 1];
      if (c == '/' || c == '\\')
         break;
      if (c == '.')
      {
         name[i - 1] = '\0';
         n = i - 1;
         break;
      }
   }
   /* Second peel for ".p8" — PICO-8 PNG-encoded cartridges (.p8.png).
    * Anchored to that exact string so we never strip an arbitrary short
    * extension off non-PICO-8 names. */
   if (n >= 3
         && name[n - 3] == '.'
         && name[n - 2] == 'p'
         && name[n - 1] == '8')
      name[n - 3] = '\0';
}

void pastime_display_name_clean(const char *raw,
      char *out, size_t out_size)
{
   pastime_display_name_clean_keep_tag(raw, out, out_size, NULL, 0);
}

/* Find the start of the trailing run of "(...)" / "[...]" blocks in s.
 * Returns strlen(s) when there's no trailing tag.  The substring from
 * the returned offset to strlen(s) is the tag verbatim (including any
 * inter-block whitespace).  Defensive against unbalanced brackets:
 * stops walking left if we can't find a matching opener. */
static size_t dp_find_trailing_tag_start(const char *s)
{
   size_t end;
   size_t cur;

   if (!s)
      return 0;
   end = strlen(s);
   /* Trim trailing whitespace from the search position so a name like
    * "Foo (USA) " still finds the trailing block. */
   while (end > 0 && (s[end - 1] == ' ' || s[end - 1] == '\t'))
      end--;
   cur = end;
   while (cur > 0)
   {
      char   close = s[cur - 1];
      char   open;
      size_t k;
      if (close == ')')      open = '(';
      else if (close == ']') open = '[';
      else                   break;
      /* Walk left for the matching opener.  Won't traverse past the
       * start of the string. */
      k = cur - 1;
      while (k > 0 && s[k - 1] != open)
         k--;
      if (k == 0 || s[k - 1] != open)
         break;        /* unmatched closer → not a tag we trust */
      cur = k - 1;
      /* Allow multiple "(USA) (Rev 1)" blocks separated by spaces. */
      while (cur > 0 && (s[cur - 1] == ' ' || s[cur - 1] == '\t'))
         cur--;
   }
   return cur;
}

void pastime_display_name_clean_keep_tag(const char *raw,
      char *out, size_t out_size,
      char *tag_out, size_t tag_size)
{
   size_t len;

   if (out && out_size > 0)
      out[0] = '\0';
   if (tag_out && tag_size > 0)
      tag_out[0] = '\0';
   if (!raw || !*raw)
      return;

   /* Capture the trailing tag (verbatim, with brackets) before any
    * cleanup runs.  Skip the leading whitespace separator so the tag
    * starts at its first '(' / '['. */
   if (tag_out && tag_size > 0)
   {
      size_t tag_start = dp_find_trailing_tag_start(raw);
      size_t raw_len   = strlen(raw);
      while (tag_start < raw_len
            && (raw[tag_start] == ' ' || raw[tag_start] == '\t'))
         tag_start++;
      if (tag_start < raw_len)
      {
         size_t tag_len = raw_len - tag_start;
         /* Trim trailing whitespace so we don't carry a dangling space. */
         while (tag_len > 0
               && (raw[tag_start + tag_len - 1] == ' '
                   || raw[tag_start + tag_len - 1] == '\t'))
            tag_len--;
         if (tag_len >= tag_size)
            tag_len = tag_size - 1;
         memcpy(tag_out, raw + tag_start, tag_len);
         tag_out[tag_len] = '\0';
      }
   }

   if (!out || out_size == 0)
      return;

   len = strlen(raw);
   if (len >= out_size)
      len = out_size - 1;
   memcpy(out, raw, len);
   out[len] = '\0';

   dp_strip_brackets(out);
   dp_rstrip(out);
   dp_rotate_article(out, out_size);
}

void pastime_display_name_sort_key(const char *display,
      char *out, size_t out_size)
{
   const char *src;
   size_t i;

   if (!out || out_size == 0)
      return;
   out[0] = '\0';
   if (!display || !*display)
      return;

   src = display;
   /* Strip ONE leading article so "The X" sorts as "X".  Case-insensitive
    * but only at the very start, so internal "The"s are untouched. */
   if (strncasecmp(src, "the ", 4) == 0)
      src += 4;
   else if (strncasecmp(src, "an ",  3) == 0)
      src += 3;
   else if (strncasecmp(src, "a ",   2) == 0)
      src += 2;

   for (i = 0; i + 1 < out_size && src[i]; i++)
      out[i] = (char)tolower((unsigned char)src[i]);
   out[i] = '\0';
}

void pastime_format_relative_time(int64_t mtime, int64_t now,
      char *out, size_t out_len)
{
   int64_t delta;
   int     n;

   if (!out || out_len == 0)
      return;
   out[0] = '\0';
   if (mtime <= 0)
   {
      snprintf(out, out_len, "Unknown");
      return;
   }

   delta = now - mtime;
   if (delta < 0)
      delta = 0;

   if (delta < 60)
      snprintf(out, out_len, "Just now");
   else if (delta < 60 * 60)
   {
      n = (int)(delta / 60);
      snprintf(out, out_len, "%d minute%s ago", n, n == 1 ? "" : "s");
   }
   else if (delta < 24 * 60 * 60)
   {
      n = (int)(delta / (60 * 60));
      snprintf(out, out_len, "%d hour%s ago", n, n == 1 ? "" : "s");
   }
   else if (delta < 2 * 24 * 60 * 60)
      snprintf(out, out_len, "Yesterday");
   else
   {
      n = (int)(delta / (24 * 60 * 60));
      snprintf(out, out_len, "%d days ago", n);
   }
}

const char *pastime_display_name_strip_sort_prefix(const char *name)
{
   const char *p = name;
   if (!p)
      return p;
   if (!(*p >= '0' && *p <= '9'))
      return name;
   while (*p >= '0' && *p <= '9')
      p++;
   if (*p != ')')
      return name;
   p++;
   while (*p == ' ' || *p == '\t')
      p++;
   return p;
}
