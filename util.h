/* This file is a part of the maildirtools package. See the COPYRIGHT file for
 * details. */

#ifndef UTIL_H
#define UTIL_H

#define _GNU_SOURCE
#include <glib.h>

/* lol. */
#ifndef g_slice_new
#define g_slice_new(t) g_new(t, 1)
#define g_slice_new0(t) g_new0(t, 1)
#define g_slice_free(t, p) g_free(p)
#endif

#endif /* UTIL_H */
