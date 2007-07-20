/* This file is a part of the maildirtools package. See the COPYRIGHT file for
 * details. */

/* Parts of this file were taken from mutt:
 * Copyright (C) 1996-2000 Michael R. Elkins <me@mutt.org>
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <ctype.h>
#include "rfc822.h"
#include "util.h"


/* References vs. In-Reply-To:
 *
 * In-Reply-To contains msg that is directly replied to. (if it contains more
 * than one, I have no idea what that means and what the order is)
 * References contains msgs this one somehow references, iow. the message this
 * one replies to, the message the one this replies to replies to etc. on the
 * order from the start of the thread to the msg directly preceding this one.
 *
 * So... to construct the list of references, we just append the In-Reply-To
 * list to the References list, avoiding duplicates.
 *
 * TJ.
 */


static char *read_rfc822_line (FILE *f, char *line, size_t *linelen);
static void parse_references (char *s, int in_reply_to, GPtrArray *lst);
static char *extract_message_id (const char *s);
static int str_is_in_ptr_array(GPtrArray *ar, char *str);
static void ptr_array_append(GPtrArray *dest, GPtrArray *src);
struct rfc822_header;
static void parse_rfc822_line (struct rfc822_header *hdr, char *line,
	char *p);


#define STRING 256
#define LONG_STRING 1024
#define SKIPWS(c) \
    do { while (*(c) && isspace ((int) *(c))) c++; } while(0)

/* Reads an arbitrarily long header field, and looks ahead for continuation
 * lines.  ``line'' must point to a dynamically allocated string; it is
 * increased if more space is required to fit the whole line.
 */
static char *read_rfc822_line (FILE *f, char *line, size_t *linelen)
{
  char *buf = line;
  char ch;
  size_t offset = 0;

  while (1)
  {
    if (fgets (buf, *linelen - offset, f) == NULL ||    /* end of file or */
        (isspace ((int) *line) && !offset))             /* end of headers */
    {
      *line = 0;
      return (line);
    }

    buf += strlen (buf) - 1;
    if (*buf == '\n')
    {
      /* we did get a full line. remove trailing space */
      while (isspace ((int) *buf))
        *buf-- = 0;     /* we cannot come beyond line's beginning because
                         * it begins with a non-space */

      /* check to see if the next line is a continuation line */
      if ((ch = fgetc (f)) != ' ' && ch != '\t')
      {
        ungetc (ch, f);
        return (line); /* next line is a separate header field or EOH */
      }

      /* eat tabs and spaces from the beginning of the continuation line */
      while ((ch = fgetc (f)) == ' ' || ch == '\t')
        ;
      ungetc (ch, f);
      *++buf = ' '; /* string is still terminated because we removed
                       at least one whitespace char above */
    }

    buf++;
    offset = buf - line;
    if (*linelen < offset + STRING)
    {
      /* grow the buffer */
      *linelen += STRING;
      line = g_realloc (line, *linelen);
      buf = line + offset;
    }
  }
  /* not reached */
}

static void parse_references (char *s, int in_reply_to, GPtrArray *lst)
{
  /*LIST *t, *lst = NULL;*/
  int m, n = 0;
  char *o = NULL, *new, *at;

  while ((s = strtok (s, " \t;")) != NULL)
  {
    /*
     * some mail clients add other garbage besides message-ids, so do a quick
     * check to make sure this looks like a valid message-id
     * some idiotic clients also break their message-ids between lines, deal
     * with that too (give up if it's more than two lines, though)
     */
    new = NULL;

    if (*s == '<')
    {
      n = strlen (s);
      if (s[n-1] != '>')
      {
        o = s;
        s = NULL;
        continue;
      }

      new = g_strdup (s);
    }
    else if (o)
    {
      m = strlen (s);
      if (s[m - 1] == '>')
      {
	new = g_strconcat (o, s, NULL);
      }
    }
    if (new)
    {
      /* make sure that this really does look like a message-id.
       * it should have exactly one @, and if we're looking at
       * an in-reply-to header, make sure that the part before
       * the @ has more than eight characters or it's probably
       * an email address
       */
      if (!(at = strchr (new, '@')) || strchr (at + 1, '@')
          || (in_reply_to && at - new <= 8))
      {
        g_free (new);
        new = NULL;
      }
      else
      {
	if (str_is_in_ptr_array (lst, new))
	{
	  g_free (new);
	  new = NULL;
	}
	else
	  g_ptr_array_add (lst, new);
      }
    }
    o = NULL;
    s = NULL;
  }
}

/* extract the first substring that looks like a message-id */
static char *extract_message_id (const char *s)
{
  const char *p;
  char *r;
  size_t l;

  if ((s = strchr (s, '<')) == NULL || (p = strchr (s, '>')) == NULL)
    return (NULL);
  l = (size_t)(p - s) + 1;
  r = g_malloc (l + 1);
  memcpy (r, s, l);
  r[l] = 0;
  return (r);
}

/** Is the string in the list?
 *
 * \return 0 - no, 1 - yes.
 */
static int str_is_in_ptr_array(GPtrArray *ar, char *str)
{
    for (int i = 0; i < ar->len; i++) {
	char *is = (char *) g_ptr_array_index(ar, i);

	if (strcmp(is, str) == 0)
	    return 1;
    }

    return 0;
}

/** Append one GPtrArray to another. */
static void ptr_array_append(GPtrArray *dest, GPtrArray *src)
{
    for (int i = 0; i < src->len; i++)
	g_ptr_array_add(dest, g_ptr_array_index(src, i));
}

/** Helper struct for #mutt_parse_rfc822_line. */
struct rfc822_header {
    char *msg_id; ///< The message ID.
    GPtrArray *references, ///< List of <code>char *</code>.
	      *in_reply_tos; ///< List of <code>char *</code>.
};

static void parse_rfc822_line (struct rfc822_header *hdr, char *line, char *p)
{
  switch (tolower ((int) line[0]))
  {
    case 'i':
    if (!strcasecmp (line+1, "n-reply-to"))
    {
      parse_references (p, 1, hdr->in_reply_tos);
    }
    break;

    case 'm':
    if (!strcasecmp (line + 1, "essage-id"))
    {
      g_free (hdr->msg_id);
      hdr->msg_id = extract_message_id (p);
    }
    break;

    case 'r':
    if (!strcasecmp (line + 1, "eferences"))
    {
      parse_references (p, 0, hdr->references);
    }
    else if (!strcasecmp (line + 1, "esent-message-id"))
    {
      /* This is probably a hack. TJ. */
      if (!hdr->msg_id)
        hdr->msg_id = extract_message_id (p);
    }
  }
}

void read_rfc822_header (FILE *f, struct message *msg)
{
  char *line = g_malloc (LONG_STRING);
  char *p;
  size_t linelen = LONG_STRING;

  struct rfc822_header hdr;
  hdr.msg_id = NULL;
  hdr.references = g_ptr_array_new ();
  hdr.in_reply_tos = g_ptr_array_new ();

  while (*(line = read_rfc822_line (f, line, &linelen)) != 0)
  {
    if ((p = strpbrk (line, ": \t")) == NULL || *p != ':')
    {
      /* The mutt code looked whether this is the From line and stopped
       * parsing headers otherwise. We just ignore it... TJ. */
      continue; /* just ignore */
    }

    *p = 0;
    p++;
    SKIPWS (p);
    if (!*p)
      continue; /* skip empty header fields */

    parse_rfc822_line (&hdr, line, p);

  }

  /* Save the results. */
  msg->msg_id = hdr.msg_id;
  msg->references = hdr.references;
  ptr_array_append (msg->references, hdr.in_reply_tos);
  g_ptr_array_free (hdr.in_reply_tos, 1);

  g_free (line);
}
