/* This file is a part of the maildirtools package. See the COPYRIGHT file for
 * details. */

#ifndef MAILDIR_H
#define MAILDIR_H

#include <dirent.h>
#include <glib.h>
#include <linux/limits.h>
#include <sys/select.h>
#include <sys/types.h>

struct maildirpp {
    char path[PATH_MAX];
    DIR *dir;
    GPtrArray *subfolders; ///< List of struct maildir_folder.
    GPtrArray *subdirs; ///< List of DIR. (watching wannabe folders)
};

struct maildir_folder_stats;

struct maildir_folder {
    struct maildirpp *md;

    char path[PATH_MAX];
    DIR *dir_new, *dir_cur;

    /* Non-mandatory fields: */
    struct maildir_folder_stats *stats;
};

struct maildir_folder_stats {
    int msgs, passed, replied, seen, trashed, draft, flagged, new;
};

enum message_flags {
    MF_PASSED	= 1 << 0,
    MF_REPLIED	= 1 << 1,
    MF_SEEN	= 1 << 2,
    MF_TRASHED	= 1 << 3,
    MF_DRAFT	= 1 << 4,
    MF_FLAGGED	= 1 << 5
};

enum maildir_folder_data {
    MFD_STATS	= 1 << 0
};

enum fill_subdirs {
    SD_NEW	= 1 << 0,
    SD_CUR	= 1 << 1
};

/** Params for walker functions. */
struct maildir_folder_walk_messages_params {
    struct maildir_folder *mdf;
    const char *msg_name, *msg_full_path;
};

typedef void (*maildir_folder_walk_messages_func)
    (struct maildir_folder_walk_messages_params *params);
typedef void (*maildir_folder_walk_func)
    (struct maildir_folder *mdf);

int maildirpp_open(struct maildirpp *md, const char *path);
void maildirpp_close(struct maildirpp *md);
int maildirpp_dirty(struct maildirpp *md, int dont_block);
int maildirpp_dirty_subfolders(struct maildirpp *md, int dont_block);
void maildirpp_pause_if_not_dirty(struct maildirpp *md);
int maildirpp_refresh_subfolders_list(struct maildirpp *md);
void maildirpp_set_verbose(int new_verbose);
void maildirpp_folders_walk(struct maildirpp *md, GArray *folder_funcs,
	GArray *msgs_funcs, int subdirs);
void maildirpp_folders_fill(struct maildirpp *md, int data, int subdirs);

#endif /* MAILDIR_H */
