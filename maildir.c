/* This file is a part of the maildirtools package. See the COPYRIGHT file for
 * details. */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#include "maildir.h"
#include "rfc822.h"
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
enum { SFI_ISSET = 0, SFI_CLEAR = 1, SFI_SET   = 2 };
enum { SFI_DONTBLOCK = 0, SFI_BLOCK = 1 };
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
static int message_open(struct message *msg);
static void message_free(struct message *msg);
static void message_free_and_free(struct message *msg);
static void maildir_folder_messages_prepare(struct maildir_folder *mdf);
static void maildir_folder_messages_post(struct maildir_folder *mdf);
static void maildir_folder_messages_msg(
	struct maildir_folder_walk_messages_params *params);


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
    act.sa_flags = SA_SIGINFO | SA_RESTART;
    sigaction(DNOTIFY_SIGNAL, &act, NULL);

    act.sa_handler = SIG_IGN;
    act.sa_flags = SA_RESTART;
    sigaction(SIGIO, &act, NULL);

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
    if (clear == SFI_CLEAR) FD_CLR(fd, &dirty_fds);
    if (clear == SFI_SET) FD_SET(fd, &dirty_fds);
    if (sig) sig_block(0);

    return ret;
}

/** Set up dnotify on a dir. */
static int dnotify(int fd, int flags)
{
    /* Clear the dirty flag */
    sig_fd_isset(fd, SFI_CLEAR, SFI_BLOCK);

    /* Set up dnotify */
    if (fcntl(fd, F_SETSIG, DNOTIFY_SIGNAL) == -1) {
	perror("fcntl(F_SETSIG)"); return -1;
    }

    if (fcntl(fd, F_NOTIFY, flags) == -1) {
	perror("fcntl(F_NOTIFY)"); return -1;
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
    sig_fd_isset(dirfd(md->dir), SFI_CLEAR, SFI_BLOCK);
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
		/* The zero ---^ is important! */
		folder->md = md;
		if (maildir_folder_open(folder, path2) == 0) {
		    g_ptr_array_add(md->subfolders, folder);
		    opened = 1;
		} else
		    g_slice_free(struct maildir_folder, folder);
	    }

	    if (!opened &&
		    strcmp(dent->d_name, "new") &&
		    strcmp(dent->d_name, "cur") &&
		    strcmp(dent->d_name, "tmp")) {
		/* Looks like a maildir folder but is not. Watch it in case it
		 * becomes a folder. */
		/* Since we don't require '.' at the beginning of a mailbox
		 * name, exclude new/cur/tmp. */

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
    /* We assume the structure was zero-filled at allocation. */
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
    sig_fd_isset(dirfd(mdf->dir_new), SFI_SET, SFI_BLOCK);
    sig_fd_isset(dirfd(mdf->dir_cur), SFI_SET, SFI_BLOCK);

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
    if (mdf->messages)
	g_tree_destroy(mdf->messages);
    assert(mdf->old_messages == NULL);

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
    ret = sig_fd_isset(dirfd(md->dir), SFI_ISSET, SFI_DONTBLOCK);
    assert(md->subdirs != NULL);
    for (int i = 0; !ret && i < md->subdirs->len; i++) {
	DIR *subdir = (DIR *) g_ptr_array_index(md->subdirs, i);
	ret |= sig_fd_isset(dirfd(subdir), SFI_ISSET, SFI_DONTBLOCK);
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
	ret |= sig_fd_isset(dirfd(mdf->dir_new), SFI_ISSET, SFI_DONTBLOCK);
	ret |= sig_fd_isset(dirfd(mdf->dir_cur), SFI_ISSET, SFI_DONTBLOCK);
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
	sig_fd_isset(dirfd(mdf->dir_new), SFI_CLEAR, SFI_BLOCK);
	rewinddir(mdf->dir_new);
    }
    if (walk_subdirs & SD_CUR) {
	sig_fd_isset(dirfd(mdf->dir_cur), SFI_CLEAR, SFI_BLOCK);
	rewinddir(mdf->dir_cur);
    }

    /* Load the list of subfolders */
    struct dirent *dent;
    struct maildir_folder_walk_messages_params params = { .mdf = mdf };
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

#if 0
	    /* Stat the messages when walking them? This significantly
	     * sacrifices performance but makes sure the files in new/ and
	     * cur/ are ordinary files. */

	    struct stat st;
	    if (stat(path2, &st)) {
		VERBOSE(perror(path2)); continue;
	    }

	    if (!S_ISREG(st.st_mode))
		continue;
#endif

	    params.msg_name = dent->d_name;
	    params.msg_full_path = path2;
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
void maildirpp_folders_walk(struct maildirpp *md,
	GArray *folder_pre_funcs, GArray *folder_post_funcs,
	GArray *msgs_funcs, int subdirs)
{
    assert(md->subfolders != NULL);

    /* For each folder: */
    for (int i = 0; i < md->subfolders->len; i++) {
	struct maildir_folder *mdf =
	    (struct maildir_folder *) g_ptr_array_index(md->subfolders, i);

	/* For each dirty folder: */
	if (sig_fd_isset(dirfd(mdf->dir_new), SFI_ISSET, SFI_BLOCK) ||
		sig_fd_isset(dirfd(mdf->dir_cur), SFI_ISSET, SFI_BLOCK)) {
	    /* Call the folder pre functions. */
	    for (int j = 0; j < folder_pre_funcs->len; j++) {
		maildir_folder_walk_func f =
		    g_array_index(folder_pre_funcs,
			    maildir_folder_walk_func, j);
		f(mdf);
	    }

	    /* Call the message functions. */
	    if (msgs_funcs->len > 0)
		maildir_folder_walk_messages(mdf, msgs_funcs, subdirs);

	    /* Call the folder post functions. */
	    for (int j = 0; j < folder_post_funcs->len; j++) {
		maildir_folder_walk_func f =
		    g_array_index(folder_post_funcs,
			    maildir_folder_walk_func, j);
		f(mdf);
	    }
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

    GArray *folder_pre_funcs = g_array_new(0, 0,
	    sizeof(maildir_folder_walk_func));
    GArray *folder_post_funcs = g_array_new(0, 0,
	    sizeof(maildir_folder_walk_func));
    GArray *msgs_funcs = g_array_new(0, 0,
	    sizeof(maildir_folder_walk_messages_func));

    if (data & MFD_STATS) {
	maildir_folder_walk_func ff = maildir_folder_stats_clear;
	maildir_folder_walk_messages_func mf = maildir_folder_stats_message;
	g_array_append_val(folder_pre_funcs, ff);
	g_array_append_val(msgs_funcs, mf);
    }

    if (data & MFD_MSGS) {
	maildir_folder_walk_func ff = maildir_folder_messages_prepare,
				 ff2 = maildir_folder_messages_post;
	maildir_folder_walk_messages_func mf = maildir_folder_messages_msg;
	g_array_append_val(folder_pre_funcs, ff);
	g_array_append_val(msgs_funcs, mf);
	g_array_append_val(folder_post_funcs, ff2);
    }

    maildirpp_folders_walk(md, folder_pre_funcs, folder_post_funcs,
	    msgs_funcs, subdirs);

    g_array_free(msgs_funcs, 1);
    g_array_free(folder_post_funcs, 1);
    g_array_free(folder_pre_funcs, 1);
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
    if (flags & MF_NEW) params->mdf->stats->new++;
}

/** Parse flags of a message. */
static int message_parse_flags(const char *name)
{
    int ret = 0;

    const char *flags = strstr(name, ":2,");
    if (!flags)
	return MF_NEW;
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

    /* A message is considered new, if it's not seen nor trashed. */
    if (! (ret & (MF_SEEN | MF_TRASHED)))
	ret |= MF_NEW;

    return ret;
}

/** Fill the message structure with the needed info.
 *
 * \return  0 - ok.
 *         -1 - message ceased to exist.
 */
static int message_open(struct message *msg)
{
    FILE *m = fopen(msg->path, "r");
    if (m == NULL)
	return -1;

    /* Parse flags */
    msg->flags = message_parse_flags(msg->name);

    /* Parse message id, references and in-reply-tos. */
    read_rfc822_header(m, msg);

    fclose(m);

    return 0;
}

/** struct message destructor. */
static void message_free(struct message *msg)
{
    if (msg->path)
	g_free(msg->path);
    if (msg->msg_id)
	g_free(msg->msg_id);
    if (msg->references) {
	g_ptr_array_foreach(msg->references, (GFunc) g_free, 0);
	g_ptr_array_free(msg->references, 1);
    }
}

/** Destruct <code>struct message</code> and free the memory occupied by the
 * struct itself.
 */
static void message_free_and_free(struct message *msg)
{
    message_free(msg);
    g_slice_free(struct message, msg);
}

/** Prepare folder for message indexing:
 * Save the current #messages map to #old_messages,
 * alloc new #messages.
 */
static void maildir_folder_messages_prepare(struct maildir_folder *mdf)
{
    mdf->old_messages = mdf->messages;
    mdf->messages = g_tree_new_full((GCompareDataFunc) strcmp, 0,
	    NULL, /* key is a part of the value */
	    (GDestroyNotify) message_free_and_free);
}

/** Clean up #old_messages. */
static void maildir_folder_messages_post(struct maildir_folder *mdf)
{
    if (mdf->old_messages) {
	g_tree_destroy(mdf->old_messages);
	mdf->old_messages = NULL;
    }
}

/** Message indexing walker. */
static void maildir_folder_messages_msg(
	struct maildir_folder_walk_messages_params *params)
{
    char *key;
    struct message *value;

    if (params->mdf->old_messages &&
	    g_tree_lookup_extended(params->mdf->old_messages,
		params->msg_name, (void **) &key, (void **) &value) == TRUE) {
	/* The message had been already indexed, and (hopefully) has not
	 * changed since. */
	g_tree_steal(params->mdf->old_messages, key);
	g_tree_insert(params->mdf->messages, key, value);
    } else {
	/* New message, index it. */
	value = g_slice_new0(struct message);
	value->path = g_strdup(params->msg_full_path);
	value->name = value->path +
	    strlen(params->msg_full_path) - strlen(params->msg_name);
	key = value->name;

	if (message_open(value) == -1)
	    message_free_and_free(value);
	else
	    g_tree_insert(params->mdf->messages, key, value);
    }
}
