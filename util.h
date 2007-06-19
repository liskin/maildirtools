#ifndef UTIL_H
#define UTIL_H

#include <glib.h>

/* lol. */
#ifndef g_slice_new
#define g_slice_new(t) g_new(t, 1)
#define g_slice_new0(t) g_new0(t, 1)
#define g_slice_free(t, p) g_free(p)
#endif

#endif /* UTIL_H */
