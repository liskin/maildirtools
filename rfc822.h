/* This file is a part of the maildirtools package. See the COPYRIGHT file for
 * details. */

#ifndef RFC822_H
#define RFC822_H

#define _GNU_SOURCE
#include <glib.h>
#include "maildir.h"

void read_rfc822_header (FILE *f, struct message *msg);

#endif /* RFC822_H */
