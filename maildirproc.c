/* This file is a part of the maildirtools package. See the COPYRIGHT file for
 * details. */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>
#include "maildir.h"

static volatile int signalled = 0;

static gboolean msg(char *key, struct message *value)
{
    printf("  %s: %s\n", key, value->msg_id ? value->msg_id : "<unknown>");
    for (int i = 0; i < value->references->len; i++)
	printf("    %s\n",
		(char *) g_ptr_array_index(value->references, i));

    return FALSE;
}

static void mailbox(struct maildir_folder *mdf)
{
    printf("%s:\n", mdf->path);
    g_tree_foreach(mdf->messages, (GTraverseFunc) msg, 0);
}

static void sighandler(int sig)
{
    signalled = 1;
}

int main(int argc, char *argv[])
{
    char *maildir;

    /* Parse cmdline options */
    while (1) {
	char c;

	if ((c = getopt(argc, argv, "nhw")) == -1)
	    break;

	switch (c) {
	    case 'h':
		fprintf(stderr, "Usage: %s [options] [<maildir location>]\n",
			argv[0]);
		fprintf(stderr, " -h - this message\n");
		return 0;

	    case ':':
	    case '?':
	    default:
		fprintf(stderr, "Use %s -h for help\n", argv[0]);
		return -1;
	}
    }

    /* Maildir location specified? Use the default otherwise. */
    if (optind < argc)
	maildir = g_strdup(argv[optind]);
    else {
	char *home = getenv("HOME");
	if (!home) abort();
	maildir = g_strconcat(home, "/Mail", NULL);
    }

    /* And the fun begins here. */
    struct maildirpp md;

    signal(SIGINT, sighandler);
    signal(SIGTERM, sighandler);
    if (maildirpp_open(&md, maildir) != 0)
	abort();

    do {
	/* If the list of subfolders changes, refresh it. */
	if (maildirpp_dirty(&md, 0))
	    maildirpp_refresh_subfolders_list(&md);

	maildirpp_folders_fill(&md, MFD_MSGS,
		SD_NEW | SD_CUR);

	puts("Dump:");
	g_ptr_array_foreach(md.subfolders, (GFunc) mailbox, 0);
	puts("Dump END.");

	break;

	maildirpp_pause_if_not_dirty(&md);
    } while (!signalled);

    maildirpp_close(&md);

    g_free(maildir);

    return 0;
}
