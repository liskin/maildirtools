#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>
#include <ncurses.h>
#include "maildir.h"

/* Configuration vars. */
static int dont_cur = 0;
static int watch = 0;
static int (*print)(const char *, ...) = printf;

static volatile int signalled = 0;
static int total = 0;

/* Print the number of new messages in the folder. */
void mailbox(struct maildir_folder *mdf)
{
    int new = mdf->stats->new;
    if (new) {
	print("Mas %4i %s v %s\n", new,
		(new == 1 ? "   novy mail" :
		 (new < 5 ? "  nove maily" :
		  "novych mailu")),
		mdf->path);
	total += new;
    }
}

void sighandler(int sig)
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
	    case 'n':
		dont_cur = 1;
		break;

	    case 'w':
		watch = 1;
		break;

	    case 'h':
		fprintf(stderr, "Usage: %s [options] [<maildir location>]\n",
			argv[0]);
		fprintf(stderr, " -h - this message\n");
		fprintf(stderr, " -n - walk only \"new\" subdir\n");
		fprintf(stderr, " -w - keep monitoring the maildir for "
			"changes\n");
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

    if (maildirpp_open(&md, maildir) != 0)
	abort();

    /* Init curses/signal, if watch. */
    if (watch) {
	initscr();
	cbreak(); noecho(); keypad(stdscr,1); nodelay(stdscr,1); nl();
	clear(); curs_set(0);
	signal(SIGINT, sighandler);
	signal(SIGTERM, sighandler);
	print = printw;
    }

    do {
	if (watch) {
	    erase();
	    move(0, 0);

	    time_t t = time(0);
	    print("\tLast update: %s\n", ctime(&t));
	}

	/* If the list of subfolders changes, refresh it. */
	if (maildirpp_dirty(&md))
	    maildirpp_refresh_subfolders_list(&md);

	/* Print counts of new messages. */
	/* This reloads only changed folders: */
	maildirpp_folders_fill(&md, MFD_STATS,
		SD_NEW | (dont_cur ? 0 : SD_CUR));
	total = 0;
	g_ptr_array_foreach(md.subfolders, (GFunc) mailbox, 0);
	if (total) {
	    print(" --\n");
	    print("Mas celkem %i %s.\n", total,
		    (total == 1 ? "   novy mail" :
		     (total < 5 ? "  nove maily" :
		      "novych mailu")));
	}

	if (watch) {
	    refresh();
	    /* A signal may have come between the refresh and the sleep call.
	     * That's why it's sleep and not pause. */
	    sleep(60);
	}
    } while (watch && !signalled);

    if (watch) {
	endwin();
    }

    maildirpp_close(&md);

    g_free(maildir);

    return 0;
}
