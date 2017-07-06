/*
 * Copyright 2010-2011 Red Hat, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <inttypes.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stddef.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <time.h>
#include <syslog.h>
#include <pthread.h>
#include <poll.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>

#include "sanlock.h"
#include "sanlock_resource.h"
#include "sanlock_admin.h"
#include "sanlock_sock.h"

#ifndef GNUC_UNUSED
#define GNUC_UNUSED __attribute__((__unused__))
#endif

static int connect_socket(int *sock_fd)
{
	int rv, s;
	struct sockaddr_un addr;

	*sock_fd = -1;
	s = socket(AF_LOCAL, SOCK_STREAM, 0);
	if (s < 0)
		return -errno;

	rv = sanlock_socket_address(&addr);
	if (rv < 0) {
		close(s);
		return rv;
	}

	rv = connect(s, (struct sockaddr *) &addr, sizeof(struct sockaddr_un));
	if (rv < 0) {
		rv = -errno;
		close(s);
		return rv;
	}
	*sock_fd = s;
	return 0;
}

static int send_header(int sock, int cmd, uint32_t cmd_flags, int datalen,
		       uint32_t data, uint32_t data2)
{
	struct sm_header header;
	int rv;

	memset(&header, 0, sizeof(header));
	header.magic = SM_MAGIC;
	header.version = SM_PROTO;
	header.cmd = cmd;
	header.cmd_flags = cmd_flags;
	header.length = sizeof(header) + datalen;
	header.data = data;
	header.data2 = data2;

retry:
	rv = send(sock, (void *) &header, sizeof(header), 0);
	if (rv == -1 && errno == EINTR)
		goto retry;

	if (rv < 0)
		return -errno;

	return 0;
}

static ssize_t send_data(int sockfd, const void *buf, size_t len, int flags)
{
	ssize_t rv;
retry:
	rv = send(sockfd, buf, len, flags);
	if (rv == -1 && errno == EINTR)
		goto retry;
	return rv;
}

static ssize_t recv_data(int sockfd, void *buf, size_t len, int flags)
{
	ssize_t rv;
retry:
	rv = recv(sockfd, buf, len, flags);
	if (rv == -1 && errno == EINTR)
		goto retry;
	return rv;
}

int send_command(int cmd, uint32_t data);

int send_command(int cmd, uint32_t data)
{
	int rv, sock;

	rv = connect_socket(&sock);
	if (rv < 0)
		return rv;

	rv = send_header(sock, cmd, 0, 0, data, 0);
	if (rv < 0) {
		close(sock);
		return rv;
	}

	return sock;
}

static int recv_result(int fd)
{
	struct sm_header h;
	int rv;

	memset(&h, 0, sizeof(h));
retry:
	rv = recv(fd, &h, sizeof(h), MSG_WAITALL);
	if (rv == -1 && errno == EINTR)
		goto retry;
	if (rv < 0)
		return -errno;
	if (rv != sizeof(h))
		return -1;

	return (int)h.data;
}

static int cmd_lockspace(int cmd, struct sanlk_lockspace *ls, uint32_t flags, uint32_t data)
{
	int rv, fd;

	rv = connect_socket(&fd);
	if (rv < 0)
		return rv;

	rv = send_header(fd, cmd, flags, sizeof(struct sanlk_lockspace), data, 0);
	if (rv < 0)
		goto out;

	rv = send_data(fd, (void *)ls, sizeof(struct sanlk_lockspace), 0);
	if (rv < 0) {
		rv = -errno;
		goto out;
	}

	rv = recv_result(fd);
 out:
	close(fd);
	return rv;
}

int sanlock_add_lockspace(struct sanlk_lockspace *ls, uint32_t flags)
{
	return cmd_lockspace(SM_CMD_ADD_LOCKSPACE, ls, flags, 0);
}

int sanlock_add_lockspace_timeout(struct sanlk_lockspace *ls, uint32_t flags, uint32_t io_timeout)
{
	return cmd_lockspace(SM_CMD_ADD_LOCKSPACE, ls, flags, io_timeout);
}

int sanlock_inq_lockspace(struct sanlk_lockspace *ls, uint32_t flags)
{
	return cmd_lockspace(SM_CMD_INQ_LOCKSPACE, ls, flags, 0);
}

int sanlock_rem_lockspace(struct sanlk_lockspace *ls, uint32_t flags)
{
	return cmd_lockspace(SM_CMD_REM_LOCKSPACE, ls, flags, 0);
}

int sanlock_get_lockspaces(struct sanlk_lockspace **lss, int *lss_count,
			   uint32_t flags)
{
	struct sanlk_lockspace *lsbuf, *ls;
	struct sm_header h;
	int rv, fd, i, ret, recv_count;

	rv = connect_socket(&fd);
	if (rv < 0)
		return rv;

	rv = send_header(fd, SM_CMD_GET_LOCKSPACES, flags, 0, 0, 0);
	if (rv < 0)
		goto out;

	/* receive result and ls structs */

	memset(&h, 0, sizeof(h));

	rv = recv_data(fd, &h, sizeof(h), MSG_WAITALL);
	if (rv < 0) {
		rv = -errno;
		goto out;
	}

	if (rv != sizeof(h)) {
		rv = -1;
		goto out;
	}

	/* -ENOSPC means that the daemon's send buffer ran out of space */

	rv = (int)h.data;
	if (rv < 0 && rv != -ENOSPC)
		goto out;

	*lss_count = h.data2;
	recv_count = h.data2;

	if (!lss)
		goto out;

	lsbuf = malloc(recv_count * sizeof(struct sanlk_lockspace));
	if (!lsbuf)
		goto out;

	ls = lsbuf;

	for (i = 0; i < recv_count; i++) {
		ret = recv_data(fd, ls, sizeof(struct sanlk_lockspace), MSG_WAITALL);
		if (ret < 0) {
			rv = -errno;
			free(lsbuf);
			goto out;
		}

		if (ret != sizeof(struct sanlk_lockspace)) {
			rv = -1;
			free(lsbuf);
			goto out;
		}

		ls++;
	}

	*lss = lsbuf;
 out:
	close(fd);
	return rv;
}

int sanlock_get_hosts(const char *ls_name, uint64_t host_id,
		      struct sanlk_host **hss, int *hss_count,
		      uint32_t flags)
{
	struct sm_header h;
	struct sanlk_lockspace ls;
	struct sanlk_host *hsbuf, *hs;
	int rv, fd, i, ret, recv_count;

	if (!ls_name)
		return -EINVAL;

	memset(&ls, 0, sizeof(struct sanlk_lockspace));
	strncpy(ls.name, ls_name, SANLK_NAME_LEN);
	ls.host_id = host_id;

	rv = connect_socket(&fd);
	if (rv < 0)
		return rv;

	rv = send_header(fd, SM_CMD_GET_HOSTS, flags,
			 sizeof(struct sanlk_lockspace),
			 0, 0);
	if (rv < 0)
		goto out;

	rv = send_data(fd, &ls, sizeof(struct sanlk_lockspace), 0);
	if (rv < 0) {
		rv = -errno;
		goto out;
	}

	/* receive result and ls structs */

	memset(&h, 0, sizeof(h));

	rv = recv_data(fd, &h, sizeof(h), MSG_WAITALL);
	if (rv < 0) {
		rv = -errno;
		goto out;
	}

	if (rv != sizeof(h)) {
		rv = -1;
		goto out;
	}

	/* -ENOSPC means that the daemon's send buffer ran out of space */

	rv = (int)h.data;
	if (rv < 0 && rv != -ENOSPC)
		goto out;

	*hss_count = h.data2;
	recv_count = h.data2;

	if (!hss)
		goto out;

	hsbuf = malloc(recv_count * sizeof(struct sanlk_host));
	if (!hsbuf)
		goto out;

	hs = hsbuf;

	for (i = 0; i < recv_count; i++) {
		ret = recv_data(fd, hs, sizeof(struct sanlk_host), MSG_WAITALL);
		if (ret < 0) {
			rv = -errno;
			free(hsbuf);
			goto out;
		}

		if (ret != sizeof(struct sanlk_host)) {
			rv = -1;
			free(hsbuf);
			goto out;
		}

		hs++;
	}

	*hss = hsbuf;
 out:
	close(fd);
	return rv;
}

int sanlock_set_config(const char *ls_name, uint32_t flags, uint32_t cmd, GNUC_UNUSED void *data)
{
	struct sanlk_lockspace ls;
	struct sm_header h;
	int rv, fd;

	if (!ls_name)
		return -EINVAL;

	memset(&ls, 0, sizeof(struct sanlk_lockspace));
	strncpy(ls.name, ls_name, SANLK_NAME_LEN);

	rv = connect_socket(&fd);
	if (rv < 0)
		return rv;

	rv = send_header(fd, SM_CMD_SET_CONFIG, flags,
			 sizeof(struct sanlk_lockspace),
			 cmd, 0);
	if (rv < 0)
		goto out;

	rv = send_data(fd, &ls, sizeof(ls), 0);
	if (rv < 0) {
		rv = -errno;
		goto out;
	}

	memset(&h, 0, sizeof(h));

	rv = recv_data(fd, &h, sizeof(h), MSG_WAITALL);
	if (rv < 0) {
		rv = -errno;
		goto out;
	}

	if (rv != sizeof(h)) {
		rv = -1;
		goto out;
	}

	rv = (int)h.data;
 out:
	close(fd);
	return rv;
}

int sanlock_align(struct sanlk_disk *disk)
{
	int rv, fd;

	rv = connect_socket(&fd);
	if (rv < 0)
		return rv;

	rv = send_header(fd, SM_CMD_ALIGN, 0, sizeof(struct sanlk_disk), 0, 0);
	if (rv < 0)
		goto out;

	rv = send_data(fd, (void *)disk, sizeof(struct sanlk_disk), 0);
	if (rv < 0) {
		rv = -errno;
		goto out;
	}

	rv = recv_result(fd);
 out:
	close(fd);
	return rv;
}

int sanlock_read_lockspace(struct sanlk_lockspace *ls, uint32_t flags, uint32_t *io_timeout)
{
	struct sm_header h;
	int rv, fd;

	if (!ls || !ls->host_id_disk.path[0])
		return -EINVAL;

	rv = connect_socket(&fd);
	if (rv < 0)
		return rv;

	rv = send_header(fd, SM_CMD_READ_LOCKSPACE, flags,
			 sizeof(struct sanlk_lockspace),
			 0, 0);
	if (rv < 0)
		goto out;

	rv = send_data(fd, ls, sizeof(struct sanlk_lockspace), 0);
	if (rv < 0) {
		rv = -errno;
		goto out;
	}

	/* receive result, io_timeout and ls struct */

	memset(&h, 0, sizeof(h));

	rv = recv_data(fd, &h, sizeof(h), MSG_WAITALL);
	if (rv < 0) {
		rv = -errno;
		goto out;
	}

	if (rv != sizeof(h)) {
		rv = -1;
		goto out;
	}

	rv = (int)h.data;
	if (rv < 0)
		goto out;

	rv = recv_data(fd, ls, sizeof(struct sanlk_lockspace), MSG_WAITALL);
	if (rv < 0) {
		rv = -errno;
		goto out;
	}

	if (rv != sizeof(struct sanlk_lockspace)) {
		rv = -1;
		goto out;
	}

	*io_timeout = h.data2;
	rv = (int)h.data;
 out:
	close(fd);
	return rv;
}

int sanlock_read_resource(struct sanlk_resource *res, uint32_t flags)
{
	struct sm_header h;
	int rv, fd;

	if (!res || !res->num_disks || res->num_disks > SANLK_MAX_DISKS ||
	    !res->disks[0].path[0])
		return -EINVAL;

	rv = connect_socket(&fd);
	if (rv < 0)
		return rv;

	rv = send_header(fd, SM_CMD_READ_RESOURCE, flags,
			 sizeof(struct sanlk_resource) +
			 sizeof(struct sanlk_disk) * res->num_disks,
			 0, 0);
	if (rv < 0)
		goto out;

	rv = send_data(fd, res, sizeof(struct sanlk_resource), 0);
	if (rv < 0) {
		rv = -errno;
		goto out;
	}

	rv = send_data(fd, res->disks, sizeof(struct sanlk_disk) * res->num_disks, 0);
	if (rv < 0) {
		rv = -errno;
		goto out;
	}

	/* receive result and res struct */

	memset(&h, 0, sizeof(h));

	rv = recv_data(fd, &h, sizeof(h), MSG_WAITALL);
	if (rv < 0) {
		rv = -errno;
		goto out;
	}

	if (rv != sizeof(h)) {
		rv = -1;
		goto out;
	}

	rv = (int)h.data;
	if (rv < 0)
		goto out;

	rv = recv_data(fd, res, sizeof(struct sanlk_resource), MSG_WAITALL);
	if (rv < 0) {
		rv = -errno;
		goto out;
	}

	if (rv != sizeof(struct sanlk_resource)) {
		rv = -1;
		goto out;
	}

	rv = (int)h.data;
 out:
	close(fd);
	return rv;
}

int sanlock_write_lockspace(struct sanlk_lockspace *ls, int max_hosts,
			    uint32_t flags, uint32_t io_timeout)
{
	int rv, fd;

	if (!ls || !ls->host_id_disk.path[0])
		return -EINVAL;

	rv = connect_socket(&fd);
	if (rv < 0)
		return rv;

	rv = send_header(fd, SM_CMD_WRITE_LOCKSPACE, flags,
			 sizeof(struct sanlk_lockspace),
			 max_hosts, io_timeout);
	if (rv < 0)
		goto out;

	rv = send_data(fd, ls, sizeof(struct sanlk_lockspace), 0);
	if (rv < 0) {
		rv = -errno;
		goto out;
	}

	rv = recv_result(fd);
 out:
	close(fd);
	return rv;
}

int sanlock_write_resource(struct sanlk_resource *res,
			   int max_hosts, int num_hosts, uint32_t flags)
{
	int rv, fd;

	if (!res || !res->num_disks || res->num_disks > SANLK_MAX_DISKS ||
	    !res->disks[0].path[0])
		return -EINVAL;

	rv = connect_socket(&fd);
	if (rv < 0)
		return rv;

	rv = send_header(fd, SM_CMD_WRITE_RESOURCE, flags,
			 sizeof(struct sanlk_resource) +
			 sizeof(struct sanlk_disk) * res->num_disks,
			 max_hosts, num_hosts);
	if (rv < 0)
		goto out;

	rv = send_data(fd, res, sizeof(struct sanlk_resource), 0);
	if (rv < 0) {
		rv = -errno;
		goto out;
	}

	rv = send_data(fd, res->disks, sizeof(struct sanlk_disk) * res->num_disks, 0);
	if (rv < 0) {
		rv = -errno;
		goto out;
	}

	rv = recv_result(fd);
 out:
	close(fd);
	return rv;
}

int sanlock_read_resource_owners(struct sanlk_resource *res, uint32_t flags,
				 struct sanlk_host **hss, int *hss_count)
{
	struct sm_header h;
	struct sanlk_host *hsbuf, *hs;
	int rv, fd, i, ret, recv_count;

	if (!res || !res->num_disks || res->num_disks > SANLK_MAX_DISKS ||
	    !res->disks[0].path[0])
		return -EINVAL;

	rv = connect_socket(&fd);
	if (rv < 0)
		return rv;

	rv = send_header(fd, SM_CMD_READ_RESOURCE_OWNERS, flags,
			 sizeof(struct sanlk_resource) +
			 sizeof(struct sanlk_disk) * res->num_disks,
			 0, 0);
	if (rv < 0)
		goto out;

	rv = send_data(fd, res, sizeof(struct sanlk_resource), 0);
	if (rv < 0) {
		rv = -errno;
		goto out;
	}

	rv = send_data(fd, res->disks, sizeof(struct sanlk_disk) * res->num_disks, 0);
	if (rv < 0) {
		rv = -errno;
		goto out;
	}

	/* receive result, res struct, and host structs */

	memset(&h, 0, sizeof(h));

	rv = recv_data(fd, &h, sizeof(h), MSG_WAITALL);
	if (rv < 0) {
		rv = -errno;
		goto out;
	}

	if (rv != sizeof(h)) {
		rv = -1;
		goto out;
	}

	rv = (int)h.data;
	if (rv < 0)
		goto out;

	rv = recv_data(fd, res, sizeof(struct sanlk_resource), MSG_WAITALL);
	if (rv < 0) {
		rv = -errno;
		goto out;
	}

	if (rv != sizeof(struct sanlk_resource)) {
		rv = -1;
		goto out;
	}

	rv = 0;

	*hss_count = h.data2;
	recv_count = h.data2;

	if (!hss)
		goto out;

	hsbuf = malloc(recv_count * sizeof(struct sanlk_host));
	if (!hsbuf)
		goto out;

	hs = hsbuf;

	for (i = 0; i < recv_count; i++) {
		ret = recv_data(fd, hs, sizeof(struct sanlk_host), MSG_WAITALL);
		if (ret < 0) {
			rv = -errno;
			free(hsbuf);
			goto out;
		}

		if (ret != sizeof(struct sanlk_host)) {
			rv = -1;
			free(hsbuf);
			goto out;
		}

		hs++;
	}

	*hss = hsbuf;
 out:
	close(fd);
	return rv;
}

int sanlock_test_resource_owners(struct sanlk_resource *res GNUC_UNUSED,
				 uint32_t flags GNUC_UNUSED,
				 struct sanlk_host *owners, int owners_count,
				 struct sanlk_host *hosts, int hosts_count,
				 uint32_t *test_flags)
{
	struct sanlk_host *owner, *host;
	int i, j, found, fail = 0;

	*test_flags = 0;

	owner = owners;

	for (i = 0; i < owners_count; i++) {
		found = 0;
		host = hosts;
		for (j = 0; j < hosts_count; j++) {
			if (owner->host_id != host->host_id) {
				host++;
				continue;
			}
			found = 1;
			break;
		}

		if (!found)
			goto next;

		if (host->generation > owner->generation)
			goto next;

		/* this should not be possible, and should never happen */
		if (host->generation < owner->generation)
			return -EINVAL;

		switch (host->flags & SANLK_HOST_MASK) {
		case SANLK_HOST_FREE:
		case SANLK_HOST_DEAD:
			break;
		case SANLK_HOST_LIVE:
		case SANLK_HOST_FAIL:
		case SANLK_HOST_UNKNOWN:
			fail = 1;
			break;
		default:
			return -EINVAL;
		}
 next:
		owner++;
	}

	if (fail)
		*test_flags |= SANLK_TRF_FAIL;

	return 0;
}

int sanlock_reg_event(const char *ls_name, struct sanlk_host_event *he, uint32_t flags)
{
	struct sm_header h;
	struct sanlk_lockspace ls;
	struct sanlk_host_event ev;
	int rv, reg_fd;

	if (!ls_name)
		return -EINVAL;

	memset(&ls, 0, sizeof(ls));
	strncpy(ls.name, ls_name, SANLK_NAME_LEN);

	memset(&ev, 0, sizeof(ev));
	if (he)
		memcpy(&ev, he, sizeof(ev));

	rv = connect_socket(&reg_fd);
	if (rv < 0)
		return rv;

	rv = send_header(reg_fd, SM_CMD_REG_EVENT, flags, sizeof(ls) + sizeof(ev), 0, 0);
	if (rv < 0)
		goto fail;

	rv = send_data(reg_fd, &ls, sizeof(ls), 0);
	if (rv < 0)
		goto fail;

	rv = send_data(reg_fd, &ev, sizeof(ev), 0);
	if (rv < 0)
		goto fail;

	memset(&h, 0, sizeof(h));

	rv = recv_data(reg_fd, &h, sizeof(h), MSG_WAITALL);
	if (rv < 0) {
		rv = -errno;
		goto fail;
	}

	if (rv != sizeof(h)) {
		rv = -1;
		goto fail;
	}

	rv = (int)h.data;
	if (rv < 0)
		goto fail;

	return reg_fd;
fail:
	close(reg_fd);
	return rv;
}

int sanlock_end_event(int reg_fd, const char *ls_name, uint32_t flags)
{
	struct sm_header h;
	struct sanlk_lockspace ls;
	int rv, fd;
	uint32_t end = 1;

	if (!ls_name)
		return -EINVAL;

	/*
	 * write 4 bytes to the registered fd.  sanlock attempts
	 * a non-blocking read of 4 bytes from registered fds to
	 * check if they have been unregistered.
	 */

	rv = send_data(reg_fd, &end, sizeof(end), 0);
	if (rv < 0) {
		close(reg_fd);
		return -EALREADY;
	}

	close(reg_fd);

	/*
	 * sanlock does not poll registered event fds because
	 * it receives nothing from them during normal operation,
	 * only to indicate it's being closed.  So, we need
	 * to tell sanlock to check the registered event fds to
	 * remove the one we've written to and closed above.
	 */

	memset(&ls, 0, sizeof(ls));
	strncpy(ls.name, ls_name, SANLK_NAME_LEN);

	rv = connect_socket(&fd);
	if (rv < 0)
		return rv;

	rv = send_header(fd, SM_CMD_END_EVENT, flags, sizeof(ls), 0, 0);
	if (rv < 0)
		goto out;

	rv = send_data(fd, &ls, sizeof(ls), 0);
	if (rv < 0)
		goto out;

	memset(&h, 0, sizeof(h));

	rv = recv_data(fd, &h, sizeof(h), MSG_WAITALL);
	if (rv < 0) {
		rv = -errno;
		goto out;
	}

	if (rv != sizeof(h)) {
		rv = -1;
		goto out;
	}

	rv = (int)h.data;
	if (rv < 0)
		goto out;

	rv = 0;
out:
	close(fd);
	return rv;
}

int sanlock_set_event(const char *ls_name, struct sanlk_host_event *he, uint32_t flags)
{
	struct sanlk_lockspace ls;
	struct sm_header h;
	int rv, fd;

	if (!ls_name || !he)
		return -EINVAL;

	memset(&ls, 0, sizeof(struct sanlk_lockspace));
	strncpy(ls.name, ls_name, SANLK_NAME_LEN);

	rv = connect_socket(&fd);
	if (rv < 0)
		return rv;

	rv = send_header(fd, SM_CMD_SET_EVENT, flags,
			 sizeof(struct sanlk_lockspace) + sizeof(struct sanlk_host_event),
			 0, 0);
	if (rv < 0)
		goto out;

	rv = send_data(fd, &ls, sizeof(ls), 0);
	if (rv < 0) {
		rv = -errno;
		goto out;
	}

	rv = send_data(fd, he, sizeof(struct sanlk_host_event), 0);
	if (rv < 0) {
		rv = -errno;
		goto out;
	}

	memset(&h, 0, sizeof(h));

	rv = recv_data(fd, &h, sizeof(h), MSG_WAITALL);
	if (rv < 0) {
		rv = -errno;
		goto out;
	}

	if (rv != sizeof(h)) {
		rv = -1;
		goto out;
	}

	rv = (int)h.data;
 out:
	close(fd);
	return rv;
}

int sanlock_get_event(int reg_fd, GNUC_UNUSED uint32_t flags, struct sanlk_host_event *he,
		      uint64_t *from_host_id, uint64_t *from_generation)
{
	struct event_cb cb;
	int rv;

	/*
	 * The caller's poll(2) indicates there's data, it doesn't know how 
	 * many events to read, and doesn't want to block, so they want to
	 * get events until we return -EAGAIN to indicate there are no more.
	 */

	rv = recv_data(reg_fd, &cb, sizeof(cb), MSG_DONTWAIT);
	if (rv < 0)
		return -errno;

	if (rv != sizeof(cb))
		return -1;

	memcpy(he, &cb.he, sizeof(struct sanlk_host_event));

	if (from_host_id)
		*from_host_id = cb.from_host_id;
	if (from_generation)
		*from_generation = cb.from_generation;

	return 0;
}

/* old api */
int sanlock_init(struct sanlk_lockspace *ls,
		 struct sanlk_resource *res,
		 int max_hosts, int num_hosts)
{
	if (ls)
		return sanlock_write_lockspace(ls, max_hosts, 0, 0);
	else
		return sanlock_write_resource(res, max_hosts, num_hosts, 0);
}

int sanlock_register(void)
{
	int sock, rv;

	rv = connect_socket(&sock);
	if (rv < 0)
		return rv;

	rv = send_header(sock, SM_CMD_REGISTER, 0, 0, 0, 0);
	if (rv < 0) {
		close(sock);
		return rv;
	}

	return sock;
}

int sanlock_restrict(int sock, uint32_t flags)
{
	int rv;

	rv = send_header(sock, SM_CMD_RESTRICT, flags, 0, 0, -1);
	if (rv < 0)
		return rv;

	rv = recv_result(sock);
	return rv;
}

int sanlock_version(uint32_t flags, uint32_t *version, uint32_t *proto)
{
	struct sm_header h;
	int fd, rv;

	rv = connect_socket(&fd);
	if (rv < 0)
		return rv;

	rv = send_header(fd, SM_CMD_VERSION, flags, 0, 0, 0);
	if (rv < 0)
		goto out;

	memset(&h, 0, sizeof(h));

	rv = recv_data(fd, &h, sizeof(h), MSG_WAITALL);
	if (rv < 0) {
		rv = -errno;
		goto out;
	}

	if (rv != sizeof(h)) {
		rv = -1;
		goto out;
	}

	if (proto)
		*proto = h.version;

	rv = (int)h.data;
	if (rv < 0)
		goto out;

	*version = h.data2;
	rv = 0;
 out:
	close(fd);
	return rv;
}

int sanlock_killpath(int sock, uint32_t flags, const char *path, char *args)
{
	char path_max[SANLK_HELPER_PATH_LEN];
	char args_max[SANLK_HELPER_ARGS_LEN];
	int rv, datalen;

	datalen = SANLK_HELPER_PATH_LEN + SANLK_HELPER_ARGS_LEN;

	memset(path_max, 0, sizeof(path_max));
	memset(args_max, 0, sizeof(args_max));

	snprintf(path_max, SANLK_HELPER_PATH_LEN-1, "%s", path);
	snprintf(args_max, SANLK_HELPER_ARGS_LEN-1, "%s", args);

	rv = send_header(sock, SM_CMD_KILLPATH, flags, datalen, 0, -1);
	if (rv < 0)
		return rv;

	rv = send_data(sock, path_max, SANLK_HELPER_PATH_LEN, 0);
	if (rv < 0) {
		rv = -errno;
		goto out;
	}

	rv = send_data(sock, args_max, SANLK_HELPER_ARGS_LEN, 0);
	if (rv < 0) {
		rv = -errno;
		goto out;
	}

	rv = recv_result(sock);
 out:
	return rv;
}

int sanlock_acquire(int sock, int pid, uint32_t flags, int res_count,
		    struct sanlk_resource *res_args[],
		    struct sanlk_options *opt_in)
{
	struct sanlk_resource *res;
	struct sanlk_options opt;
	int rv, i, fd, data2;
	int datalen = 0;

	if (res_count > SANLK_MAX_RESOURCES)
		return -EINVAL;

	for (i = 0; i < res_count; i++) {
		res = res_args[i];
		datalen += sizeof(struct sanlk_resource);

		if (res->num_disks > SANLK_MAX_DISKS)
			return -EINVAL;

		datalen += (res->num_disks * sizeof(struct sanlk_disk));
	}

	datalen += sizeof(struct sanlk_options);
	if (opt_in) {
		memcpy(&opt, opt_in, sizeof(struct sanlk_options));
		datalen += opt_in->len;
	} else {
		memset(&opt, 0, sizeof(opt));
	}

	if (sock == -1) {
		/* connect to daemon and ask it to acquire a lease for
		   another registered pid */

		data2 = pid;

		rv = connect_socket(&fd);
		if (rv < 0)
			return rv;
	} else {
		/* use our own existing registered connection and ask daemon
		   to acquire a lease for self */

		data2 = -1;
		fd = sock;
	}

	rv = send_header(fd, SM_CMD_ACQUIRE, flags, datalen, res_count, data2);
	if (rv < 0)
		return rv;

	for (i = 0; i < res_count; i++) {
		res = res_args[i];
		rv = send_data(fd, res, sizeof(struct sanlk_resource), 0);
		if (rv < 0) {
			rv = -1;
			goto out;
		}

		rv = send_data(fd, res->disks, sizeof(struct sanlk_disk) * res->num_disks, 0);
		if (rv < 0) {
			rv = -1;
			goto out;
		}
	}

	rv = send_data(fd, &opt, sizeof(struct sanlk_options), 0);
	if (rv < 0) {
		rv = -1;
		goto out;
	}

	if (opt.len) {
		rv = send_data(fd, opt_in->str, opt.len, 0);
		if (rv < 0) {
			rv = -1;
			goto out;
		}
	}

	rv = recv_result(fd);
 out:
	if (sock == -1)
		close(fd);
	return rv;
}

int sanlock_inquire(int sock, int pid, uint32_t flags, int *res_count,
		    char **res_state)
{
	struct sm_header h;
	char *reply_data = NULL;
	int rv, fd, data2, len;

	*res_count = 0;

	if (res_state)
		*res_state = NULL;

	if (sock == -1) {
		/* connect to daemon and ask it to acquire a lease for
		   another registered pid */

		data2 = pid;

		rv = connect_socket(&fd);
		if (rv < 0)
			return rv;
	} else {
		/* use our own existing registered connection and ask daemon
		   to acquire a lease for self */

		data2 = -1;
		fd = sock;
	}

	rv = send_header(fd, SM_CMD_INQUIRE, flags, 0, 0, data2);
	if (rv < 0)
		return rv;

	/* get result */

	memset(&h, 0, sizeof(h));

	rv = recv_data(fd, &h, sizeof(h), MSG_WAITALL);
	if (rv != sizeof(h)) {
		rv = -1;
		goto out;
	}

	len = h.length - sizeof(h);
	if (!len) {
		rv = (int)h.data;
		goto out;
	}

	reply_data = malloc(len);
	if (!reply_data) {
		rv = -ENOMEM;
		goto out;
	}

	rv = recv_data(fd, reply_data, len, MSG_WAITALL);
	if (rv != len) {
		free(reply_data);
		rv = -1;
		goto out;
	}

	if (res_state)
		*res_state = reply_data;
	else
		free(reply_data);

	*res_count = (int)h.data2;
	rv = (int)h.data;
 out:
	if (sock == -1)
		close(fd);
	return rv;
}

int sanlock_convert(int sock, int pid, uint32_t flags, struct sanlk_resource *res)
{
	int fd, rv, data2, datalen;

	if (!res)
		return -EINVAL;

	if (sock == -1) {
		/* connect to daemon and ask it to acquire a lease for
		   another registered pid */

		data2 = pid;

		rv = connect_socket(&fd);
		if (rv < 0)
			return rv;
	} else {
		/* use our own existing registered connection and ask daemon
		   to acquire a lease for self */

		data2 = -1;
		fd = sock;
	}

	datalen = sizeof(struct sanlk_resource);

	rv = send_header(fd, SM_CMD_CONVERT, flags, datalen, 0, data2);
	if (rv < 0)
		goto out;

	rv = send_data(fd, res, sizeof(struct sanlk_resource), 0);
	if (rv < 0) {
		rv = -errno;
		goto out;
	}

	rv = recv_result(fd);
 out:
	if (sock == -1)
		close(fd);
	return rv;
}

/* tell daemon to release lease(s) for given pid.
   I don't think the pid itself will usually tell sm to release leases,
   but it will be requested by a manager overseeing the pid */

int sanlock_release(int sock, int pid, uint32_t flags, int res_count,
		    struct sanlk_resource *res_args[])
{
	int fd, rv, i, data2, datalen;

	if (sock == -1) {
		/* connect to daemon and ask it to acquire a lease for
		   another registered pid */

		data2 = pid;

		rv = connect_socket(&fd);
		if (rv < 0)
			return rv;
	} else {
		/* use our own existing registered connection and ask daemon
		   to acquire a lease for self */

		data2 = -1;
		fd = sock;
	}

	datalen = res_count * sizeof(struct sanlk_resource);

	rv = send_header(fd, SM_CMD_RELEASE, flags, datalen, res_count, data2);
	if (rv < 0)
		goto out;

	for (i = 0; i < res_count; i++) {
		rv = send_data(fd, res_args[i], sizeof(struct sanlk_resource), 0);
		if (rv < 0) {
			rv = -1;
			goto out;
		}
	}

	rv = recv_result(fd);
 out:
	if (sock == -1)
		close(fd);
	return rv;
}

int sanlock_request(uint32_t flags, uint32_t force_mode,
		    struct sanlk_resource *res)
{
	int fd, rv, datalen;

	if (!res)
		return -EINVAL;

	datalen = sizeof(struct sanlk_resource) +
		  sizeof(struct sanlk_disk) * res->num_disks;

	rv = connect_socket(&fd);
	if (rv < 0)
		return rv;

	rv = send_header(fd, SM_CMD_REQUEST, flags, datalen, force_mode, 0);
	if (rv < 0)
		goto out;

	rv = send_data(fd, res, sizeof(struct sanlk_resource), 0);
	if (rv < 0) {
		rv = -errno;
		goto out;
	}

	rv = send_data(fd, res->disks, sizeof(struct sanlk_disk) * res->num_disks, 0);
	if (rv < 0) {
		rv = -errno;
		goto out;
	}

	rv = recv_result(fd);
 out:
	close(fd);
	return rv;
}

int sanlock_examine(uint32_t flags, struct sanlk_lockspace *ls,
		    struct sanlk_resource *res)
{
	char *data;
	int rv, fd, cmd, datalen;

	if (!ls && !res)
		return -EINVAL;

	rv = connect_socket(&fd);
	if (rv < 0)
		return rv;

	if (ls && ls->host_id_disk.path[0]) {
		cmd = SM_CMD_EXAMINE_LOCKSPACE;
		datalen = sizeof(struct sanlk_lockspace);
		data = (char *)ls;
	} else {
		cmd = SM_CMD_EXAMINE_RESOURCE;
		datalen = sizeof(struct sanlk_resource);
		data = (char *)res;
	}

	rv = send_header(fd, cmd, flags, datalen, 0, 0);
	if (rv < 0)
		goto out;

	rv = send_data(fd, data, datalen, 0);
	if (rv < 0) {
		rv = -errno;
		goto out;
	}

	rv = recv_result(fd);
 out:
	close(fd);
	return rv;
}

int sanlock_set_lvb(uint32_t flags, struct sanlk_resource *res, char *lvb, int lvblen)
{
	int datalen = 0;
	int rv, fd;

	if (!res || !lvb || !lvblen)
		return -EINVAL;

	datalen = sizeof(struct sanlk_resource) + lvblen;

	rv = connect_socket(&fd);
	if (rv < 0)
		return rv;

	rv = send_header(fd, SM_CMD_SET_LVB, flags, datalen, 0, 0);
	if (rv < 0)
		return rv;

	rv = send_data(fd, res, sizeof(struct sanlk_resource), 0);
	if (rv < 0) {
		rv = -errno;
		goto out;
	}

	rv = send_data(fd, lvb, lvblen, 0);
	if (rv < 0) {
		rv = -1;
		goto out;
	}

	rv = recv_result(fd);
 out:
	close(fd);
	return rv;
}

int sanlock_get_lvb(uint32_t flags, struct sanlk_resource *res, char *lvb, int lvblen)
{
	struct sm_header h;
	char *reply_data = NULL;
	int datalen = 0;
	int rv, fd, len;

	if (!res || !lvb || !lvblen)
		return -EINVAL;

	datalen = sizeof(struct sanlk_resource);

	rv = connect_socket(&fd);
	if (rv < 0)
		return rv;

	rv = send_header(fd, SM_CMD_GET_LVB, flags, datalen, 0, 0);
	if (rv < 0)
		return rv;

	rv = send_data(fd, res, sizeof(struct sanlk_resource), 0);
	if (rv < 0) {
		rv = -1;
		goto out;
	}

	/* get result */

	memset(&h, 0, sizeof(h));

	rv = recv_data(fd, &h, sizeof(h), MSG_WAITALL);
	if (rv != sizeof(h)) {
		rv = -1;
		goto out;
	}

	len = h.length - sizeof(h);
	if (!len) {
		rv = (int)h.data;
		goto out;
	}

	reply_data = malloc(len);
	if (!reply_data) {
		rv = -ENOMEM;
		goto out;
	}

	rv = recv_data(fd, reply_data, len, MSG_WAITALL);
	if (rv != len) {
		free(reply_data);
		rv = -1;
		goto out;
	}

	if (lvblen < len)
		len = lvblen;

	memcpy(lvb, reply_data, len);

	free(reply_data);

	rv = (int)h.data;
 out:
	close(fd);
	return rv;
}

/*
 * src may have colons/spaces escaped (with backslash) or unescaped.
 * if unescaped colons/spaces are found, insert backslash before them.
 *
 * returns strlen of dst.
 */

size_t sanlock_path_export(char *dst, const char *src, size_t dstlen)
{
	int i = 0; /* pos in src */
	int j = 0; /* pos in dst */

	memset(dst, 0, dstlen);

	for (i = 0; i < strlen(src); i++) {

		/* take an escape character plus whatever follows it. */

		if (src[i] == '\\') {
			if (j > dstlen - 3)
				goto out;

			dst[j] = src[i];
			j++;
			i++;
			dst[j] = src[i];

			goto next_char;
		}

		/* add escape character before an unescaped space or colon. */

		if ((src[i] == ' ') || (src[i] == ':')) {
			if (j > dstlen - 3)
				goto out;

			dst[j] = '\\';
			j++;
			dst[j] = src[i];

			goto next_char;
		}

		/* copy non-special char from src to dst. */

		if (j > dstlen - 2)
			goto out;

		dst[j] = src[i];

 next_char:
		if (dst[j] == '\0')
			goto out;

		j++;
	}
 out:
	return strlen(dst);
}

/* src has colons/spaces escaped with backslash, dst should have backslash removed */

size_t sanlock_path_import(char *dst, const char *src, size_t dstlen)
{
	size_t j = 0;
	const char *p = src;

	while (j < dstlen) {
		if (*p == '\\')
			goto next_loop;

		dst[j] = *p;

		if (*p == '\0')
			return j;

		j++;

 next_loop:
		p++;
	}

	return 0;
}

/*
 * convert from struct sanlk_resource to string with format:
 * <lockspace_name>:<resource_name>:<path>:<offset>[:<path>:<offset>...]:<lver>
 */

int sanlock_res_to_str(struct sanlk_resource *res, char **str_ret)
{
	char path[SANLK_PATH_LEN + 1];
	char *str;
	int ret, len, pos, d;

	str = malloc(SANLK_MAX_RES_STR + 1);
	if (!str)
		return -ENOMEM;
	memset(str, 0, SANLK_MAX_RES_STR + 1);

	len = SANLK_MAX_RES_STR;
	pos = 0;

	ret = snprintf(str + pos, len - pos, "%s:%s",
		       res->lockspace_name, res->name);

	if (ret >= len - pos)
		goto fail;
	pos += ret;

	for (d = 0; d < res->num_disks; d++) {
		memset(path, 0, sizeof(path));
		sanlock_path_export(path, res->disks[d].path, sizeof(path));

		ret = snprintf(str + pos, len - pos, ":%s:%llu", path,
			       (unsigned long long)res->disks[d].offset);

		if (ret >= len - pos)
			goto fail;
		pos += ret;
	}

	if (res->flags & SANLK_RES_SHARED)
		ret = snprintf(str + pos, len - pos, ":SH");
	else
		ret = snprintf(str + pos, len - pos, ":%llu",
			       (unsigned long long)res->lver);

	if (ret > len - pos)
		goto fail;
	pos += ret;

	if (pos > len)
		goto fail;

	*str_ret = str;
	return 0;

 fail:
	free(str);
	return -EINVAL;
}

/*
 * convert to struct sanlk_resource from string with format:
 * <lockspace_name>:<resource_name>:<path>:<offset>[:<path>:<offset>...][:<lver>]
 *
 * If str contains a backslash escape character, the backslash needs to be
 * excluded from the string in res struct.  The path string in the res struct
 * needs to be suitable for passing to open(2), which means it should not
 * include escape characters.
 */

int sanlock_str_to_res(char *str, struct sanlk_resource **res_ret)
{
	struct sanlk_resource *res;
	char sub[SANLK_PATH_LEN + 1];
	int i, j, d, rv, len, sub_count, colons, num_disks, have_lver;

	if (strlen(str) < 3)
		return -ENXIO;

	colons = 0;
	for (i = 0; i < strlen(str); i++) {
		if (str[i] == '\\') {
			i++;
			continue;
		}

		if (str[i] == ':')
			colons++;
	}
	if (!colons || (colons == 2)) {
		return -1;
	}

	num_disks = (colons - 1) / 2;
	have_lver = (colons - 1) % 2;

	if (num_disks > SANLK_MAX_DISKS)
		return -2;

	len = sizeof(struct sanlk_resource) + num_disks * sizeof(struct sanlk_disk);

	res = malloc(len);
	if (!res)
		return -ENOMEM;
	memset(res, 0, len);

	res->num_disks = num_disks;

	d = 0;
	sub_count = 0;
	j = 0;
	memset(sub, 0, sizeof(sub));

	len = strlen(str);

	for (i = 0; i < len + 1; i++) {
		if (str[i] == '\\') {
			if (i == (len - 1))
				goto fail;
			if (j >= SANLK_PATH_LEN)
				goto fail;

			i++;
			sub[j++] = str[i];
			continue;
		}
		if (i < len && str[i] != ':') {
			if (j >= SANLK_PATH_LEN)
				goto fail;
			sub[j++] = str[i];
			continue;
		}

		/* do something with sub when we hit ':' or end of str,
		   first and second subs are lockspace and resource names,
		   then even sub is path, odd sub is offset */

		if (sub_count < 2 && strlen(sub) > SANLK_NAME_LEN)
			goto fail;
		if (sub_count >= 2 && (strlen(sub) > SANLK_PATH_LEN-1 || strlen(sub) < 1))
			goto fail;

		if (sub_count == 0) {
			strncpy(res->lockspace_name, sub, SANLK_NAME_LEN);

		} else if (sub_count == 1) {
			strncpy(res->name, sub, SANLK_NAME_LEN);

		} else if (!(sub_count % 2)) {
			if (have_lver && (d == num_disks)) {
				if (!strncmp(sub, "SH", 2)) {
					res->flags |= SANLK_RES_SHARED;
				} else {
					res->flags |= SANLK_RES_LVER;
					res->lver = strtoull(sub, NULL, 0);
				}
			} else {
				strncpy(res->disks[d].path, sub, SANLK_PATH_LEN - 1);
			}
		} else {
			rv = sscanf(sub, "%llu", (unsigned long long *)&res->disks[d].offset);
			if (rv != 1)
				goto fail;
			d++;
		}

		sub_count++;
		j = 0;
		memset(sub, 0, sizeof(sub));
	}

	*res_ret = res;
	return 0;

 fail:
	free(res);
	return -1;
}

/*
 * convert from array of struct sanlk_resource * to state string with format:
 * "RESOURCE1 RESOURCE2 RESOURCE3 ..."
 * RESOURCE format in sanlock_res_to_str() comment
 */

int sanlock_args_to_state(int res_count,
			  struct sanlk_resource *res_args[],
			  char **res_state)
{
	char *str, *state;
	int i, rv;

	state = malloc(res_count * (SANLK_MAX_RES_STR + 1));
	if (!state)
		return -ENOMEM;
	memset(state, 0, res_count * (SANLK_MAX_RES_STR + 1));

	for (i = 0; i < res_count; i++) {
		str = NULL;

		rv = sanlock_res_to_str(res_args[i], &str);
		if (rv < 0 || !str) {
			free(state);
			return rv;
		}

		if (strlen(str) > SANLK_MAX_RES_STR - 1) {
			free(str);
			free(state);
			return -EINVAL;
		}

		if (i)
			strcat(state, " ");
		strcat(state, str);
		free(str);
	}

	/* caller to free state */
	*res_state = state;
	return 0;
}

/*
 * convert to array of struct sanlk_resource * from state string with format:
 * "RESOURCE1 RESOURCE2 RESOURCE3 ..."
 * RESOURCE format in sanlock_str_to_res() comment
 */

int sanlock_state_to_args(char *res_state,
			  int *res_count,
			  struct sanlk_resource ***res_args)
{
	struct sanlk_resource **args;
	struct sanlk_resource *res;
	char str[SANLK_MAX_RES_STR + 1];
	int count = 1, arg_count = 0;
	int escape = 0;
	int sep_colons = 0;
	int i, j, len, rv;

	for (i = 0; i < strlen(res_state); i++) {
		if (res_state[i] == '\\') {
			i++;
			continue;
		}
		if (res_state[i] == ' ')
			count++;
	}

	*res_count = count;

	args = malloc(count * sizeof(*args));
	if (!args)
		return -ENOMEM;
	memset(args, 0, count * sizeof(*args));

	j = 0;
	memset(str, 0, sizeof(str));
	sep_colons = 0;

	len = strlen(res_state);

	for (i = 0; i < len + 1; i++) {

		if (i < len && res_state[i] == '\\') {
			str[j++] = res_state[i];
			escape = 1;
			continue;
		}

		if (i < len && escape) {
			str[j++] = res_state[i];
			escape = 0;
			continue;
		}

		if ((i < len) && (res_state[i] == ' ') && (sep_colons < 3)) {
			/*
			 * This is a bit dubious.  It's meant to detect when
			 * a res string contains an unescaped space, and
			 * inserts an escape char before it.  An unescaped
			 * space within a res string would otherwise be
			 * misinterpreted as a separator between res strings.
			 * If we've not yet seen three colons within a single
			 * res string, then we should not be at the end yet.
			 */
			str[j++] = '\\';
			str[j++] = res_state[i];
			continue;
		}

		if (i < len && res_state[i] != ' ') {
			if (res_state[i] == ':')
				sep_colons++;

			str[j++] = res_state[i];
			continue;
		}

		rv = sanlock_str_to_res(str, &res);
		if (rv < 0 || !res)
			goto fail_free;

		if (arg_count == count)
			goto fail_free;

		args[arg_count++] = res;

		j = 0;
		memset(str, 0, sizeof(str));
		sep_colons = 0;
	}

	/* caller to free res_count res and args */
	*res_count = arg_count;
	*res_args = args;
	return 0;

 fail_free:
	for (i = 0; i < count; i++) {
		if (args[i])
			free(args[i]);
	}
	free(args);
	return rv;
}

/*
 * convert to struct sanlk_lockspace from string with format:
 * <lockspace_name>:<host_id>:<path>:<offset>
 */

int sanlock_str_to_lockspace(char *str, struct sanlk_lockspace *ls)
{
	char *host_id = NULL;
	char *path = NULL;
	char *offset = NULL;
	int i;

	if (!str)
		return -EINVAL;

	for (i = 0; i < strlen(str); i++) {
		if (str[i] == '\\') {
			i++;
			continue;
		}

		if (str[i] == ':') {
			if (!host_id)
				host_id = &str[i];
			else if (!path)
				path = &str[i];
			else if (!offset)
				offset = &str[i];
		}
	}

	if (host_id) {
		*host_id = '\0';
		host_id++;
	}
	if (path) {
		*path = '\0';
		path++;
	}
	if (offset) {
		*offset= '\0';
		offset++;
	}

	strncpy(ls->name, str, SANLK_NAME_LEN);

	if (host_id)
		ls->host_id = atoll(host_id);
	if (path)
		sanlock_path_import(ls->host_id_disk.path, path, sizeof(ls->host_id_disk.path));
	if (offset)
		ls->host_id_disk.offset = atoll(offset);

	return 0;
}

