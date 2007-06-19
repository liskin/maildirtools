#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "maildir.h"

static int total = 0;

void mailbox(struct maildir_folder *mdf)
{
    int unseen = mdf->stats->msgs - mdf->stats->seen;
    if (unseen) {
	printf("Mas %4i %s v %s\n", unseen,
		(unseen == 1 ? "   novy mail" :
		 (unseen < 5 ? "  nove maily" :
		               "novych mailu")),
		mdf->path);
	total += unseen;
    }
}

int main(int argc, char *argv[])
{
    char *maildir;

    if (argc == 2)
	maildir = g_strdup(argv[1]);
    else {
	char *home = getenv("HOME");
	if (!home) abort();
	maildir = g_strconcat(home, "/Mail", NULL);
    }

    struct maildirpp md;

    if (maildirpp_open(&md, maildir) != 0)
	abort();

    maildirpp_folders_fill(&md, MFD_STATS);
    g_ptr_array_foreach(md.subfolders, (GFunc) mailbox, 0);
    if (total) {
	puts(" --");
	printf("Mas celkem %i %s.\n", total,
		(total == 1 ? "   novy mail" :
		 (total < 5 ? "  nove maily" :
		              "novych mailu")));
    }

    maildirpp_close(&md);

    g_free(maildir);

    return 0;
}
