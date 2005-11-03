/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License, Version 1.0 only
 * (the "License").  You may not use this file except in compliance
 * with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */
/*
 * Copyright 2005 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 *
 * write binary audit records directly to a file.
 */
#pragma ident	"%Z%%M%	%I%	%E% SMI"

#define	DEBUG   0

#if DEBUG
#define	DPRINT(x) {fprintf x; }
#else
#define	DPRINT(x)
#endif

/*
 * auditd_plugin_open(), auditd_plugin() and auditd_plugin_close()
 * implement a replacable library for use by auditd; they are a
 * project private interface and may change without notice.
 *
 */

#include <assert.h>
#include <bsm/audit.h>
#include <bsm/audit_record.h>
#include <bsm/libbsm.h>
#include <errno.h>
#include <fcntl.h>
#include <libintl.h>
#include <netdb.h>
#include <pthread.h>
#include <secdb.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <sys/types.h>
#include <time.h>
#include <tzfile.h>
#include <unistd.h>
#include <sys/vfs.h>
#include <security/auditd.h>
#include <audit_plugin.h>

#define	AUDIT_DATE_SZ	14
#define	AUDIT_FNAME_SZ	2 * AUDIT_DATE_SZ + 2 + MAXHOSTNAMELEN
#define	AUDIT_BAK_SZ	50	/* size of name of audit_data back-up file */

			/* per-directory status */
#define	SOFT_SPACE	0	/* minfree or less space available	*/
#define	PLENTY_SPACE	1	/* more than minfree available		*/
#define	SPACE_FULL	2	/* out of space				*/
#define	STAY_FULL	3	/* unusable file system			*/

#define	AVAIL_MIN	50	/* If there are less that this number	*/
				/* of blocks avail, the filesystem is	*/
				/* presumed full.			*/


/*
 * The directory list is a circular linked list.  It is pointed into by
 * activeDir.  Each element contains the pointer to the next
 * element, the directory pathname, a flag for how much space there is
 * in the directory's filesystem, and a file handle.  Since a new
 * directory list can be created from auditd_plugin_open() while the
 * current list is in use, activeDir is protected by log_mutex.
 */
typedef struct dirlist_s dirlist_t;
struct dirlist_s {
	dirlist_t	*dl_next;
	int		dl_space;
	int		dl_flags;
	char		*dl_dirname;
	char		*dl_filename;	/* file name (not path) if open */
	int		dl_fd;		/* file handle, -1 unless open */
};
/*
 * Defines for dl_flags
 */
#define	SOFT_WARNED	0x0001	/* already did soft warning for this dir */
#define	HARD_WARNED	0x0002	/* already did hard warning for this dir */

#if DEBUG
static FILE		*dbfp;			/* debug file */
#endif

static pthread_mutex_t	log_mutex;
static int		binfile_is_open = 0;

static int		minfree = -1;
static int		minfreeblocks;		/* minfree in blocks */

static dirlist_t	*activeDir = NULL;	/* current directory */
static int		activeCount = 0;	/* number of dirs in the ring */

static int		openNewFile = 1;	/* need to open a new file */
static int		hung_count = 0;		/* count of audit_warn hard */

/* flag from audit_plugin_open to audit_plugin_close */
static int		am_open = 0;
/* preferred dir state */
static int		fullness_state = PLENTY_SPACE;

static int open_log(dirlist_t *);

static void
freedirlist(dirlist_t *head)
{
	dirlist_t	 *n1, *n2;
	/*
	 * Free up the old directory list if any
	 */
	if (head != NULL) {
		n1 = head;
		do {
			n2 = n1->dl_next;
			free(n1->dl_dirname);
			free(n1->dl_filename);
			free(n1);
			n1 = n2;
		} while (n1 != head);
	}
}


/*
 * add to a linked list of directories available for writing
 *
 */

static int
growauditlist(dirlist_t **listhead, char *dirlist,
    dirlist_t *endnode, int *count)
{
	dirlist_t	*node;
	char		*bs, *be;
	dirlist_t	**node_p;
	char		*dirname;
	char		*remainder;

	DPRINT((dbfp, "binfile: dirlist=%s\n", dirlist));

	if (*listhead == NULL)
		node_p = listhead;
	else
		node_p = &(endnode->dl_next);

	node = NULL;
	while ((dirname = strtok_r(dirlist, ",", &remainder)) != NULL) {
		dirlist = NULL;

		DPRINT((dbfp, "binfile: p_dir = %s\n", dirname));

		(*count)++;
		node = malloc(sizeof (dirlist_t));
		if (node == NULL)
			return (AUDITD_NO_MEMORY);

		node->dl_flags = 0;
		node->dl_filename = NULL;
		node->dl_fd = -1;
		node->dl_space = PLENTY_SPACE;

		node->dl_dirname = malloc((unsigned)strlen(dirname) + 1);
		if (node->dl_dirname == NULL)
			return (AUDITD_NO_MEMORY);

		bs = dirname;
		while ((*bs == ' ') || (*bs == '\t'))	/* trim blanks */
			bs++;
		be = bs + strlen(bs) - 1;
		while (be > bs) {	/* trim trailing blanks */
			if ((*bs != ' ') && (*bs != '\t'))
				break;
			be--;
		}
		*(be + 1) = '\0';
		(void) strlcpy(node->dl_dirname, bs, AUDIT_FNAME_SZ);

		if (*listhead != NULL)
			node->dl_next = *listhead;
		else
			node->dl_next = node;
		*node_p = node;
		node_p = &(node->dl_next);

	}
	return (0);
}

/*
 * create a linked list of directories available for writing
 *
 * if a list already exists, the two are compared and the new one is
 * used only if it is different than the old.
 *
 * returns -2 for new or changed list, 0 for unchanged list and -1 for
 * error.  (Positive returns are for AUDITD_<error code> values)
 *
 */

static int
loadauditlist(char *dirstr, char *minfreestr)
{
	char		buf[MAXPATHLEN];
	char		*bs, *be;
	dirlist_t	 *node, *n1, *n2;
	dirlist_t	 **node_p;
	dirlist_t	*listhead = NULL;
	dirlist_t	*thisdir;
	int		acresult;
	int		node_count = 0;
	int		rc;
	int		temp_minfree;
	au_acinfo_t	*ach;

	static dirlist_t	*activeList = NULL;	/* directory list */

	DPRINT((dbfp, "binfile: Loading audit list from auditcontrol\n"));

	/*
	 * Build new directory list
	 */
	/* part 1 -- using pre Sol 10 audit_control directives */
	node_p = &listhead;

	ach = _openac(NULL);
	if (ach == NULL)
		return (-1);

	/* at least one directory is needed */
	while ((acresult = _getacdir(ach, buf, sizeof (buf))) == 0 ||
		acresult == 2 || acresult == -3) {
		/*
		 * loop if the result is 0 (success), 2 (a warning
		 * that the audit_data file has been rewound),
		 * or -3 (a directory entry was found, but it
		 * was badly formatted.
		 */
		if (acresult == 0) {
			/*
			 * A directory entry was found.
			 */
			node_count++;
			node = malloc(sizeof (dirlist_t));
			if (node == NULL)
				return (AUDITD_NO_MEMORY);

			node->dl_flags = 0;
			node->dl_fd = -1;
			node->dl_space = PLENTY_SPACE;
			node->dl_filename = NULL;

			node->dl_dirname = malloc((unsigned)strlen(buf) + 1);
			if (node->dl_dirname == NULL)
				return (AUDITD_NO_MEMORY);

			bs = buf;
			while ((*bs == ' ') || (*bs == '\t'))
				bs++;
			be = bs + strlen(bs) - 1;
			while (be > bs) {	/* trim trailing blanks */
				if ((*bs != ' ') && (*bs != '\t'))
					break;
				be--;
			}
			*(be + 1) = '\0';
			(void) strlcpy(node->dl_dirname, bs, AUDIT_FNAME_SZ);

			if (listhead != NULL)
				node->dl_next = listhead;
			else
				node->dl_next = node;
			*node_p = node;
			node_p = &(node->dl_next);
		}
	}   /* end of getacdir while */
	/*
	 * part 2 -- use directories and minfree from the (new as of Sol 10)
	 * plugin directive
	 */
	if (dirstr != NULL) {
		if (node_count == 0) {
			listhead = NULL;
			node = NULL;
		}
		rc = growauditlist(&listhead, dirstr, node, &node_count);
		if (rc)
			return (rc);
	}
	if (node_count == 0) {
		/*
		 * there was a problem getting the directory
		 * list or remote host info from the audit_control file
		 * even though auditd thought there was at least 1 good
		 * entry
		 */
		DPRINT((dbfp, "binfile: "
		    "problem getting directory / libpath list "
		    "from audit_control.\n"));

		_endac(ach);
		return (-1);
	}
#if DEBUG
	/* print out directory list */

	if (listhead != NULL) {
		fprintf(dbfp, "Directory list:\n\t%s\n", listhead->dl_dirname);
		thisdir = listhead->dl_next;

		while (thisdir != listhead) {
			fprintf(dbfp, "\t%s\n", thisdir->dl_dirname);
			thisdir = thisdir->dl_next;
		}
	}
#endif	/* DEBUG */
	thisdir = listhead;
	/*
	 * See if the list has changed.
	 * If there was a change  rc = 0 if no change, else 1
	 */
	rc = 0;	/* no change */

	if (node_count == activeCount) {
		n1 = listhead;
		n2 = activeList;
		do {
			if (strcmp(n1->dl_dirname, n2->dl_dirname) != 0) {
				DPRINT((dbfp,
				    "binfile: new dirname = %s\n"
				    "binfile: old dirname = %s\n",
				    n1->dl_dirname,
				    n2->dl_dirname));
				rc = -2;
				break;
			}
			n1 = n1->dl_next;
			n2 = n2->dl_next;
		} while ((n1 != listhead) && (n2 != activeList));
	} else {
		DPRINT((dbfp, "binfile:  old dir count = %d\n"
		    "binfile:  new dir count = %d\n",
		    activeCount, node_count));
		rc = -2;
	}
	if (rc == -2) {
		(void) pthread_mutex_lock(&log_mutex);
		DPRINT((dbfp, "loadauditlist:  close / open log\n"));
		if (open_log(listhead) == 0)
			openNewFile = 1;	/* try again later */
		freedirlist(activeList);	/* old list */
		activeList = listhead;		/* new list */
		activeDir = thisdir;
		activeCount = node_count;
		(void) pthread_mutex_unlock(&log_mutex);
	} else
		freedirlist(listhead);
	/*
	 * Get the minfree value.  If minfree comes in via the attribute
	 * list, ignore the possibility it may also be listed on a separate
	 * audit_control line.
	 */
	if (minfreestr != NULL)
		temp_minfree = atoi(minfreestr);
	else if (!(_getacmin(ach, &temp_minfree) == 0))
		temp_minfree = 0;

	if ((temp_minfree < 0) || (temp_minfree > 100))
		temp_minfree = 0;

	if (minfree != temp_minfree) {
		DPRINT((dbfp, "minfree:  old = %d, new = %d\n",
		    minfree, temp_minfree));
		rc = -2;		/* data change */
		minfree = temp_minfree;
	}
	_endac(ach);

	return (rc);
}


/*
 * getauditdate - get the current time (GMT) and put it in the form
 *		  yyyymmddHHMMSS .
 */
static void
getauditdate(char *date)
{
	struct timeval tp;
	struct timezone tzp;
	struct tm tm;

	(void) gettimeofday(&tp, &tzp);
	tm = *gmtime(&tp.tv_sec);
	/*
	 * NOTE:  if we want to use gmtime, we have to be aware that the
	 *	structure only keeps the year as an offset from TM_YEAR_BASE.
	 *	I have used TM_YEAR_BASE in this code so that if they change
	 *	this base from 1900 to 2000, it will hopefully mean that this
	 *	code does not have to change.  TM_YEAR_BASE is defined in
	 *	tzfile.h .
	 */
	(void) sprintf(date, "%.4d%.2d%.2d%.2d%.2d%.2d",
		tm.tm_year + TM_YEAR_BASE, tm.tm_mon + 1, tm.tm_mday,
		tm.tm_hour, tm.tm_min, tm.tm_sec);
}



/*
 * write_file_token - put the file token into the audit log
 */
static int
write_file_token(int fd, char *name)
{
	adr_t adr;					/* xdr ptr */
	struct timeval tv;				/* time now */
	char for_adr[AUDIT_FNAME_SZ + AUDIT_FNAME_SZ];	/* plenty of room */
	char	token_id;
	short	i;

	(void) gettimeofday(&tv, (struct timezone *)0);
	i = strlen(name) + 1;
	adr_start(&adr, for_adr);
#ifdef _LP64
		token_id = AUT_OTHER_FILE64;
		adr_char(&adr, &token_id, 1);
		adr_int64(&adr, (int64_t *)& tv, 2);
#else
		token_id = AUT_OTHER_FILE32;
		adr_char(&adr, &token_id, 1);
		adr_int32(&adr, (int32_t *)& tv, 2);
#endif

	adr_short(&adr, &i, 1);
	adr_char(&adr, name, i);

	if (write(fd, for_adr, adr_count(&adr)) < 0) {
		DPRINT((dbfp, "binfile: Bad write\n"));
		return (errno);
	}
	return (0);
}

/*
 * close_log - close the file if open.  Also put the name of the
 *	new log file in the trailer, and rename the old file
 *	to oldname.  The caller must hold log_mutext while calling
 *      close_log since any change to activeDir is a complete redo
 *	of all it points to.
 * arguments -
 *	oldname - the new name for the file to be closed
 *	newname - the name of the new log file (for the trailer)
 */
static void
close_log(dirlist_t *currentdir, char *oname, char *newname)
{
	char	auditdate[AUDIT_DATE_SZ+1];
	char	*name;
	char	oldname[AUDIT_FNAME_SZ+1];

	if ((currentdir == NULL) || (currentdir->dl_fd == -1))
		return;
	/*
	 * If oldname is blank, we were called by auditd_plugin_close()
	 * instead of by open_log, so we need to update our name.
	 */
	(void) strlcpy(oldname, oname, AUDIT_FNAME_SZ);

	if (strcmp(oldname, "") == 0) {
		getauditdate(auditdate);

		assert(currentdir->dl_filename != NULL);

		(void) strlcpy(oldname, currentdir->dl_filename,
		    AUDIT_FNAME_SZ);

		name = strrchr(oldname, '/') + 1;
		(void) memcpy(name + AUDIT_DATE_SZ + 1, auditdate,
		    AUDIT_DATE_SZ);
	}
	/*
	 * Write the trailer record and rename and close the file.
	 * If any of the write, rename, or close fail, ignore it
	 * since there is not much else we can do and the next open()
	 * will trigger the necessary full directory logic.
	 *
	 * newname is "" if binfile is being closed down.
	 */
	(void) write_file_token(currentdir->dl_fd, newname);
	if (currentdir->dl_fd >= 0)
		(void) close(currentdir->dl_fd);
	currentdir->dl_fd = -1;
	(void) rename(currentdir->dl_filename, oldname);

	DPRINT((dbfp, "binfile: Log closed %s\n", oldname));

	free(currentdir->dl_filename);
	currentdir->dl_filename = NULL;
}


/*
 * open_log - open a new file in the current directory.  If a
 *	file is already open, close it.
 *
 *	return 1 if ok, 0 if all directories are full.
 *
 *	lastOpenDir - used to get the oldfile name (and change it),
 *		to close the oldfile.
 *
 * The caller must hold log_mutex while calling open_log.
 *
 */
static int
open_log(dirlist_t *current_dir)
{
	char	auditdate[AUDIT_DATE_SZ + 1];
	char	oldname[AUDIT_FNAME_SZ + 1] = "";
	char	newname[AUDIT_FNAME_SZ + 1];
	char	*name;			/* pointer into oldname */
	int	opened;
	int	error = 0;
	int	newfd = 0;

	static char		host[MAXHOSTNAMELEN + 1] = "";
	/* previous directory with open log file */
	static dirlist_t	*lastOpenDir = NULL;

	if (host[0] == '\0')
		(void) gethostname(host, MAXHOSTNAMELEN);

	/* Get a filename which does not already exist */
	opened = 0;
	while (!opened) {
		getauditdate(auditdate);
		(void) snprintf(newname, AUDIT_FNAME_SZ,
		    "%s/%s.not_terminated.%s",
		    current_dir->dl_dirname, auditdate, host);
		newfd = open(newname,
		    O_RDWR | O_APPEND | O_CREAT | O_EXCL, 0640);
		if (newfd < 0) {
			switch (errno) {
			case EEXIST:
				DPRINT((dbfp,
				    "open_log says duplicate for %s "
				    "(will try another)\n", newname));
				(void) sleep(1);
				break;
			default:
				/* open failed */
				DPRINT((dbfp,
				    "open_log says full for %s: %s\n",
				    newname, strerror(errno)));
				current_dir->dl_space = SPACE_FULL;
				current_dir = current_dir->dl_next;
				return (0);
			} /* switch */
		} else
			opened = 1;
	} /* while */

	/*
	 * When we get here, we have opened our new log file.
	 * Now we need to update the name of the old file to
	 * store in this file's header.  lastOpenDir may point
	 * to current_dir if the list is only one entry long and
	 * there is only one list.
	 */
	if ((lastOpenDir != NULL) && (lastOpenDir->dl_filename != NULL)) {
		(void) strlcpy(oldname, lastOpenDir->dl_filename,
		    AUDIT_FNAME_SZ);
		name = (char *)strrchr(oldname, '/') + 1;

		(void) memcpy(name + AUDIT_DATE_SZ + 1, auditdate,
			AUDIT_DATE_SZ);

		close_log(lastOpenDir, oldname, newname);
	}
	error = write_file_token(newfd, oldname);
	if (error) {
		/* write token failed */
		(void) close(newfd);

		current_dir->dl_space = SPACE_FULL;
		current_dir->dl_fd = -1;
		free(current_dir->dl_filename);
		current_dir->dl_filename = NULL;
		current_dir = current_dir->dl_next;
		return (0);
	} else {
		lastOpenDir = current_dir;
		current_dir->dl_fd = newfd;
		current_dir->dl_filename = strdup(newname);

		__logpost(newname);

		DPRINT((dbfp, "binfile: Log opened: %s\n", newname));
		return (1);
	}
}

#define	IGNORE_SIZE	8192
/*
 * spacecheck - determine whether the given directory's filesystem
 *	has the at least the space requested.  Also set the space
 *	value in the directory list structure.  If the caller
 *	passes other than PLENTY_SPACE or SOFT_SPACE, the caller should
 *	ignore the return value.  Otherwise, 0 = less than the
 *	requested space is available, 1 = at least the requested space
 *	is available.
 *
 *	log_mutex must be held by the caller
 *
 *	-1 is returned if stat fails
 *
 * IGNORE_SIZE is one page (Sol 9 / 10 timeframe) and is the default
 * buffer size written for Sol 9 and earlier.  To keep the same accuracy
 * for the soft limit check as before, spacecheck checks for space
 * remaining IGNORE_SIZE bytes.  This reduces the number of statvfs()
 * calls and related math.
 *
 * globals -
 *	minfree - the soft limit, i.e., the % of filesystem to reserve
 */
static int
spacecheck(dirlist_t *thisdir, int test_limit, size_t next_buf_size)
{
	struct statvfs	sb;
	static int	ignore_size = 0;

	ignore_size += next_buf_size;

	if ((test_limit == PLENTY_SPACE) && (ignore_size < IGNORE_SIZE))
		return (1);

	assert(thisdir != NULL);

	if (thisdir->dl_space == STAY_FULL) {
		thisdir->dl_space = SPACE_FULL;
		minfreeblocks = AVAIL_MIN;
	} else if (statvfs(thisdir->dl_dirname, &sb) < 0) {
		thisdir->dl_space = SPACE_FULL;
		minfreeblocks = AVAIL_MIN;
		return (-1);
	} else {
		minfreeblocks = ((minfree * sb.f_blocks) / 100) + AVAIL_MIN;

		if (sb.f_bavail < AVAIL_MIN)
			thisdir->dl_space = SPACE_FULL;
		else if (sb.f_bavail > minfreeblocks)
			thisdir->dl_space = PLENTY_SPACE;
		else
			thisdir->dl_space = SOFT_SPACE;
	}
	if (thisdir->dl_space == PLENTY_SPACE)
		return (1);

	return (thisdir->dl_space == test_limit);
}

/*
 * auditd_plugin() writes a buffer to the currently open file The
 * global "openNewFile" is used to force a new log file for the
 * initial open; for "audit -s" with changed audit_control data or
 * "audit -n" the new log file is opened immediately.
 *
 * This function manages one or more audit directories as follows:
 *
 * 	If the current open file is in a directory that has not
 *	reached the soft limit, write the input data and return.
 *
 *	Scan the list of directories for one which has not reached
 *	the soft limit; if one is found, write and return.  Such
 *	a writable directory is in "PLENTY_SPACE" state.
 *
 *	Scan the list of directories for one which has not reached
 *	the hard limit; if one is found, write and return.  This
 *	directory in in "SOFT_SPACE" state.
 *
 * Oh, and if a write fails, handle it like a hard space limit.
 *
 * audit_warn (via __audit_dowarn()) is used to alert an operator
 * at various levels of fullness.
 */
/* ARGSUSED */
auditd_rc_t
auditd_plugin(const char *input, size_t in_len, uint32_t sequence, char **error)
{
	auditd_rc_t	rc = AUDITD_FAIL;
	dirlist_t	*startdir;
	int		open_status;
	size_t		out_len;
	/* LINTED */
	int		statrc;
	/* avoid excess audit_warnage */
	static int	somesoftfull_warning = 0;
	static int	allsoftfull_warning = 0;
#if DEBUG
	static char	*last_file_written_to = NULL;
	static uint32_t	last_sequence = 0;
	static uint32_t	write_count = 0;

	if ((last_sequence > 0) && (sequence != last_sequence + 1))
		fprintf(dbfp, "binfile: buffer sequence=%d but prev=%d=n",
				sequence, last_sequence);
	last_sequence = sequence;

	fprintf(dbfp, "binfile: input seq=%d, len=%d\n",
		sequence, in_len);
#endif
	*error = NULL;
	/*
	 * lock is for activeDir, referenced by open_log() and close_log()
	 */
	(void) pthread_mutex_lock(&log_mutex);
	startdir = activeDir;
	while (rc == AUDITD_FAIL) {
		open_status = 1;
		if (openNewFile) {
			open_status = open_log(activeDir);
			if (open_status == 1)	/* ok */
				openNewFile = 0;
		}
		/*
		 * consider "space ok" return and error return the same;
		 * a -1 means spacecheck couldn't check for space.
		 */
		if ((open_status == 1) &&
		    (statrc = spacecheck(activeDir, fullness_state,
		    in_len)) != 0) {
#if DEBUG
			DPRINT((dbfp, "binfile: returned from spacecheck\n"));
			/*
			 * The last copy of last_file_written_to is
			 * never free'd, so there will be one open
			 * memory reference on exit.  It's debug only.
			 */
			if ((last_file_written_to != NULL) &&
			    (strcmp(last_file_written_to,
			    activeDir->dl_filename) != 0)) {
				DPRINT((dbfp, "binfile:  now writing to %s\n",
				    activeDir->dl_filename));
				free(last_file_written_to);
			}
			DPRINT((dbfp, "binfile:  finished some debug stuff\n"));
			last_file_written_to =
			    strdup(activeDir->dl_filename);
#endif
			out_len = write(activeDir->dl_fd, input, in_len);
			DPRINT((dbfp, "binfile:  finished the write\n"));

			if (out_len == in_len) {
				DPRINT((dbfp,
				    "binfile: write_count=%u, sequence=%u,"
				    " l=%u\n",
				    ++write_count, sequence, out_len));
				allsoftfull_warning = 0;
				if (fullness_state == PLENTY_SPACE)
					somesoftfull_warning = 0;

				rc = AUDITD_SUCCESS;
				break;
			} else if (!activeDir->dl_flags & HARD_WARNED) {
				DPRINT((dbfp,
				    "binfile: write failed, sequence=%u, "
				    "l=%u\n", sequence, out_len));
				DPRINT((dbfp, "hard warning sent.\n"));
				__audit_dowarn("hard", activeDir->dl_dirname,
				    0);

				activeDir->dl_flags |= HARD_WARNED;
			}
		} else {
			DPRINT((dbfp, "binfile: statrc=%d, fullness_state=%d\n",
			    statrc, fullness_state));
			somesoftfull_warning++;
			if ((somesoftfull_warning <= activeCount) &&
			    !(activeDir->dl_flags & SOFT_WARNED)) {
				DPRINT((dbfp, "soft warning sent\n"));
				__audit_dowarn("soft",
				    activeDir->dl_dirname, 0);
				activeDir->dl_flags |= SOFT_WARNED;
			}
			if (!activeDir->dl_flags & HARD_WARNED) {
				DPRINT((dbfp, "hard warning sent.\n"));
				__audit_dowarn("hard",
				activeDir->dl_dirname, 0);
				activeDir->dl_flags |= HARD_WARNED;
			}
		}
		DPRINT((dbfp, "binfile: activeDir=%s, next=%s\n",
		    activeDir->dl_dirname, activeDir->dl_next->dl_dirname));

		activeDir = activeDir->dl_next;

		if (activeDir == startdir) {		/* full circle */
			if (fullness_state == PLENTY_SPACE) {	/* once */
				fullness_state = SOFT_SPACE;
				if (allsoftfull_warning == 0) {
					allsoftfull_warning++;
					__audit_dowarn("allsoft", "", 0);
				}
			} else {			/* full circle twice */
				__audit_dowarn("allhard", "", ++hung_count);
				minfreeblocks = AVAIL_MIN;
				rc = AUDITD_RETRY;
				*error = strdup(gettext(
				    "all partitions full\n"));
				__logpost("");
			}
		}
	}
	(void) pthread_mutex_unlock(&log_mutex);

	return (rc);
}


/*
 * the open function uses getacdir() and getacmin to determine which
 * directories to use and when to switch.  It takes no inputs.
 *
 * It may be called multiple times as auditd handles SIGHUP and SIGUSR1
 * corresponding to the audit(1M) flags -s and -n
 *
 * kvlist is NULL only if auditd caught a SIGUSR1, so after the first
 * time open is called, the reason is -s if kvlist != NULL and -n
 * otherwise.
 *
 */

auditd_rc_t
auditd_plugin_open(const kva_t *kvlist, char **ret_list, char **error)
{
	int		rc = 0;
	int		status;
	int		reason;
	char		*dirlist;
	char		*minfree;
	kva_t		*kv;

	*error = NULL;
	*ret_list = NULL;
	kv = (kva_t *)kvlist;

	if (am_open) {
		if (kvlist == NULL)
			reason = 1;	/* audit -n */
		else
			reason = 2;	/* audit -s */
	} else {
		reason = 0;		/* initial open */
#if DEBUG
		dbfp = __auditd_debug_file_open();
#endif
	}
	DPRINT((dbfp, "binfile: am_open=%d, reason=%d\n", am_open, reason));

	am_open = 1;

	if (kvlist == NULL) {
		dirlist = NULL;
		minfree = NULL;
	} else {
		dirlist = kva_match(kv, "p_dir");
		minfree = kva_match(kv, "p_minfree");
	}
	switch (reason) {
	case 0:			/* initial open */
		if (!binfile_is_open)
			(void) pthread_mutex_init(&log_mutex, NULL);
		binfile_is_open = 1;
		openNewFile = 1;
		/* FALLTHRU */
	case 2:			/* audit -s */
		fullness_state = PLENTY_SPACE;
		status = loadauditlist(dirlist, minfree);

		if (status == -1) {
			__logpost("");
			*error = strdup(gettext("no directories configured"));
			return (AUDITD_RETRY);
		} else if (status == AUDITD_NO_MEMORY) {
			__logpost("");
			*error = strdup(gettext("no memory"));
			return (status);
		} else {	/* status is 0 or -2 (no change or changed) */
			hung_count = 0;
			DPRINT((dbfp, "binfile: loadauditlist returned %d\n",
				status));
		}
		break;
	case 1:			/* audit -n */
		(void) pthread_mutex_lock(&log_mutex);
		if (open_log(activeDir) == 1)	/* ok */
			openNewFile = 0;
		(void) pthread_mutex_unlock(&log_mutex);
		break;
	}

	rc = AUDITD_SUCCESS;
	*ret_list = NULL;

	return (rc);
}

auditd_rc_t
auditd_plugin_close(char **error)
{
	*error = NULL;

	(void) pthread_mutex_lock(&log_mutex);
	close_log(activeDir, "", "");
	freedirlist(activeDir);
	activeDir = NULL;
	(void) pthread_mutex_unlock(&log_mutex);

	DPRINT((dbfp, "binfile:  closed\n"));

	if (binfile_is_open) {
		(void) pthread_mutex_destroy(&log_mutex);
		binfile_is_open = 0;
		/* LINTED */
	} else {
		DPRINT((dbfp,
		    "auditd_plugin_close() called when already closed."));
	}
	am_open = 0;
	return (AUDITD_SUCCESS);
}
