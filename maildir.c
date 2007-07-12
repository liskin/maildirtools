#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#include "maildir.h"
#include "util.h"

#define DNOTIFY_SIGNAL (SIGRTMIN + 1)

static int verbose = 0;
#define VERBOSE(x) do { if (verbose) { x; } } while (0)

/** A set of dirty directories. */
static volatile fd_set dirty_fds;
static int sig_inited = 0;


/* Forward decls */
static void sig_handler(int sig, siginfo_t *si, void *data);
static void sig_init(void);
static void sig_block(int block);
static void sig_wait(int (*f) (void *), void *param);
static int sig_fd_isset(int fd, int clear, int sig);
static int dnotify(int fd, int flags);
static int maildirpp_load_subfolders_list(struct maildirpp *md);
static int maildirpp_compare_folder(struct maildir_folder **a,
	struct maildir_folder **b);
static void maildirpp_free_subfolders_list(struct maildirpp *md);
static int maildir_folder_open(struct maildir_folder *mdf, const char *path);
static void maildir_folder_close(struct maildir_folder *mdf);
static void maildir_folder_close_and_free(struct maildir_folder *mdf);
static int maildirpp_dirty2(struct maildirpp *md);
static void maildir_folder_walk_messages(struct maildir_folder *mdf,
	GArray *funcs, int walk_subdirs);
static void maildir_folder_stats_clear(struct maildir_folder *mdf);
static void maildir_folder_stats_message(
	struct maildir_folder_walk_messages_params *params);
static int message_parse_flags(const char *name);


/** dnotify signal handler. */
static void sig_handler(int sig, siginfo_t *si, void *data)
{
    assert(si != NULL);
    FD_SET(si->si_fd, &dirty_fds);
}

/** Initialize the signal handler and dirty_fds set. */
static void sig_init(void)
{
    struct sigaction act;

    if (sig_inited)
	return;
    sig_inited = 1;

    FD_ZERO(&dirty_fds);

    act.sa_sigaction = sig_handler;
    sigemptyset(&act.sa_mask);
    act.sa_flags = SA_SIGINFO;
    sigaction(DNOTIFY_SIGNAL, &act, NULL);

    sig_block(0);
}

/** Block/unblock the dnotify signal. */
static void sig_block(int block)
{
    sigset_t mask;

    sigemptyset(&mask);
    sigaddset(&mask, DNOTIFY_SIGNAL);
    sigprocmask(block ? SIG_BLOCK : SIG_UNBLOCK, &mask, NULL);
}

/** Block the signal, run the function and if it returns 1, return, otherwise
 * unblock and pause. */
static void sig_wait(int (*f) (void *), void *param)
{
    sigset_t mask, old_mask;

    sigemptyset(&mask);
    sigaddset(&mask, DNOTIFY_SIGNAL);
    sigprocmask(SIG_BLOCK, &mask, &old_mask);
    if (f(param) == 0)
	sigsuspend(&old_mask);
    sigprocmask(SIG_UNBLOCK, &mask, NULL);
}

/** Check and eventually clear/set a given fd in the dirty_fds set. 
 * \param clear 0 - nothing, 1 - clear, 2 - set
 * \param sig Block/unblock the signal while accessing? */
static int sig_fd_isset(int fd, int clear, int sig)
{
    int ret;

    assert(fd >= 0);

    if (sig) sig_block(1);
    ret = FD_ISSET(fd, &dirty_fds);
    if (clear == 1) FD_CLR(fd, &dirty_fds);
    if (clear == 2) FD_SET(fd, &dirty_fds);
    if (sig) sig_block(0);

    return ret;
}

/** Set up dnotify on a dir. */
static int dnotify(int fd, int flags)
{
    /* Clear the dirty flag */
    sig_fd_isset(fd, 1, 1);

    /* Set up dnotify */
    if (fcntl(fd, F_SETSIG, SIGRTMIN + 1) == -1) {
	perror("fnctl(F_SETSIG)"); return -1;
    }

    if (fcntl(fd, F_NOTIFY, flags) == -1) {
	perror("fnctl(F_NOTIFY)"); return -1;
    }

    return 0;
}

/** Open the given maildir++.
 * \return 0 - ok, -1 - error.
 */
int maildirpp_open(struct maildirpp *md, const char *path)
{
    memset(md, 0, sizeof(struct maildirpp));

    /* Check the path len. (add 1 for /)
     * (just a stupid safety check, we need much more of course) */
    if (strlen(path) + 1 >= PATH_MAX) {
	fprintf(stderr, "Overlong path: %s\n", path); goto err1;
    }
    strcpy(md->path, path);

    /* Init signal handler */
    sig_init();

    /* Open the dir */
    md->dir = opendir(path);
    if (!md->dir) {
	perror(path); goto err1;
    }

    /* Set up dnotify */
    if (dnotify(dirfd(md->dir), DN_CREATE|DN_DELETE|DN_RENAME|DN_MULTISHOT))
	goto err2;

    if (maildirpp_load_subfolders_list(md))
	goto err3;

    return 0;

err3:
    maildirpp_free_subfolders_list(md);
err2:
    closedir(md->dir);
err1:
    return -1;
}

/** Load the list of subfolders */
static int maildirpp_load_subfolders_list(struct maildirpp *md)
{
    char path2[PATH_MAX];
    size_t path2_len;

    /* This is not public API, the path + "/" _does_ fit into PATH_LEN. */
    strcpy(path2, md->path); path2_len = strlen(path2);
    strcpy(path2 + path2_len, "/"); path2_len++;

    /* Init the subfolders/subdirs arrays */
    assert(md->subfolders == NULL);
    md->subfolders = g_ptr_array_new();
    assert(md->subdirs == NULL);
    md->subdirs = g_ptr_array_new();

    /* Unset dirty flag and rewind dir */
    sig_fd_isset(dirfd(md->dir), 1, 1);
    rewinddir(md->dir);

    /* Load the list of subfolders */
    struct dirent *dent;
    while (1) {
	errno = 0;
	if ((dent = readdir(md->dir)) == 0) {
	    if (errno == 0)
		break;
	    perror("readdir"); return -1;
	}

	/* Filter out "..". */
	if (strcmp(dent->d_name,"..") /*&& dent->d_name[0] == '.'*/) {
	    /* Another dumb len check */
	    size_t name_len = strlen(dent->d_name);
	    if (name_len + path2_len + 4 >= PATH_MAX) {
		fprintf(stderr, "Overlong path: %s%s/new\n", path2,
			dent->d_name);
		continue;
	    }

	    int opened = 0;

	    /* Does it have a "new" subdir? */
	    strcpy(path2 + path2_len, dent->d_name);
	    strcpy(path2 + path2_len + name_len, "/new");
	    if (access(path2, X_OK) == 0) {
		/* Ok, open and push */

		path2[path2_len + name_len] = 0;

		struct maildir_folder *folder =
		    g_slice_new0(struct maildir_folder);
		if (maildir_folder_open(folder, path2) == 0) {
		    g_ptr_array_add(md->subfolders, folder);
		    opened = 1;
		} else
		    g_slice_free(struct maildir_folder, folder);
	    }

	    if (!opened) {
		/* Looks like a maildir folder but is not. Watch it in case it
		 * becomes a folder. */

		path2[path2_len + name_len] = 0;

		DIR *subdir = opendir(path2);
		if (subdir) {
		    if (dnotify(dirfd(subdir),
				DN_CREATE|DN_DELETE|DN_RENAME|DN_MULTISHOT))
			closedir(subdir);
		    else
			g_ptr_array_add(md->subdirs, subdir);
		} else
		    VERBOSE(perror(path2));
	    }
	}
    }

    /* Sort them */
    g_ptr_array_sort(md->subfolders, (GCompareFunc) maildirpp_compare_folder);

    return 0;
}

static int maildirpp_compare_folder(struct maildir_folder **a,
	struct maildir_folder **b)
{
    return strcmp((*a)->path, (*b)->path);
}

/** Free the list of subfolders. */
static void maildirpp_free_subfolders_list(struct maildirpp *md)
{
    assert(md->subfolders != NULL);
    g_ptr_array_foreach(md->subfolders,
	    (GFunc) maildir_folder_close_and_free, 0);
    g_ptr_array_free(md->subfolders, 1);
    md->subfolders = 0;

    assert(md->subdirs != NULL);
    g_ptr_array_foreach(md->subdirs, (GFunc) closedir, 0);
    g_ptr_array_free(md->subdirs, 1);
    md->subdirs = 0;
}

/** Refresh the list of subfolders. */
int maildirpp_refresh_subfolders_list(struct maildirpp *md)
{
    maildirpp_free_subfolders_list(md);
    return maildirpp_load_subfolders_list(md);
}

/** Close the given maildir++. */
void maildirpp_close(struct maildirpp *md)
{
    maildirpp_free_subfolders_list(md);

    assert(md->dir != NULL);
    closedir(md->dir);

    memset(md, 0, sizeof(struct maildirpp));
}

/** Open a given subfolder. */
static int maildir_folder_open(struct maildir_folder *mdf, const char *path)
{
    /*memset(mdf, 0, sizeof(struct maildir_folder));*/

    char path2[PATH_MAX];
    size_t path_len;

    /* This is not public API, the path _does_ fit into PATH_LEN.
     * (so does path/new, path/cur or path/tmp) */
    path_len = strlen(path);
    strcpy(mdf->path, path);
    strcpy(path2, path);

    /* Open the new subdir */
    strcpy(path2 + path_len, "/new");
    mdf->dir_new = opendir(path2);
    if (!mdf->dir_new) {
	VERBOSE(perror(path2)); goto err1;
    }

    /* Open the cur subdir */
    strcpy(path2 + path_len, "/cur");
    mdf->dir_cur = opendir(path2);
    if (!mdf->dir_cur) {
	VERBOSE(perror(path2)); goto err2;
    }

    /* Set up dnotify */
    if (dnotify(dirfd(mdf->dir_new),
		DN_CREATE|DN_DELETE|DN_RENAME|DN_MODIFY|DN_MULTISHOT))
	goto err3;
    if (dnotify(dirfd(mdf->dir_cur),
		DN_CREATE|DN_DELETE|DN_RENAME|DN_MODIFY|DN_MULTISHOT))
	goto err3;

    /* The folder is dirty by default, because we haven't read any messages
     * yet. */
    sig_fd_isset(dirfd(mdf->dir_new), 2, 1);
    sig_fd_isset(dirfd(mdf->dir_cur), 2, 1);

    return 0;

err3:
    closedir(mdf->dir_cur);
err2:
    closedir(mdf->dir_new);
err1:
    return -1;
}

/** Close a given subfolder. */
static void maildir_folder_close(struct maildir_folder *mdf)
{
    assert(mdf->dir_cur != NULL);
    closedir(mdf->dir_cur);
    assert(mdf->dir_new != NULL);
    closedir(mdf->dir_new);

    if (mdf->stats)
	g_slice_free(struct maildir_folder_stats, mdf->stats);

    /*memset(mdf, 0, sizeof(struct maildir_folder));*/
}

/** Close a given subfolder and free the pointer. Helper function for
 * g_ptr_array_foreach. */
static void maildir_folder_close_and_free(struct maildir_folder *mdf)
{
    maildir_folder_close(mdf);
    g_slice_free(struct maildir_folder, mdf);
}

/** Is the maildir++ dirty (has the list of subfolders changed?)
 * \param dont_block - non-public API, just pass 0 */
int maildirpp_dirty(struct maildirpp *md, int dont_block)
{
    int ret = 0;

    if (!dont_block) sig_block(1);
    ret = sig_fd_isset(dirfd(md->dir), 0, 0);
    assert(md->subdirs != NULL);
    for (int i = 0; !ret && i < md->subdirs->len; i++) {
	DIR *subdir = (DIR *) g_ptr_array_index(md->subdirs, i);
	ret |= sig_fd_isset(dirfd(subdir), 0, 0);
    }
    if (!dont_block) sig_block(0);

    return ret;
}

/** Is any of the subfolders dirty?
 * (message added/removed/changed status/modified)
 * \param dont_block - non-public API, just pass 0 */
int maildirpp_dirty_subfolders(struct maildirpp *md, int dont_block)
{
    int ret = 0;

    if (!dont_block) sig_block(1);
    assert(md->subfolders != NULL);
    for (int i = 0; !ret && i < md->subfolders->len; i++) {
	struct maildir_folder *mdf =
	    (struct maildir_folder *) g_ptr_array_index(md->subfolders, i);
	ret |= sig_fd_isset(dirfd(mdf->dir_new), 0, 0);
	ret |= sig_fd_isset(dirfd(mdf->dir_cur), 0, 0);
    }
    if (!dont_block) sig_block(0);

    return ret;
}

static int maildirpp_dirty2(struct maildirpp *md)
{
    return maildirpp_dirty(md, 1) || maildirpp_dirty_subfolders(md, 1);
}

/** Reliable way to wait for signal or just return if it's dirty */
void maildirpp_pause_if_not_dirty(struct maildirpp *md)
{
    sig_wait((int (*) (void *)) maildirpp_dirty2, (void *) md);
}

/** Set verbosity. */
void maildirpp_set_verbose(int new_verbose)
{
    verbose = new_verbose;
}

/** Walk the list of messages, calling the specified functions of type
 * <code>void (*)(struct maildir_folder_walk_messages_params *params)</code>.
 *
 * \param walk_subdirs Mask of SD_CUR, SD_NEW -- subdirs to be walked.
 */
static void maildir_folder_walk_messages(struct maildir_folder *mdf,
	GArray *funcs, int walk_subdirs)
{
    /* Prepare the path string */
    char path2[PATH_MAX];
    size_t path2_len;

    strcpy(path2, mdf->path);
    path2_len = strlen(path2);
    if (path2_len + 5 >= PATH_MAX) {
	fprintf(stderr, "Overlong path: %s/new/\n", path2);
	return; /* Should we abort instead? */
    }

    /* Unset dirty flag and rewind dirs */
    if (walk_subdirs & SD_NEW) {
	sig_fd_isset(dirfd(mdf->dir_new), 1, 1);
	rewinddir(mdf->dir_new);
    }
    if (walk_subdirs & SD_CUR) {
	sig_fd_isset(dirfd(mdf->dir_cur), 1, 1);
	rewinddir(mdf->dir_cur);
    }

    /* Load the list of subfolders */
    struct dirent *dent;
    struct maildir_folder_walk_messages_params params = { .mdf = mdf };
    struct stat st;
    static const char subdirs[2][6] = { "/new/", "/cur/" };

    for (int subdir = 0; subdir < 2; subdir++) {
	if (!(walk_subdirs & (subdir ? SD_CUR : SD_NEW)))
	    continue;

	strcpy(path2 + path2_len, subdirs[subdir]);
	while (1) {
	    errno = 0;
	    if ((dent = readdir(subdir ? mdf->dir_cur : mdf->dir_new)) == 0) {
		if (errno == 0)
		    break;
		perror("readdir"); return; /* What to do? */
	    }

	    if (!strcmp(dent->d_name, ".") || !strcmp(dent->d_name, ".."))
		continue;

	    if (path2_len + 5 + strlen(dent->d_name) >= PATH_MAX) {
		fprintf(stderr, "Overlong path: %s%s%s\n", path2,
			subdirs[subdir], dent->d_name);
		continue; /* Should we abort instead? */
	    }

	    strcpy(path2 + path2_len + 5, dent->d_name);
	    if (stat(path2, &st)) {
		VERBOSE(perror(path2)); continue;
	    }

	    if (!S_ISREG(st.st_mode))
		continue;

	    params.msg_name = dent->d_name;
	    for (int i = 0; i < funcs->len; i++) {
		maildir_folder_walk_messages_func f =
		    g_array_index(funcs, maildir_folder_walk_messages_func, i);
		f(&params);
	    }
	}
    }

    /*g_ptr_array_free(funcs, 1);*/
}

/** Walk all dirty subfolders, calling the specified functions:
 * For each dirty subfolder:
 *   of type <code>void (*)(struct maildir_folder *)</code>
 * For each message in each dirty subfolder:
 *   of type
 *   <code>void (*)(struct maildir_folder_walk_messages_params *)</code>.
 *
 * \param subdirs See #maildir_folder_walk_messages.
 */
void maildirpp_folders_walk(struct maildirpp *md, GArray *folder_funcs,
	GArray *msgs_funcs, int subdirs)
{
    assert(md->subfolders != NULL);

    /* For each folder: */
    for (int i = 0; i < md->subfolders->len; i++) {
	struct maildir_folder *mdf =
	    (struct maildir_folder *) g_ptr_array_index(md->subfolders, i);

	/* For each dirty folder: */
	if (sig_fd_isset(dirfd(mdf->dir_new), 0, 1) ||
		sig_fd_isset(dirfd(mdf->dir_cur), 0, 1)) {
	    /* Call the folder functions. */
	    for (int j = 0; j < folder_funcs->len; j++) {
		maildir_folder_walk_func f =
		    g_array_index(folder_funcs, maildir_folder_walk_func, j);
		f(mdf);
	    }

	    /* Call the message functions. */
	    maildir_folder_walk_messages(mdf, msgs_funcs, subdirs);
	}
    }
}

/** Load the requested data for dirty folders.
 *
 * \param subdirs See #maildir_folder_walk_messages.
 */
void maildirpp_folders_fill(struct maildirpp *md, int data, int subdirs)
{
    assert(md->subfolders != NULL);

    GArray *folder_funcs = g_array_new(0, 0,
	    sizeof(maildir_folder_walk_func));
    GArray *msgs_funcs = g_array_new(0, 0,
	    sizeof(maildir_folder_walk_messages_func));

    if (data & MFD_STATS) {
	maildir_folder_walk_func ff = maildir_folder_stats_clear;
	maildir_folder_walk_messages_func mf = maildir_folder_stats_message;
	g_array_append_val(folder_funcs, ff);
	g_array_append_val(msgs_funcs, mf);
    }

    maildirpp_folders_walk(md, folder_funcs, msgs_funcs, subdirs);

    g_array_free(msgs_funcs, 1);
    g_array_free(folder_funcs, 1);
}

/** Free the current and allocate a new stats structure for a given folder
 * (which is going to be walked).
 */
static void maildir_folder_stats_clear(struct maildir_folder *mdf)
{
    if (mdf->stats)
	g_slice_free(struct maildir_folder_stats, mdf->stats);
    mdf->stats = g_slice_new0(struct maildir_folder_stats);
}

/** Count in the message. */
static void maildir_folder_stats_message(
	struct maildir_folder_walk_messages_params *params)
{
    params->mdf->stats->msgs++;

    int flags = message_parse_flags(params->msg_name);

    if (flags & MF_PASSED) params->mdf->stats->passed++; 
    if (flags & MF_REPLIED) params->mdf->stats->replied++; 
    if (flags & MF_SEEN) params->mdf->stats->seen++; 
    if (flags & MF_TRASHED) params->mdf->stats->trashed++; 
    if (flags & MF_DRAFT) params->mdf->stats->draft++; 
    if (flags & MF_FLAGGED) params->mdf->stats->flagged++;

    /* A message is considered new, if its flags are empty or contain just
     * MF_FLAGGED. (because you may flag the message in sieve/procmail) */
    if (flags == 0 || flags == MF_FLAGGED)
	params->mdf->stats->new++;
}

/** Parse flags of a message. */
static int message_parse_flags(const char *name)
{
    int ret = 0;

    const char *flags = strstr(name, ":2,");
    if (!flags)
	return 0;
    flags += 3;

    for (; *flags; flags++)
	switch (*flags) {
	    case 'P': ret |= MF_PASSED; break;
	    case 'R': ret |= MF_REPLIED; break;
	    case 'S': ret |= MF_SEEN; break;
	    case 'T': ret |= MF_TRASHED; break;
	    case 'D': ret |= MF_DRAFT; break;
	    case 'F': ret |= MF_FLAGGED; break;
	}

    return ret;
}
