/*
 * Copyright 2010-2011 Red Hat, Inc.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU General Public License v2 or (at your option) any later version.
 */

#ifndef __DELTA_LEASE_H__
#define __DELTA_LEASE_H__

int delta_lease_leader_read(struct task *task,
			    int io_timeout,
			    struct sync_disk *disk,
			    char *space_name,
			    uint64_t host_id,
			    struct leader_record *leader_ret,
			    const char *caller);

int delta_lease_acquire(struct task *task,
                        struct space *sp,
                        struct sync_disk *disk,
                        char *space_name,
                        char *our_host_name,
                        uint64_t host_id,
                        struct leader_record *leader_ret);

int delta_lease_renew(struct task *task,
                      struct space *sp,
                      struct sync_disk *disk,
                      char *space_name,
                      char *bitmap,
		      struct delta_extra *extra,
                      int prev_result,
		      int *read_result,
		      int log_renewal_level,
                      struct leader_record *leader_last,
                      struct leader_record *leader_ret,
		      int *rd_ms, int *wr_ms);

int delta_lease_release(struct task *task,
                        struct space *sp,
                        struct sync_disk *disk,
                        char *space_name GNUC_UNUSED,
                        struct leader_record *leader_last,
                        struct leader_record *leader_ret);

int delta_lease_init(struct task *task,
		     int io_timeout,
		     struct sync_disk *disk,
		     char *space_name,
		     int max_hosts);

int delta_read_lockspace(struct task *task,
			struct sync_disk *disk,
			uint64_t host_id,
			struct sanlk_lockspace *ls,
			int io_timeout,
			int *io_timeout_ret);

int delta_lease_leader_clobber(struct task *task, int io_timeout,
                               struct sync_disk *disk,
                               uint64_t host_id,
                               struct leader_record *leader,
                               const char *caller);

#endif
