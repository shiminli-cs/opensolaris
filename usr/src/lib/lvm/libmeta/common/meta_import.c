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
 */

#pragma ident	"%Z%%M%	%I%	%E% SMI"

#include <assert.h>
#include <ctype.h>
#include <libdevinfo.h>
#include <mdiox.h>
#include <meta.h>
#include "meta_repartition.h"
#include "meta_set_prv.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/lvm/md_mddb.h>
#include <sys/lvm/md_names.h>
#include <sys/lvm/md_crc.h>

typedef struct did_list {
	void		*rdid;	/* real did if replicated set */
	void		*did;	/* did stored in lb */
	char		*devname;
	dev_t		dev;
	uint_t		did_index;
	char		*minor_name;
	struct did_list	*next;
} did_list_t;

typedef struct replicated_disk {
	void			*old_devid;
	void 			*new_devid;
	struct replicated_disk	*next;
} replicated_disk_t;

/*
 * The current implementation limits the max device id length to 256 bytes.
 * Should the max device id length be increased, this define would have to
 * be bumped up accordingly
 */
#define	MAX_DEVID_LEN		256

/*
 * We store a global list of all the replicated disks in the system. In
 * order to prevent us from performing a linear search on this list, we
 * store the disks in a two dimensional sparse array. The disks are bucketed
 * based on the length of their device ids.
 */
static replicated_disk_t *replicated_disk_list[MAX_DEVID_LEN + 1] = {NULL};

/*
 * The list of replicated disks is built just once and this flag is set
 * once it's done
 */
static int replicated_disk_list_built = 0;

/*
 * Map logical blk to physical
 *
 * This is based on the routine of the same name in the md kernel module (see
 * file md_mddb.c), with the following caveats:
 *
 * - The kernel routine works on in core master blocks, or mddb_mb_ic_t; this
 * routine works instead on the mddb_mb_t read directly from the disk
 */
static daddr_t
getphysblk(
	mddb_block_t	blk,
	mddb_mb_t	*mbp
)
{
	/*
	 * Sanity check: is the block within range?  If so, we then assume
	 * that the block range map in the master block is valid and
	 * consistent with the block count.  Unfortunately, there is no
	 * reliable way to validate this assumption.
	 */
	if (blk >= mbp->mb_blkcnt || blk >= mbp->mb_blkmap.m_consecutive)
		return ((daddr_t)-1);

	return (mbp->mb_blkmap.m_firstblk + blk);
}



/*
 * drive_append()
 *
 * Append to tail of linked list of md_im_drive_info_t.
 *
 * Will allocate space for new node and copy args into new space.
 *
 * Returns pointer to new node.
 */
static md_im_drive_info_t *
drive_append(
	md_im_drive_info_t	**midpp,
	mddrivename_t		*dnp,
	void			*devid,
	void			*rdevid,
	int			devid_sz,
	char			*minor_name,
	md_timeval32_t		timestamp,
	md_im_replica_info_t	*mirp
)
{
	md_im_drive_info_t	*midp;
	int			o_devid_sz;

	for (; (*midpp != NULL); midpp = &((*midpp)->mid_next))
		;

	midp = *midpp = Zalloc(sizeof (md_im_drive_info_t));

	midp->mid_dnp = dnp;

	/*
	 * If rdevid is not NULL then we know we are dealing with
	 * replicated diskset case. 'devid_sz' will always be the
	 * size of a valid devid which can be 'devid' or 'rdevid'
	 */
	midp->mid_devid = (void *)Malloc(devid_sz);

	if (rdevid) {
		(void) memcpy(midp->mid_devid, rdevid, devid_sz);
		/*
		 * Also need to store the 'other' devid
		 */
		o_devid_sz = devid_sizeof((ddi_devid_t)devid);
		midp->mid_o_devid = (void *)Malloc(o_devid_sz);
		(void) memcpy(midp->mid_o_devid, devid, o_devid_sz);
		midp->mid_o_devid_sz = o_devid_sz;
	} else {
		/*
		 * In the case of regular diskset, midp->mid_o_devid
		 * will be a NULL pointer
		 */
		(void) memcpy(midp->mid_devid, devid, devid_sz);
	}

	midp->mid_devid_sz = devid_sz;
	midp->mid_setcreatetimestamp = timestamp;
	(void) strlcpy(midp->mid_minor_name, minor_name, MDDB_MINOR_NAME_MAX);
	midp->mid_replicas = mirp;

	return (midp);
}



/*
 * drive_append_wrapper()
 *
 * Constant time append wrapper; the append function will always walk the list,
 * this will take a tail argument and use the append function on just the tail
 * node, doing the appropriate old-tail-next-pointer bookkeeping.
 */
static md_im_drive_info_t **
drive_append_wrapper(
	md_im_drive_info_t	**tailpp,
	mddrivename_t		*dnp,
	void 			*devid,
	void			*rdevid,
	int			devid_sz,
	char			*minor_name,
	md_timeval32_t		timestamp,
	md_im_replica_info_t	*mirp
)
{
	(void) drive_append(tailpp, dnp, devid, rdevid, devid_sz, minor_name,
		timestamp, mirp);

	if ((*tailpp)->mid_next == NULL)
		return (tailpp);

	return (&((*tailpp)->mid_next));
}



/*
 * replica_append()
 *
 * Append to tail of linked list of md_im_replica_info_t.
 *
 * Will allocate space for new node and copy args into new space.
 *
 * Returns pointer to new node.
 */
static md_im_replica_info_t *
replica_append(
	md_im_replica_info_t	**mirpp,
	int			flags,
	daddr32_t		offset,
	daddr32_t		length,
	md_timeval32_t		timestamp
)
{
	md_im_replica_info_t	*mirp;

	for (; (*mirpp != NULL); mirpp = &((*mirpp)->mir_next))
		;

	mirp = *mirpp = Zalloc(sizeof (md_im_replica_info_t));

	mirp->mir_flags = flags;
	mirp->mir_offset = offset;
	mirp->mir_length = length;
	mirp->mir_timestamp = timestamp;

	return (mirp);

}



/*
 * replica_append_wrapper()
 *
 * Constant time append wrapper; the append function will always walk the list,
 * this will take a tail argument and use the append function on just the tail
 * node, doing the appropriate old-tail-next-pointer bookkeeping.
 */
static md_im_replica_info_t **
replica_append_wrapper(
	md_im_replica_info_t	**tailpp,
	int			flags,
	daddr32_t		offset,
	daddr32_t		length,
	md_timeval32_t		timestamp
)
{
	(void) replica_append(tailpp, flags, offset, length, timestamp);

	if ((*tailpp)->mir_next == NULL)
		return (tailpp);

	return (&(*tailpp)->mir_next);
}

/*
 * map_replica_disk()
 *
 * Searches the device id list for a specific
 * disk based on the locator block device id array index.
 *
 * Returns a pointer to the did_list node if a match was
 * found or NULL otherwise.
 */
static did_list_t *
map_replica_disk(
	did_list_t	*did_listp,
	int		did_index
)
{
	did_list_t	*tailp = did_listp;

	while (tailp != NULL) {
		if (tailp->did_index == did_index)
			return (tailp);
		tailp = tailp->next;
	}

	/* not found, return failure */
	return (NULL);
}

/*
 * replicated_list_lookup()
 *
 * looks up a replicated disk entry in the global replicated disk list
 * based upon the length of that disk's device id. returns the new device id
 * for the disk.
 * If you store the returned devid you must create a local copy.
 */
static void *
replicated_list_lookup(
	uint_t	devid_len,
	void	*old_devid
)
{
	replicated_disk_t *head = NULL;

	assert(devid_len <= MAX_DEVID_LEN);
	head = replicated_disk_list[devid_len];

	if (head == NULL)
		return (NULL);

	do {
		if (devid_compare((ddi_devid_t)old_devid,
			(ddi_devid_t)head->old_devid) == 0)
			return (head->new_devid);
		head = head->next;
	} while (head != NULL);

	return (NULL);
}

/*
 * replicated_list_insert()
 *
 * inserts a replicated disk entry into the global replicated disk list
 */
static void
replicated_list_insert(
	size_t	old_devid_len,
	void	*old_devid,
	void	*new_devid
)
{
	replicated_disk_t	*repl_disk, **first_entry;
	void			*repl_old_devid = NULL;

	assert(old_devid_len <= MAX_DEVID_LEN);

	repl_disk = Zalloc(sizeof (replicated_disk_t));
	repl_old_devid = Zalloc(old_devid_len);
	(void) memcpy(repl_old_devid, (void *)old_devid, old_devid_len);

	repl_disk->old_devid = repl_old_devid;
	repl_disk->new_devid = new_devid;

	first_entry = &replicated_disk_list[old_devid_len];

	if (*first_entry == NULL) {
		*first_entry = repl_disk;
		return;
	}

	repl_disk->next = *first_entry;
	replicated_disk_list[old_devid_len] = repl_disk;
}

/*
 * get_replica_disks()
 *
 * Will step through the locator records in the supplied locator block, and add
 * each one with an active replica to a supplied list of md_im_drive_info_t, and
 * add the appropriate replicas to the md_im_replica_info_t contained therein.
 */
static void
get_replica_disks(
	md_im_set_desc_t	*misp,
	did_list_t		*did_listp,
	mddb_mb_t		*mb,
	mddb_lb_t		*lbp,
	md_error_t		*ep,
	int			replicated
)
{
	mddrivename_t		*dnp;
	int			indx, on_list;
	mdsetname_t		*sp = metasetname(MD_LOCAL_NAME, ep);
	int			flags;
	int			devid_sz;
	char			*minor_name;
	did_list_t		*replica_disk;
	daddr32_t		offset;
	daddr32_t		length;
	md_timeval32_t		timestamp;
	md_im_replica_info_t	**mirpp = NULL;
	md_im_drive_info_t	**midpp = &misp->mis_drives;
	md_im_drive_info_t	*midp;
	void			*did;

	for (indx = 0; indx < lbp->lb_loccnt; indx++) {

		on_list = 0;
		if (lbp->lb_locators[indx].l_flags & MDDB_F_ACTIVE) {

			/*
			 * search the device id list for a
			 * specific ctds based on the locator
			 * block device id array index.
			 */
			replica_disk = map_replica_disk(did_listp, indx);

			assert(replica_disk != NULL);


			/*
			 * metadrivename() can fail for a slice name
			 * if there is not an existing mddrivename_t.
			 * So we use metadiskname() to strip the slice
			 * number.
			 */
			dnp = metadrivename(&sp,
			    metadiskname(replica_disk->devname), ep);

			for (midp = misp->mis_drives; midp != NULL;
				midp = midp->mid_next) {
				if (dnp == midp->mid_dnp) {
					on_list = 1;
					mirpp = &midp->mid_replicas;
					break;
				}
			}

			/*
			 * Get the correct devid_sz
			 */
			if (replicated)
				did = replica_disk->rdid;
			else
				did = replica_disk->did;

			devid_sz = devid_sizeof((ddi_devid_t)did);
			minor_name = replica_disk->minor_name;

			/*
			 * New on the list so add it
			 */
			if (!on_list) {
				mddb_mb_t	*mbp;
				uint_t		sliceno;
				mdname_t	*rsp;
				int		fd = -1;

				mbp = Malloc(DEV_BSIZE);

				/* determine the replica slice */
				if (meta_replicaslice(dnp, &sliceno,
				    ep) != 0) {
					Free(mbp);
					continue;
				}

				/*
				 * if the replica slice size is zero,
				 * don't bother opening
				 */
				if (dnp->vtoc.parts[sliceno].size == 0) {
					Free(mbp);
					continue;
				}

				if ((rsp = metaslicename(dnp, sliceno,
				    ep)) == NULL) {
					Free(mbp);
					continue;
				}

				if ((fd = open(rsp->rname,
				    O_RDONLY| O_NDELAY)) < 0) {
					Free(mbp);
					continue;
				}

				/*
				 * a drive may not have a master block
				 */
				if (read_master_block(ep, fd, mbp,
				    DEV_BSIZE) <= 0) {
					mdclrerror(ep);
					Free(mbp);
					(void) close(fd);
					continue;
				}

				(void) close(fd);
				midpp = drive_append_wrapper(midpp, dnp,
				    replica_disk->did, replica_disk->rdid,
				    devid_sz, minor_name, mbp->mb_setcreatetime,
				    NULL);
				mirpp = &((*midpp)->mid_replicas);
				Free(mbp);
			}

			/*
			 * For either of these assertions to fail, it implies
			 * a NULL return from metadrivename() above.  Since
			 * the args came from a presumed valid locator block,
			 * that's Bad.
			 */
			assert(midpp != NULL);
			assert(mirpp != NULL);

			/*
			 * Extract the parameters describing this replica.
			 *
			 * The magic "1" in the length calculation accounts
			 * for the length of the master block, in addition to
			 * the block count it describes.  (The master block
			 * will always take up one block on the disk, and
			 * there will always only be one master block per
			 * replica, even though much of the code is structured
			 * to handle noncontiguous replicas.)
			 */
			flags = lbp->lb_locators[indx].l_flags;
			offset = lbp->lb_locators[indx].l_blkno;
			length = mb->mb_blkcnt + 1;
			timestamp = mb->mb_setcreatetime;

			mirpp = replica_append_wrapper(mirpp, flags,
				offset, length, timestamp);

			/*
			 * If we're here it means -
			 *
			 * a) we had an active copy of the replica, and
			 * b) we've added the disk to the list of
			 *    disks as well.
			 *
			 * We need to bump up the number of active
			 * replica count for each such replica so that it
			 * can be used later for replica quorum check.
			 */
			misp->mis_active_replicas++;
		}
	}
}



/*
 * get_nonreplica_disks()
 *
 * Extracts the disks without replicas from the locator name space and adds them
 * to the supplied list of md_im_drive_info_t.
 */
static void
get_nonreplica_disks(
	md_im_set_desc_t	*misp,
	mddb_rb_t		*did_nm,
	mddb_rb_t		*did_shrnm,
	md_error_t		*ep,
	int			replicated
)
{
	char			*search_path = "/dev";
	devid_nmlist_t		*nmlist;
	md_im_drive_info_t	*midp, **midpp = &misp->mis_drives;
	mddrivename_t		*dnp;
	mdsetname_t		*sp = metasetname(MD_LOCAL_NAME, ep);
	mddb_rb_t		*rbp_did = did_nm;
	mddb_rb_t		*rbp_did_shr = did_shrnm;
	int			on_list = 0;
	int			devid_sz;
	struct devid_min_rec	*did_rec;
	struct devid_shr_rec	*did_shr_rec;
	struct did_shr_name	*did;
	struct did_min_name	*min;
	void			*r_did;	/* NULL if not a replicated diskset */
	void			*valid_did;

	/*
	 * We got a pointer to an mddb record, which we expect to contain a
	 * name record; extract the pointer thereto.
	 */
	/* LINTED */
	did_rec = (struct devid_min_rec *)((caddr_t)(&rbp_did->rb_data));
	/* LINTED */
	did_shr_rec = (struct devid_shr_rec *)
	    ((caddr_t)(&rbp_did_shr->rb_data));

	/*
	 * Skip the nm_rec_hdr and iterate on the array of struct minor_name
	 * at the end of the devid_min_rec
	 */
	for (min = &did_rec->minor_name[0]; min->min_devid_key != 0;
	    /* LINTED */
	    min = (struct did_min_name *)((char *)min + DID_NAMSIZ(min))) {

		on_list = 0;
		r_did = NULL;

		/*
		 * For a give DID_NM key, locate the corresponding device
		 * id from DID_NM_SHR
		 */
		for (did = &did_shr_rec->device_id[0]; did->did_key != 0;
		    /* LINTED */
		    did = (struct did_shr_name *)
		    ((char *)did + DID_SHR_NAMSIZ(did))) {
			/*
			 * We got a match, this is the device id we're
			 * looking for
			 */
			if (min->min_devid_key == did->did_key)
				break;
		}

		if (did->did_key == 0) {
			/* we didn't find a match */
			assert(did->did_key != 0);
			md_exit(NULL, 1);
		}

		/*
		 * If replicated diskset
		 */
		if (replicated) {
			size_t		new_devid_len;
			char		*temp;
			/*
			 * In this case, did->did_devid will
			 * be invalid so lookup the real one
			 */
			temp = replicated_list_lookup(did->did_size,
			    did->did_devid);
			new_devid_len = devid_sizeof((ddi_devid_t)temp);
			r_did = Zalloc(new_devid_len);
			(void) memcpy(r_did, temp, new_devid_len);
			valid_did = r_did;
		} else {
			valid_did = did->did_devid;
		}

		/* Get the ctds mapping for that device id */
		if (meta_deviceid_to_nmlist(search_path,
		    (ddi_devid_t)valid_did,
		    &min->min_name[0], &nmlist) == 0) {

			assert(nmlist->devname != NULL);
			/* Don't bother with metadevices, but track disks */
			if (!is_metaname(nmlist->devname)) {
				dnp = metadrivename(&sp,
				    metadiskname(nmlist->devname), ep);

				assert(dnp != NULL);
				/* Is it already on the list? */
				for (midp = misp->mis_drives; midp != NULL;
				    midp = midp->mid_next) {
					if (midp->mid_dnp == dnp) {
						on_list = 1;
						break;
					}
				}

				devid_sz = devid_sizeof(
				    (ddi_devid_t)valid_did);

				if (!on_list) {
					mddb_mb_t	*mbp;
					uint_t		sliceno;
					mdname_t	*rsp;
					int		fd = -1;

					mbp = Malloc(DEV_BSIZE);

					/* determine the replica slice */
					if (meta_replicaslice(dnp, &sliceno,
					    ep) != 0) {
						Free(mbp);
						continue;
					}

					/*
					 * if the replica slice size is zero,
					 * don't bother opening
					 */
					if (dnp->vtoc.parts[sliceno].size
					    == 0) {
						Free(mbp);
						continue;
					}

					if ((rsp = metaslicename(dnp, sliceno,
					    ep)) == NULL) {
						Free(mbp);
						continue;
					}

					if ((fd = open(rsp->rname,
					    O_RDONLY| O_NDELAY)) < 0) {
						Free(mbp);
						continue;
					}

					/*
					 * a drive may not have a master block
					 */
					if (read_master_block(ep, fd, mbp,
					    DEV_BSIZE) <= 0) {
						mdclrerror(ep);
						Free(mbp);
						(void) close(fd);
						continue;
					}

					(void) close(fd);
					/*
					 * If it is replicated diskset,
					 * r_did will be non-NULL and
					 * devid_sz will be its size
					 */
					midpp = drive_append_wrapper(midpp,
					    dnp, &did->did_devid, r_did,
					    devid_sz, &min->min_name[0],
					    mbp->mb_setcreatetime, NULL);
					Free(mbp);
				}
			}
			devid_free_nmlist(nmlist);
		}
	}
}

/*
 * set_append()
 *
 * Append to tail of linked list of md_im_set_desc_t.
 *
 * Will allocate space for new node AND populate it by extracting disks with
 * and without replicas from the locator blocks and locator namespace.
 *
 * Returns pointer to new node.
 */
static md_im_set_desc_t *
set_append(
	md_im_set_desc_t	**mispp,
	did_list_t		*did_listp,
	mddb_mb_t		*mb,
	mddb_lb_t		*lbp,
	mddb_rb_t		*nm,
	mddb_rb_t		*did_nm,
	mddb_rb_t		*did_shrnm,
	md_error_t		*ep,
	int			replicated
)
{
	md_im_set_desc_t	*misp;
	set_t			setno = mb->mb_setno;

	/* run to end of list */
	for (; (*mispp != NULL); mispp = &((*mispp)->mis_next))
		;

	/* allocate new list element */
	misp = *mispp = Zalloc(sizeof (md_im_set_desc_t));

	if (replicated)
		misp->mis_flags = MD_IM_SET_REPLICATED;

	misp->mis_oldsetno = setno;

	/* Get the disks with and without replicas */
	get_replica_disks(misp, did_listp, mb, lbp, ep, replicated);

	if (nm != NULL && did_nm != NULL && did_shrnm != NULL) {
		get_nonreplica_disks(misp, did_nm, did_shrnm, ep, replicated);
	}

	/*
	 * An error in this struct could come from either of the above routines;
	 * in both cases, we want to pass it back on up.
	 */
	return (misp);
}



/*
 * set_append_wrapper()
 *
 * Constant time append wrapper; the append function will always walk the list,
 * this will take a tail argument and use the append function on just the tail
 * node, doing the appropriate old-tail-next-pointer bookkeeping.
 */
static md_im_set_desc_t **
set_append_wrapper(
	md_im_set_desc_t	**tailpp,
	did_list_t		*did_listp,
	mddb_mb_t		*mb,
	mddb_lb_t		*lbp,
	mddb_rb_t		*nm,
	mddb_rb_t		*did_nm,
	mddb_rb_t		*did_shrnm,
	md_error_t		*ep,
	int			replicated
)
{
	(void) set_append(tailpp, did_listp, mb, lbp, nm, did_nm,
	    did_shrnm, ep, replicated);

	/* it's the first item in the list, return it instead of the next */
	return (((*tailpp)->mis_next == NULL) ? tailpp : &(*tailpp)->mis_next);
}



/*
 * add_disk_names()
 *
 * Iterator to walk the minor node tree of the device snapshot, adding only the
 * first non-block instance of each non-cdrom minor node to a list of disks.
 */
static int
add_disk_names(di_node_t node, di_minor_t minor, void *args)
{
	char			*search_path = "/dev";
	ddi_devid_t		devid = di_devid(node);
	devid_nmlist_t		*nm;
	char			*min = di_minor_name(minor);
	md_im_names_t		*cnames = (md_im_names_t *)args;
	static di_node_t	save_node = NULL;

	/*
	 * skip CD devices
	 * If a device does not have a device id, we can't
	 * do anything with it so just exclude it from our
	 * list.
	 *
	 * This would also encompass CD devices and floppy
	 * devices that don't have a device id.
	 */
	if (devid == NULL) {
		return (DI_WALK_CONTINUE);
	}

	/* char disk devices (as opposed to block) */
	if (di_minor_spectype(minor) == S_IFCHR) {

		/* only first occurrence (slice 0) of each instance */
		if (save_node == NULL || node != save_node) {
			save_node = node;
			if (meta_deviceid_to_nmlist(search_path, devid,
			    min, &nm) == 0) {
				int	index = cnames->min_count++;

				assert(nm->devname != NULL);
				cnames->min_names =
					Realloc(cnames->min_names,
						cnames->min_count *
						sizeof (char *));

				assert(cnames->min_names != NULL);
				cnames->min_names[index] =
					metadiskname(nm->devname);
				devid_free_nmlist(nm);
			}
		}
	}
	return (DI_WALK_CONTINUE);
}



/*
 * meta_list_disks()
 *
 * Snapshots the device tree and extracts disk devices from the snapshot.
 */
int
meta_list_disks(md_error_t *ep, md_im_names_t *cnames)
{
	di_node_t root_node;

	assert(cnames != NULL);
	cnames->min_count = 0;
	cnames->min_names = NULL;

	if ((root_node = di_init("/", DINFOCPYALL|DINFOFORCE))
	    == DI_NODE_NIL) {
		return (mdsyserror(ep, errno, NULL));
	}

	(void) di_walk_minor(root_node, DDI_NT_BLOCK, 0, cnames,
	    add_disk_names);

	di_fini(root_node);
	return (0);
}

/*
 * meta_imp_drvused
 *
 * Checks if given drive is mounted, swapped, part of disk configuration
 * or in use by SVM.  ep also has error code set up if drive is in use.
 *
 * Returns 1 if drive is in use.
 * Returns 0 if drive is not in use.
 */
int
meta_imp_drvused(
	mdsetname_t		*sp,
	mddrivename_t		*dnp,
	md_error_t		*ep
)
{
	md_error_t		status = mdnullerror;
	md_error_t		*db_ep = &status;

	/*
	 * We pass in db_ep to meta_setup_db_locations
	 * and never ever use the error contained therein
	 * because all we're interested in is a check to
	 * see whether any local metadbs are present.
	 */
	if ((meta_check_drivemounted(sp, dnp, ep) != 0) ||
	    (meta_check_driveswapped(sp, dnp, ep) != 0) ||
	    (((meta_setup_db_locations(db_ep) == 0) &&
	    ((meta_check_drive_inuse(sp, dnp, 1, ep) != 0) ||
	    (meta_check_driveinset(sp, dnp, ep) != 0))))) {
		return (1);
	} else {
		return (0);
	}
}

/*
 * meta_prune_cnames()
 *
 * Removes in-use disks from the list prior to further processing.
 *
 * Return value depends on err_on_prune flag: if set, and one or more disks
 * are pruned, the return list will be the pruned disks.  If not set, or if no
 * disks are pruned, the return list will be the unpruned disks.
 */
mddrivenamelist_t *
meta_prune_cnames(
	md_error_t *ep,
	md_im_names_t *cnames,
	int err_on_prune
)
{
	int			d;
	int			fcount = 0;
	mddrivenamelist_t	*dnlp = NULL;
	mddrivenamelist_t	**dnlpp = &dnlp;
	mddrivenamelist_t	*fdnlp = NULL;
	mddrivenamelist_t	**fdnlpp = &fdnlp;
	mdsetname_t		*sp = metasetname(MD_LOCAL_NAME, ep);

	for (d = 0; d < cnames->min_count; ++d) {
		mddrivename_t	*dnp;

		dnp = metadrivename(&sp, cnames->min_names[d], ep);
		if (dnp == NULL) {
			/*
			 * Assuming we're interested in knowing about
			 * whatever error occurred, but not in stopping.
			 */
			mde_perror(ep, cnames->min_names[d]);
			mdclrerror(ep);

			continue;
		}

		/*
		 * Check if the drive is inuse.
		 */
		if (meta_imp_drvused(sp, dnp, ep)) {
			fdnlpp = meta_drivenamelist_append_wrapper(fdnlpp, dnp);
			fcount++;
			mdclrerror(ep);
		} else {
			dnlpp = meta_drivenamelist_append_wrapper(dnlpp, dnp);
		}
	}

	if (fcount) {
		if (err_on_prune) {
			(void) mddserror(ep, MDE_DS_DRIVEINUSE, 0,
			    NULL, fdnlp->drivenamep->cname, NULL);
			metafreedrivenamelist(dnlp);
			return (fdnlp);
		}
		metafreedrivenamelist(fdnlp);
	}

	return (dnlp);
}

/*
 * read_master_block()
 *
 * Returns:
 *	< 0 for failure
 *	  0 for no valid master block
 *	  1 for valid master block
 *
 * The supplied buffer will be filled in for EITHER 0 or 1.
 */
int
read_master_block(
	md_error_t	*ep,
	int		fd,
	void		*bp,
	int		bsize
)
{
	mddb_mb_t	*mbp = bp;
	int		rval = 1;

	assert(bp != NULL);

	if (lseek(fd, (off_t)dbtob(16), SEEK_SET) < 0)
		return (mdsyserror(ep, errno, NULL));

	if (read(fd, bp, bsize) != bsize)
		return (mdsyserror(ep, errno, NULL));

	/*
	 * The master block magic number can either be MDDB_MAGIC_MB in
	 * the case of a real master block, or, it can be MDDB_MAGIC_DU
	 * in the case of a dummy master block
	 */
	if ((mbp->mb_magic != MDDB_MAGIC_MB) &&
	    (mbp->mb_magic != MDDB_MAGIC_DU)) {
		rval = 0;
		(void) mdmddberror(ep, MDE_DB_MASTER, 0, 0, 0, NULL);
	}

	if (mbp->mb_revision != MDDB_REV_MB) {
		rval = 0;
	}

	return (rval);
}

/*
 * read_locator_block()
 *
 * Returns:
 *	< 0 for failure
 *	  0 for no valid locator block
 *	  1 for valid locator block
 */
int
read_locator_block(
	md_error_t	*ep,
	int		fd,
	mddb_mb_t	*mbp,
	void		*bp,
	int		bsize
)
{
	mddb_lb_t	*lbp = bp;

	assert(bp != NULL);

	if (lseek(fd, (off_t)dbtob(mbp->mb_blkmap.m_firstblk), SEEK_SET) < 0)
		return (mdsyserror(ep, errno, NULL));

	if (read(fd, bp, bsize) != bsize)
		return (mdsyserror(ep, errno, NULL));

	return ((lbp->lb_magic == MDDB_MAGIC_LB) ? 1 : 0);
}

int
phys_read(
	md_error_t	*ep,
	int		fd,
	mddb_mb_t	*mbp,
	daddr_t		blk,
	void		*bp,
	int		bcount
)
{
	daddr_t		pblk;

	if ((pblk = getphysblk(blk, mbp)) < 0)
		return (mdmddberror(ep, MDE_DB_BLKRANGE, NODEV32,
			MD_LOCAL_SET, blk, NULL));

	if (lseek(fd, (off_t)dbtob(pblk), SEEK_SET) < 0)
		return (mdsyserror(ep, errno, NULL));

	if (read(fd, bp, bcount) != bcount)
		return (mdsyserror(ep, errno, NULL));

	return (bcount);
}

/*
 * read_locator_block_did()
 *
 * Returns:
 * 	< 0 for failure
 *	  0 for no valid locator name struct
 *	  1 for valid locator name struct
 */
int
read_locator_block_did(
	md_error_t	*ep,
	int		fd,
	mddb_mb_t	*mbp,
	mddb_lb_t	*lbp,
	void		*bp,
	int		bsize
)
{
	int		lb_didfirstblk = lbp->lb_didfirstblk;
	mddb_did_blk_t	*lbdidp = bp;
	int		rval;

	assert(bp != NULL);

	if ((rval = phys_read(ep, fd, mbp, lb_didfirstblk, bp, bsize)) < 0)
		return (rval);

	return ((lbdidp->blk_magic == MDDB_MAGIC_DI) ? 1 : 0);
}

/*
 * read_locator_names()
 *
 * Returns:
 *	< 0 for failure
 *	  0 for no valid locator name struct
 *	  1 for valid locator name struct
 */
int
read_locator_names(
	md_error_t	*ep,
	int		fd,
	mddb_mb_t	*mbp,
	mddb_lb_t	*lbp,
	void		*bp,
	int		bsize
)
{
	int		lnfirstblk = lbp->lb_lnfirstblk;
	mddb_ln_t	*lnp = bp;
	int		rval;

	assert(bp != NULL);

	if ((rval = phys_read(ep, fd, mbp, lnfirstblk, bp, bsize)) < 0)
		return (rval);

	return ((lnp->ln_magic == MDDB_MAGIC_LN) ? 1 : 0);
}


int
read_database_block(
	md_error_t	*ep,
	int		fd,
	mddb_mb_t	*mbp,
	int		dbblk,
	void		*bp,
	int		bsize
)
{
	mddb_db_t	*dbp = bp;
	int		rval;

	assert(bp != NULL);

	if ((rval = phys_read(ep, fd, mbp, dbblk, bp, bsize)) < 0)
		return (rval);

	return ((dbp->db_magic == MDDB_MAGIC_DB) ? 1 : 0);
}

int
read_loc_didblks(
	md_error_t	*ep,
	int		fd,
	mddb_mb_t	*mbp,
	int		didblk,
	void		*bp,
	int		bsize
)
{
	mddb_did_blk_t	*didbp = bp;
	int		rval;

	assert(bp != NULL);

	if ((rval = phys_read(ep, fd, mbp, didblk, bp, bsize)) < 0)
		return (rval);

	return ((didbp->blk_magic == MDDB_MAGIC_DI) ? 1 : 0);
}


int
read_loc_didinfo(
	md_error_t	*ep,
	int		fd,
	mddb_mb_t	*mbp,
	int		infoblk,
	void		*bp,
	int		bsize
)
{
	int		rval = 1;
	mddb_did_info_t	*infop = bp;

	assert(bp != NULL);

	if ((rval = phys_read(ep, fd, mbp, infoblk, bp, bsize)) < 0)
		return (rval);

	return ((infop->info_flags & MDDB_DID_EXISTS) ? 1 : 0);
}

/*
 * meta_nm_rec()
 *
 * Return the DE corresponding to the requested namespace record type.
 * Modifies dbp to have a firstentry if one isn't there.
 */
static mddb_de_t *
meta_nm_rec(mddb_db_t *dbp, mddb_type_t rectype)
{
	mddb_de_t *dep;
	int	desize;

	if (dbp->db_firstentry != NULL) {
		/* LINTED */
		dep = (mddb_de_t *)((caddr_t)(&dbp->db_firstentry)
				    + sizeof (dbp->db_firstentry));
		dbp->db_firstentry = dep;
		while (dep && dep->de_next) {
			desize = sizeof (*dep) - sizeof (dep->de_blks) +
				sizeof (daddr_t) * dep->de_blkcount;
			/* LINTED */
			dep->de_next = (mddb_de_t *)
				((caddr_t)dep + desize);
			dep = dep->de_next;
		}
	}

	for (dep = dbp->db_firstentry; dep != NULL; dep = dep->de_next) {
		if (dep->de_type1 == rectype)
			break;
	}
	return (dep);
}

/*
 * read_nm_rec()
 *
 * Reads the NM, NM_DID or NM_DID_SHR record in the mddb and stores the
 * configuration data in the buffer 'nm'
 *
 * Returns:
 *	< 0 for failure
 *	  0 for no valid NM/DID_NM/DID_NM_SHR record
 *	  1 for valid NM/DID_NM/DID_NM_SHR record
 *
 */
static int
read_nm_rec(
	md_error_t 	*ep,
	int 		fd,
	mddb_mb_t	*mbp,
	mddb_lb_t	*lbp,
	char		**nm,
	mddb_type_t	rectype,
	char		*diskname
)
{
	int		cnt, dbblk, rval = 0;
	char		db[DEV_BSIZE];
	mddb_de_t	*dep;
	/*LINTED*/
	mddb_db_t	*dbp = (mddb_db_t *)&db;
	char 		*tmpnm = NULL;
	daddr_t		pblk;

	for (dbblk = lbp->lb_dbfirstblk;
	    dbblk != 0;
	    dbblk = dbp->db_nextblk) {

		if ((rval = read_database_block(ep, fd, mbp, dbblk, dbp,
		    sizeof (db))) <= 0)
			return (rval);

		/*
		 * Locate NM/DID_NM/DID_NM_SHR record. Normally there is
		 * only one record per mddb. There is a rare case when we
		 * can't expand the record. If this is the case then we
		 * will have multiple NM/DID_NM/DID_NM_SHR records linked
		 * with r_next_recid.
		 *
		 * For now assume the normal case and handle the extended
		 * namespace in Phase 2.
		 */
		if ((dep = meta_nm_rec(dbp, rectype)) != NULL)
			break;
	}

	/* If meta_nm_rec() never succeeded, bail out */
	if (dep == NULL)
		return (0);

	/* Read in the appropriate record and return configurations */
	tmpnm = (char *)Zalloc(dbtob(dep->de_blkcount));
	*nm = tmpnm;

	for (cnt = 0; cnt < dep->de_blkcount; cnt++) {
		if ((pblk = getphysblk(dep->de_blks[cnt], mbp)) < 0) {
			rval = mdmddberror(ep, MDE_DB_BLKRANGE,
			    NODEV32, MD_LOCAL_SET,
			    dep->de_blks[cnt], diskname);
			return (rval);
		}

		if (lseek(fd, (off_t)dbtob(pblk), SEEK_SET) < 0) {
			rval = mdsyserror(ep, errno, diskname);
			return (rval);
		}

		if (read(fd, tmpnm, DEV_BSIZE) != DEV_BSIZE) {
			rval = mdsyserror(ep, errno, diskname);
			return (rval);
		}

		tmpnm += DEV_BSIZE;
	}
	return (1);
}

/*
 * is_replicated
 *
 * Determines whether a disk has been replicated or not. It checks to see
 * if the device id stored in the master block is the same as the device id
 * registered for that disk on the current system. If the two device ids are
 * different, then we know that the disk has been replicated.
 *
 * If need_devid is set and the disk is replicated, fill in the new_devid.
 * Also, if need_devid is set, this routine allocates memory for the device
 * ids; the caller of this routine is responsible for free'ing up the memory.
 *
 * Returns:
 * 	1	if it's a replicated disk
 * 	0 	if it's not a replicated disk
 */
static int
is_replicated(
	int fd,
	mddb_mb_t *mbp,
	int need_devid,
	void **new_devid
)
{
	ddi_devid_t	current_devid;
	int		retval = 0;
	size_t		new_devid_len;

	if (mbp->mb_devid_magic != MDDB_MAGIC_DE)
		return (retval);

	if (devid_get(fd, &current_devid) != 0)
		return (retval);

	if (devid_compare((ddi_devid_t)mbp->mb_devid, current_devid) != 0)
		retval = 1;

	if (retval && need_devid) {
		new_devid_len = devid_sizeof(current_devid);
		*new_devid = Zalloc(new_devid_len);
		(void) memcpy(*new_devid, (void *)current_devid, new_devid_len);
	}

	devid_free(current_devid);
	return (retval);
}

/*
 * free_replicated_disks_list()
 *
 * this frees up all the memory allocated by build_replicated_disks_list
 */
static void
free_replicated_disks_list()
{
	replicated_disk_t 	**repl_disk, *temp;
	int 			index;

	for (index = 0; index <= MAX_DEVID_LEN; index++) {
		repl_disk = &replicated_disk_list[index];

		while (*repl_disk != NULL) {
			temp = *repl_disk;
			*repl_disk = (*repl_disk)->next;

			Free(temp->old_devid);
			Free(temp->new_devid);
			Free(temp);
		}
	}
}

/*
 * build_replicated_disks_list()
 *
 * Builds a list of disks that have been replicated using either a
 * remote replication or a point-in-time replication software. The
 * list is stored as a two dimensional sparse array.
 *
 * Returns
 * 	1	on success
 * 	0 	on failure
 */
static int
build_replicated_disks_list(
	md_error_t *ep,
	mddrivenamelist_t *dnlp
)
{
	uint_t			sliceno;
	int			fd = -1;
	mddrivenamelist_t	*dp;
	mdname_t		*rsp;
	mddb_mb_t		*mbp;

	mbp = Malloc(DEV_BSIZE);

	for (dp = dnlp; dp != NULL; dp = dp->next) {
		mddrivename_t *dnp;
		void *new_devid;

		dnp = dp->drivenamep;
		/* determine the replica slice */
		if (meta_replicaslice(dnp, &sliceno, ep) != 0)
			continue;

		/*
		 * if the replica slice size is zero, don't bother opening
		 */
		if (dnp->vtoc.parts[sliceno].size == 0)
			continue;

		if ((rsp = metaslicename(dnp, sliceno, ep)) == NULL)
			continue;

		if ((fd = open(rsp->rname, O_RDONLY| O_NDELAY)) < 0)
			return (mdsyserror(ep, errno, rsp->rname));

		/* a drive may not have a master block so we just continue */
		if (read_master_block(ep, fd, mbp, DEV_BSIZE) <= 0) {
			(void) close(fd);
			mdclrerror(ep);
			continue;
		}

		if (is_replicated(fd, mbp, 1, &new_devid)) {
			replicated_list_insert(mbp->mb_devid_len,
			    mbp->mb_devid, new_devid);
		}
		(void) close(fd);
	}
	replicated_disk_list_built = 1;

	Free(mbp);
	return (1);
}

/*
 * free_did_list()
 *
 * Frees the did_list allocated as part of build_did_list
 */
static void
free_did_list(
	did_list_t	*did_listp
)
{
	did_list_t	*temp, *head;

	head = did_listp;

	while (head != NULL) {
		temp = head;
		head = head->next;
		if (temp->rdid)
			Free(temp->rdid);
		if (temp->did)
			Free(temp->did);
		if (temp->devname)
			Free(temp->devname);
		if (temp->minor_name)
			Free(temp->minor_name);
		Free(temp);
	}
}

/*
 * build_did_list()
 *
 * Build a list of device ids corresponding to disks in the locator block.
 * Memory is allocated here for the nodes in the did_list. The callers of
 * this routine must also call free_did_list to free up the memory after
 * they're done.
 *
 * Returns:
 *	< 0 		for failure
 *	  0 		for no valid locator block device id array
 *	  1 		for valid locator block device id array
 *	  ENOTSUP	partial diskset, not all disks in a diskset on the
 *			system where import is being executed
 */
static int
build_did_list(
	md_error_t	*ep,
	int		fd,
	mddb_mb_t	*mb,
	mddb_did_blk_t	*lbdidp,
	did_list_t	**did_listp,
	int		replicated
)
{
	char 		*search_path = "/dev";
	char		*minor_name;
	int		rval, cnt;
	devid_nmlist_t	*nm;
	uint_t		did_info_length = 0;
	uint_t		did_info_firstblk = 0;
	did_list_t	*new, *head = NULL;
	char		*bp = NULL, *temp;
	mddb_did_info_t	*did_info = NULL;
	void		*did = NULL;
	size_t		new_devid_len;

	for (cnt = 0; cnt < MDDB_NLB; cnt++) {
		did_info = &lbdidp->blk_info[cnt];

		if (!(did_info->info_flags & MDDB_DID_EXISTS))
			continue;

		new = Zalloc(sizeof (did_list_t));
		new->did = Zalloc(did_info->info_length);

		/*
		 * If we can re-use the buffer already has been
		 * read in then just use it.  Otherwise free
		 * the previous one and alloc a new one
		 */
		if (dbtob(did_info->info_blkcnt) != did_info_length &&
		    did_info->info_firstblk != did_info_firstblk) {

			did_info_length = dbtob(did_info->info_blkcnt);
			did_info_firstblk = did_info->info_firstblk;

			if (bp)
				Free(bp);
			bp = temp = Zalloc(did_info_length);

			if ((rval = phys_read(ep, fd, mb, did_info_firstblk,
			    (void *)bp, did_info_length)) < 0)
				return (rval);
		} else {
			temp = bp;
		}

		temp += did_info->info_offset;
		(void) memcpy(new->did, temp, did_info->info_length);
		new->did_index = cnt;
		minor_name = did_info->info_minor_name;

		/*
		 * If we are not able to find the ctd mapping corresponding
		 * to a given device id, it probably means the device id in
		 * question is not registered with the system.
		 *
		 * Highly likely that the only time this happens, we've hit
		 * a case where not all the disks that are a part of the
		 * diskset were moved before importing the diskset.
		 *
		 * If set is a replicated diskset, then the device id we get
		 * from 'lb' will be the 'other' did and we need to lookup
		 * the real one before we call this routine.
		 */
		if (replicated) {
		    temp = replicated_list_lookup(did_info->info_length,
			new->did);
		    new_devid_len = devid_sizeof((ddi_devid_t)temp);
		    new->rdid = Zalloc(new_devid_len);
		    (void) memcpy(new->rdid, temp, new_devid_len);
		    did = new->rdid;
		} else {
		    did = new->did;
		}

		if (devid_valid((ddi_devid_t)(did)) == 0) {
			return (-1);
		}

		if ((rval = meta_deviceid_to_nmlist(search_path,
		    (ddi_devid_t)did, minor_name, &nm)) != 0) {
			*did_listp = head;
			free_did_list(*did_listp);
			*did_listp = NULL;
			(void) mddserror(ep, MDE_DS_PARTIALSET, MD_SET_BAD,
			    mynode(), NULL, NULL);
			return (ENOTSUP);
		}

		assert(nm->devname != NULL);
		new->devname = Strdup(nm->devname);
		new->dev = nm->dev;
		new->minor_name = Strdup(minor_name);

		devid_free_nmlist(nm);

		new->next = head;
		head = new;
	}

	/* Free the last bp */
	if (bp)
		Free(bp);
	*did_listp = head;
	return (1);
}
/*
 * check_nm_disks
 *	Checks the disks listed in the shared did namespace to see if they
 *	are accessable on the system. If not, return ENOTSUP error to
 *	indicate we have a partial diskset.
 * Returns:
 *	< 0 		for failure
 *	  0		success
 *	  ENOTSUP	partial diskset, not all disks in a diskset on the
 *			system where import is being executed
 */
static int
check_nm_disks(
	md_error_t		*ep,
	struct devid_min_rec	*did_nmp,
	struct devid_shr_rec	*did_shrnmp
)
{
	char 		*search_path = "/dev";
	char		*minor_name = NULL;
	uint_t		used_size, min_used_size;
	ddi_devid_t	did;
	devid_nmlist_t	*nm;
	void		*did_min_namep;
	void		*did_shr_namep;
	size_t		did_nsize, did_shr_nsize;

	used_size = did_shrnmp->did_rec_hdr.r_used_size -
	    sizeof (struct nm_rec_hdr);
	min_used_size = did_nmp->min_rec_hdr.r_used_size -
	    sizeof (struct nm_rec_hdr);
	did_shr_namep = (void *)(&did_shrnmp->device_id[0]);
	while (used_size > (int)sizeof (struct did_shr_name)) {
		did_min_namep = (void *)(&did_nmp->minor_name[0]);
		/* grab device id and minor name from the shared spaces */
		did = (ddi_devid_t)(((struct did_shr_name *)
		    did_shr_namep)->did_devid);
		if (devid_valid(did) == 0) {
			return (-1);
		}

		/*
		 * We need to check that the DID_NM and DID_SHR_NM are in
		 * sync. It is possible that we took a panic between writing
		 * the two areas to disk. This would be cleaned up on the
		 * next snarf but we don't know for sure that snarf has even
		 * happened since we're reading from disk.
		 */
		while (((struct did_shr_name *)did_shr_namep)->did_key !=
		    ((struct did_min_name *)did_min_namep)->min_devid_key) {
			did_nsize = DID_NAMSIZ((struct did_min_name *)
			    did_min_namep);
			did_min_namep = ((void *)((char *)did_min_namep +
			    did_nsize));
			min_used_size -= did_nsize;
			if (min_used_size < (int)sizeof (struct did_min_name))
				continue;
		}
		minor_name = ((struct did_min_name *)did_min_namep)->min_name;

		/*
		 * Try to find disk in the system. If we can't find the
		 * disk, we have a partial diskset.
		 */
		if ((meta_deviceid_to_nmlist(search_path,
		    did, minor_name, &nm)) != 0) {
			(void) mddserror(ep, MDE_DS_PARTIALSET, MD_SET_BAD,
			    mynode(), NULL, NULL);
			return (ENOTSUP);
		}
		devid_free_nmlist(nm);
		used_size -= DID_SHR_NAMSIZ((struct did_shr_name *)
		    did_shr_namep);
		/* increment to next item in the shared spaces */
		did_shr_nsize = DID_SHR_NAMSIZ((struct did_shr_name *)
		    did_shr_namep);
		did_shr_namep = ((void *)((char *)did_shr_namep +
		    did_shr_nsize));
	}
	return (0);
}

/*
 * meta_get_set_info
 *
 * Scans a given drive for set specific information. If the given drive
 * has a shared metadb, scans the shared metadb for information pertaining
 * to the set.
 *
 * Returns:
 * 	<0 	for failure
 *	0	success but no replicas were found
 *	1	success and a replica was found
 *	ENOTSUP for partial disksets detected
 */
int
meta_get_set_info(
	mddrivenamelist_t *dp,
	md_im_set_desc_t **mispp,
	int local_mb_ok,
	md_error_t *ep
)
{
	uint_t			s;
	mdname_t		*rsp;
	int			fd;
	char			mb[DEV_BSIZE];
				/*LINTED*/
	mddb_mb_t		*mbp = (mddb_mb_t *)mb;
	char			lb[dbtob(MDDB_LBCNT)];
				/*LINTED*/
	mddb_lb_t		*lbp = (mddb_lb_t *)lb;
	mddb_did_blk_t		*lbdidp = NULL;
	mddb_ln_t		*lnp = NULL;
	int			lnsize, lbdid_size;
	int			rval = 0;
	char			db[DEV_BSIZE];
				/*LINTED*/
	mddb_db_t		*dbp = (mddb_db_t *)db;
	did_list_t		*did_listp = NULL;
	mddrivenamelist_t	*dnlp;
	mddrivename_t 		*dnp;
	md_im_names_t		cnames = { 0, NULL};
	char			*nm = NULL;
	char			*did_nm = NULL, *did_shrnm = NULL;
	struct nm_rec		*nmp;
	struct devid_shr_rec	*did_shrnmp;
	struct devid_min_rec	*did_nmp;
	int			extended_namespace = 0;
	int			replicated = 0;

	dnp = dp->drivenamep;

	/*
	 * Determine and open the replica slice
	 */
	if (meta_replicaslice(dnp, &s, ep) != 0) {
		return (-1);
	}

	/*
	 * Test for the size of replica slice in question. If
	 * the size is zero, we know that this is not a disk that was
	 * part of a set and it should be silently ignored for import.
	 */
	if (dnp->vtoc.parts[s].size == 0)
		return (0);

	if ((rsp = metaslicename(dnp, s, ep)) == NULL) {
		return (-1);
	}

	if ((fd = open(rsp->rname, O_RDONLY|O_NDELAY)) < 0)
		return (mdsyserror(ep, errno, rsp->cname));

	/*
	 * After the open() succeeds, we should return via the "out"
	 * label to clean up after ourselves.  (Up 'til now, we can
	 * just return directly, because there are no resources to
	 * give back.)
	 */

	if ((rval = read_master_block(ep, fd, mbp, sizeof (mb))) <= 0)
		goto out;

	replicated = is_replicated(fd, mbp, 0, NULL);

	if (!local_mb_ok && mbp->mb_setno == 0) {
		rval = 0;
		goto out;
	}

	if ((rval = read_locator_block(ep, fd, mbp, lbp, sizeof (lb))) <= 0)
		goto out;

	/*
	 * Once the locator block has been read, we need to
	 * check if the locator block commit count is zero.
	 * If it is zero, we know that the replica we're dealing
	 * with is on a disk that was deleted from the disk set;
	 * and, it potentially has stale data. We need to quit
	 * in that case
	 */
	if (lbp->lb_commitcnt == 0) {
		rval = 0;
		goto out;
	}

	/*
	 * Make sure that the disk being imported has device id
	 * namespace present for disksets. If a disk doesn't have
	 * device id namespace, we skip reading the replica on that disk
	 */
	if (!(lbp->lb_flags & MDDB_DEVID_STYLE)) {
		rval = 0;
		goto out;
	}

	/*
	 * Grab the locator block device id array. Allocate memory for the
	 * array first.
	 */
	lbdid_size = dbtob(lbp->lb_didblkcnt);
	lbdidp = Zalloc(lbdid_size);

	if ((rval = read_locator_block_did(ep, fd, mbp, lbp, lbdidp,
	    lbdid_size)) <= 0)
		goto out;

	/*
	 * For a disk that has not been replicated, extract the device ids
	 * stored in the locator block device id array and store them in
	 * a list.
	 *
	 * If the disk has been replicated using replication software such
	 * as HDS Truecopy/ShadowImage or EMC SRDF/BCV, the device ids in
	 * the locator block are invalid and we need to build a list of
	 * replicated disks.
	 */
	if (replicated && !replicated_disk_list_built) {
		/*
		 * if there's a replicated diskset involved, we need to
		 * scan the system one more time and build a list of all
		 * candidate disks that might be part of that replicated set
		 */
		if (meta_list_disks(ep, &cnames) != 0) {
			rval = 0;
			goto out;
		}
		dnlp = meta_prune_cnames(ep, &cnames, 0);
		rval = build_replicated_disks_list(ep, dnlp);
		if (rval == 0)
			goto out;
	}

	rval = build_did_list(ep, fd, mbp, lbdidp, &did_listp, replicated);

	if ((rval <= 0) || (rval == ENOTSUP))
		goto out;

	/*
	 * Until here, we've gotten away with fixed sizes for the
	 * master block and locator block.  The locator names,
	 * however, are sized (and therefore allocated) dynamically
	 * according to information in the locator block.
	 */
	lnsize = dbtob(lbp->lb_lnblkcnt);
	lnp = Zalloc(lnsize);

	if ((rval = read_locator_names(ep, fd, mbp, lbp, lnp, lnsize)) <= 0)
		goto out;

	/*
	 * Read in the NM record
	 * If no NM record was found, it still is a valid configuration
	 * but it also means that we won't find any corresponding DID_NM
	 * or DID_SHR_NM.
	 */
	if ((rval = read_nm_rec(ep, fd, mbp, lbp, &nm, MDDB_NM, rsp->cname))
	    < 0)
		goto out;
	else if (rval == 0)
		goto append;

	/*
	 * At this point, we have read in all of the blocks that form
	 * the nm_rec.  We should at least detect the corner case
	 * mentioned above, in which r_next_recid links to another
	 * nm_rec. Extended namespace handling is left for Phase 2.
	 *
	 * What this should really be is a loop, each iteration of
	 * which reads in a nm_rec and calls the set_append_wrapper().
	 */
	/*LINTED*/
	nmp = (struct nm_rec *)(nm + sizeof (mddb_rb_t));
	if (nmp->r_rec_hdr.r_next_recid != (mddb_recid_t)0) {
		extended_namespace = 1;
		rval = 0;
		goto out;
	}

	if ((rval = read_nm_rec(ep, fd, mbp, lbp, &did_nm,
	    MDDB_DID_NM, rsp->cname)) < 0)
		goto out;
	else if (rval == 0)
		goto append;

	/*LINTED*/
	did_nmp = (struct devid_min_rec *)(did_nm + sizeof (mddb_rb_t) -
	    sizeof (int));
	if (did_nmp->min_rec_hdr.r_next_recid != (mddb_recid_t)0) {
		extended_namespace = 1;
		rval = 0;
		goto out;
	}

	if ((rval = read_nm_rec(ep, fd, mbp, lbp, &did_shrnm,
	    MDDB_DID_SHR_NM, rsp->cname)) < 0)
		goto out;
	else if (rval == 0)
		goto append;

	/*LINTED*/
	did_shrnmp = (struct devid_shr_rec *)(did_shrnm + sizeof (mddb_rb_t) -
	    sizeof (int));
	if (did_shrnmp->did_rec_hdr.r_next_recid != (mddb_recid_t)0) {
		extended_namespace = 1;
		rval = 0;
		goto out;
	}

	/*
	 * We need to check if all of the disks listed in the namespace
	 * are actually available. If they aren't we'll return with
	 * an ENOTSUP error which indicates a partial diskset.
	 */
	rval = check_nm_disks(ep, did_nmp, did_shrnmp);
	if ((rval < 0) || (rval == ENOTSUP))
		goto out;

append:
	/* Finally, we've got what we need to process this replica. */
	mispp = set_append_wrapper(mispp, did_listp, mbp, lbp,
	    /*LINTED*/
	    (mddb_rb_t *)nm, (mddb_rb_t *)did_nm, (mddb_rb_t *)did_shrnm,
	    ep, replicated);

	/* Return the fact that we found at least one set */
	rval = 1;

out:
	if (fd >= 0)
		(void) close(fd);
	if (did_listp != NULL)
		free_did_list(did_listp);
	if (lnp != NULL)
		Free(lnp);
	if (nm != NULL)
		Free(nm);
	if (did_nm != NULL)
		Free(did_nm);
	if (did_shrnm != NULL)
		Free(did_shrnm);

	/*
	 * If we are at the end of the list, we must free up
	 * the replicated list too
	 */
	if (dp->next == NULL)
		free_replicated_disks_list();

	if (extended_namespace)
		return (mddserror(ep, MDE_DS_EXTENDEDNM, MD_SET_BAD,
		    mynode(), NULL, NULL));

	return (rval);
}

/*
 * Return the minor name associated with a given disk slice
 */
static char *
meta_getminor_name(
	char *devname,
	md_error_t *ep
)
{
	int 	fd = -1;
	char 	*minor_name = NULL;
	char	*ret_minor_name = NULL;

	if (devname == NULL)
		return (NULL);

	if ((fd = open(devname, O_RDONLY|O_NDELAY, 0)) < 0) {
		(void) mdsyserror(ep, errno, devname);
		return (NULL);
	}

	if (devid_get_minor_name(fd, &minor_name) == 0) {
		ret_minor_name = Strdup(minor_name);
		devid_str_free(minor_name);
	}

	(void) close(fd);
	return (ret_minor_name);
}

static int
meta_replica_quorum(
	md_im_set_desc_t *misp,
	md_error_t *ep
)
{
	md_im_drive_info_t	*midp;
	mddrivename_t		*dnp;
	md_im_replica_info_t    *midr;
	mdname_t		*np;
	struct stat		st_buf;
	uint_t			rep_slice;
	int			replica_count = 0;

	for (midp = misp->mis_drives; midp != NULL;
		midp = midp->mid_next) {

		dnp = midp->mid_dnp;

		if ((meta_replicaslice(dnp, &rep_slice, ep) != 0) ||
			((np = metaslicename(dnp, rep_slice, ep))
			== NULL)) {
			mdclrerror(ep);
			continue;
		}

		if (stat(np->bname, &st_buf) != 0)
			continue;

		/*
		 * The drive is okay now count its replicas
		 */
		for (midr = midp->mid_replicas; midr != NULL;
			midr = midr->mir_next) {
			replica_count++;
		}
	}

	if (replica_count < (misp->mis_active_replicas + 1)/2)
		return (-1);

	return (0);
}

static set_t
meta_imp_setno(
	md_error_t *ep
)
{
	set_t	max_sets, setno;
	int	bool;

	if ((max_sets = get_max_sets(ep)) == 0) {
		return (MD_SET_BAD);
	}

	/*
	 * This code needs to be expanded when we run in SunCluster
	 * environment SunCluster obtains setno internally
	 */
	for (setno = 1; setno < max_sets; setno++) {
		if (clnt_setnumbusy(mynode(), setno,
			&bool, ep) == -1) {
			setno = MD_SET_BAD;
			break;
		}
		/*
		 * found one available
		 */
		if (bool == FALSE)
			break;
	}

	if (setno == max_sets) {
		setno = MD_SET_BAD;
	}

	return (setno);
}

int
meta_imp_set(
	md_im_set_desc_t *misp,
	char		*setname,
	int		force,
	bool_t		dry_run,
	md_error_t	*ep
)
{
	md_timeval32_t		tp;
	md_im_drive_info_t	*midp;
	uint_t			rep_slice;
	mddrivename_t		*dnp;
	struct mddb_config	c;
	mdname_t		*np;
	md_im_replica_info_t	*mirp;
	char			setnum_link[MAXPATHLEN];
	char			setname_link[MAXPATHLEN];
	char			*minor_name = NULL;

	(void) memset(&c, 0, sizeof (c));
	(void) strlcpy(c.c_setname, setname, sizeof (c.c_setname));
	c.c_sideno = 0;
	c.c_flags = MDDB_C_IMPORT;

	/*
	 * Check to see if the setname that the set is being imported into,
	 * already exists.
	 */
	if (getsetbyname(c.c_setname, ep) != NULL) {
		return (mddserror(ep, MDE_DS_SETNAMEBUSY, MD_SET_BAD,
		    mynode(), NULL, c.c_setname));
	}

	/*
	 * Find the next available set number
	 */
	if ((c.c_setno = meta_imp_setno(ep)) == MD_SET_BAD) {
		return (mddserror(ep, MDE_DS_SETNOTIMP, MD_SET_BAD,
		    mynode(), NULL, c.c_setname));
	}

	if (meta_gettimeofday(&tp) == -1) {
		return (mdsyserror(ep, errno, NULL));
	}
	c.c_timestamp = tp;

	/* Check to see if replica quorum requirement is fulfilled */
	if (!force && meta_replica_quorum(misp, ep) == -1)
		return (mddserror(ep, MDE_DS_INSUFQUORUM, MD_SET_BAD,
		    mynode(), NULL, c.c_setname));

	for (midp = misp->mis_drives; midp != NULL;
		midp = midp->mid_next) {
		mdcinfo_t	*cinfo;

		/*
		 * We pass down the list of the drives in the
		 * set down to the kernel irrespective of
		 * whether the drives have a replica or not.
		 *
		 * The kernel detects which of the drives don't
		 * have a replica and accordingly does the
		 * right thing.
		 */
		dnp = midp->mid_dnp;
		if ((meta_replicaslice(dnp, &rep_slice, ep) != 0) ||
		    ((np = metaslicename(dnp, rep_slice, ep))
		    == NULL)) {
			mdclrerror(ep);
			continue;
		}

		(void) strcpy(c.c_locator.l_devname, np->bname);
		c.c_locator.l_dev = meta_cmpldev(np->dev);
		c.c_locator.l_mnum = meta_getminor(np->dev);
		c.c_locator.l_devid = (uintptr_t)Malloc(midp->mid_devid_sz);
		(void) memcpy((void *)(uintptr_t)c.c_locator.l_devid,
		    midp->mid_devid, midp->mid_devid_sz);
		c.c_locator.l_devid_sz = midp->mid_devid_sz;
		c.c_locator.l_devid_flags =
		    MDDB_DEVID_VALID | MDDB_DEVID_SPACE | MDDB_DEVID_SZ;
		if (midp->mid_o_devid) {
			c.c_locator.l_old_devid =
			    (uint64_t)(uintptr_t)Malloc(midp->mid_o_devid_sz);
			(void) memcpy((void *)(uintptr_t)
			    c.c_locator.l_old_devid,
			    midp->mid_o_devid, midp->mid_o_devid_sz);
			c.c_locator.l_old_devid_sz = midp->mid_o_devid_sz;
		}
		minor_name = meta_getminor_name(np->bname, ep);
		(void) strncpy(c.c_locator.l_minor_name, minor_name,
		    sizeof (c.c_locator.l_minor_name));

		if ((cinfo = metagetcinfo(np, ep)) == NULL) {
			mdclrerror(ep);
			continue;
		}
		(void) strncpy(c.c_locator.l_driver, cinfo->dname,
		    sizeof (c.c_locator.l_driver));

		mirp = midp->mid_replicas;

		do {
			if (mirp) {
				c.c_locator.l_flags = 0;
				c.c_locator.l_blkno = mirp->mir_offset;
				mirp = mirp->mir_next;
			} else {
				/*
				 * Default offset for dummy is 16
				 */
				c.c_locator.l_blkno = 16;
			}

			if (metaioctl(MD_DB_USEDEV, &c, &c.c_mde, NULL) != 0) {
				Free((void *)(uintptr_t)c.c_locator.l_devid);
				if (c.c_locator.l_old_devid)
					Free((void *)(uintptr_t)
					    c.c_locator.l_old_devid);
				return (mdstealerror(ep, &c.c_mde));
			}
		} while (mirp != NULL);
	}

	/*
	 * If the dry run option was specified, flag success
	 * and exit out
	 */
	if (dry_run == 1) {
		md_eprintf("%s\n", dgettext(TEXT_DOMAIN,
		    "import should be successful"));
		Free((void *)(uintptr_t)c.c_locator.l_devid);
		if (c.c_locator.l_old_devid)
			Free((void *)(uintptr_t)c.c_locator.l_old_devid);
		return (0);
	}

	/*
	 * Now kernel should have all the information
	 * regarding the import diskset replica.
	 * Tell kernel to load them up and import the set
	 */
	if (metaioctl(MD_IOCIMP_LOAD, &c.c_setno, &c.c_mde, NULL) != 0) {
		Free((void *)(uintptr_t)c.c_locator.l_devid);
		if (c.c_locator.l_old_devid)
			Free((void *)(uintptr_t)c.c_locator.l_old_devid);
		return (mdstealerror(ep, &c.c_mde));
	}

	(void) meta_smf_enable(META_SMF_DISKSET, NULL);

	/* The set has now been imported, create the appropriate symlink */
	(void) snprintf(setname_link, MAXPATHLEN, "/dev/md/%s", setname);
	(void) snprintf(setnum_link, MAXPATHLEN, "shared/%d", c.c_setno);

	/*
	 * Since we already verified that the setname was OK, make sure to
	 * cleanup before proceeding.
	 */
	if (unlink(setname_link) == -1) {
		if (errno != ENOENT)
			(void) mdsyserror(ep, errno, setname_link);
	}

	if (symlink(setnum_link, setname_link) == -1)
		(void) mdsyserror(ep, errno, setname_link);

	/* resnarf the set that has just been imported */
	if (clnt_resnarf_set(mynode(), c.c_setno, ep) != 0)
		md_eprintf("%s\n", dgettext(TEXT_DOMAIN, "Please stop and "
		    "restart rpc.metad"));

	Free((void *)(uintptr_t)c.c_locator.l_devid);
	if (c.c_locator.l_old_devid)
		Free((void *)(uintptr_t)c.c_locator.l_old_devid);
	return (0);
}
