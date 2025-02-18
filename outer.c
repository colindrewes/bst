/* Copyright © 2020 Arista Networks, Inc. All rights reserved.
 *
 * Use of this source code is governed by the MIT license that can be found
 * in the LICENSE file.
 */

#include <assert.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/epoll.h>
#include <unistd.h>

#include "capable.h"
#include "enter.h"
#include "outer.h"
#include "path.h"
#include "userns.h"
#include "util.h"
#include "cgroups.h"
#include "fd.h"

enum {
	/* This should be enough for defining our mappings. If we assign
	   340 mappings, and since each line would contain at most
	   12 digits * 3 + 2 spaces + 1 newline, this would take about 13260
	   bytes. */
	ID_MAP_MAX = 4 * 4096,
};

/* burn opens the file pointed by path relative to dirfd, burns in it a
   null-terminated string using exactly one write syscall, then closes the file.

   This helper is useful for writing data in files that can only be written to
   exactly once (hence "burning" rather than "writing"). Such files
   include /proc/pid/uid_map, /proc/pid/gid_map, and /proc/pid/setgroups
   under some circumstances. */
void burn(int dirfd, char *path, char *data)
{
	int fd = openat(dirfd, path, O_WRONLY, 0);
	if (fd == -1) {
		err(1, "burn %s: open", path);
	}

	if (write(fd, data, strlen(data)) == -1) {
		err(1, "burn %s: write", path);
	}

	if (close(fd) == -1) {
		err(1, "burn %s: close", path);
	}
}

static void make_idmap(char *idmap, size_t size, const char *which,
		const char *subid_path,
		const char *procmap_path,
		const struct id *id, id_map desired)
{
	id_map cur_id_map;
	id_map_load_procids(cur_id_map, procmap_path);

	/* /proc/self/[ug]id_map files should be well-formed, but we might as well
	   enforce that rather than blindly trust. */
	id_map_normalize(cur_id_map, true, false);

	id_map subids;
	id_map_load_subids(subids, subid_path, id);
	id_map_normalize(subids, false, true);

	/* Project desired id maps onto permissible maps */
	if (!id_map_empty(desired)) {
		for (struct id_range *r = subids; r < subids + MAX_USER_MAPPINGS; ++r) {
			r->inner = r->outer;
		}

		id_map_normalize(desired, false, true);
		id_map_project(desired, subids, subids);

		uint32_t nids = id_map_count_ids(subids);
		uint32_t desired_ids = id_map_count_ids(desired);
		if (nids == UINT32_MAX || desired_ids == UINT32_MAX) {
			err(1, "too many %ss to map", which);
		}
		if (nids != desired_ids) {
			errx(1, "cannot map desired %s map: some %ss are not in the %ss "
				"allowed in %s", which, which, which, subid_path);
		}
	} else {
		id_map_generate(subids, subids, subid_path, id);
	}

	/* Slice up subid maps according to current id mappings. */
	id_map_project(subids, cur_id_map, subids);

	id_map_format(subids, idmap, size);
}

static void burn_uidmap_gidmap(pid_t child_pid, id_map uid_desired, id_map gid_desired)
{
	char procpath[PATH_MAX];
	if ((size_t) snprintf(procpath, PATH_MAX, "/proc/%d", child_pid) >= sizeof (procpath)) {
		errx(1, "/proc/%d takes more than PATH_MAX bytes.", child_pid);
	}

	int procfd = open(procpath, O_DIRECTORY | O_PATH);
	if (procfd == -1) {
		err(1, "open %s", procpath);
	}

	struct id uid = id_load_user(getuid());
	struct id gid = id_load_group(getgid());

	char uid_map[ID_MAP_MAX];
	make_idmap(uid_map, sizeof (uid_map), "uid", "/etc/subuid", "/proc/self/uid_map", &uid, uid_desired);

	char gid_map[ID_MAP_MAX];
	make_idmap(gid_map, sizeof (gid_map), "gid", "/etc/subgid", "/proc/self/gid_map", &gid, gid_desired);

	make_capable(BST_CAP_SETUID | BST_CAP_SETGID | BST_CAP_DAC_OVERRIDE);

	burn(procfd, "uid_map", uid_map);
	burn(procfd, "gid_map", gid_map);

	reset_capabilities();
}

static void create_nics(pid_t child_pid, struct nic_options *nics, size_t nnics)
{
	make_capable(BST_CAP_NET_ADMIN);

	int rtnl = init_rtnetlink_socket();

	for (size_t i = 0; i < nnics; ++i) {
		nics[i].netns_pid = child_pid;
		net_if_add(rtnl, &nics[i]);
	}

	reset_capabilities();
}

static void persist_ns_files(pid_t pid, const char **persist)
{
	for (enum nstype ns = 0; ns < MAX_NS; ++ns) {
		if (persist[ns] == NULL) {
			continue;
		}

		const char *name = ns_name(ns);

		if (mknod(persist[ns], S_IFREG, 0) == -1 && errno != EEXIST) {
			err(1, "create %s", persist[ns]);
		}

		char procpath[PATH_MAX];
		makepath_r(procpath, "/proc/%d/ns/%s", pid, name);

		make_capable(BST_CAP_SYS_ADMIN | BST_CAP_SYS_PTRACE);

		int rc = mount(procpath, persist[ns], "", MS_BIND, "");

		reset_capabilities();

		if (rc == -1) {
			unlink(persist[ns]);

			switch errno {
			case ENOENT:
				/* Kernel does not support this namespace type. */
				break;
			case EINVAL:
				errx(1, "bind-mount %s to %s: %s (is the destination on a private mount?)",
						procpath, persist[ns], strerror(EINVAL));
			default:
				err(1, "bind-mount %s to %s", procpath, persist[ns]);
			}
		}
	}
}

/*
 * If bst has entered a cgroup this function will epoll the cgroup.events file
 * to detect when all pids have exited the cgroup ("populated 0"). The cgroup is 
 * destroyed when this condition is met.
 */
static void cgroup_helper(int cgroupfd, pid_t rootpid)
{
	// Create a new session in case current group leader is killed
	if (setsid() == -1) {
		err(1, "unable to create new session leader for cgroup cleanup process");
	}

	char *subcgroup = makepath("bst.%d", rootpid);

	int subcgroupfd = openat(cgroupfd, subcgroup, O_DIRECTORY);
	if (subcgroupfd == -1) {
		err(1, "unable to open bst.%d", rootpid);
	}

	int cevent = openat(subcgroupfd, "cgroup.events", 0);
	if (cevent == -1) {
		err(1, "unable to open cgroup.events");
	}

	/* Use EPOLLET to be notified of any changes to the cgroup.events file without
	   needing to seek through the entire file (which seems problematic with this
	   kernel interface).*/
	struct epoll_event event = {
		.events = EPOLLET,
		.data.fd = 0
	};

	int epollfd = epoll_create1(0);
	if (epollfd == -1) {
		err(1, "epoll_create1");
	}

	// Notify about I/O changes to cgroupfd
	if (epoll_ctl(epollfd, EPOLL_CTL_ADD, cevent, &event) == -1) {
		err(1, "epoll_ctl_add cgroupfd");
	}

	char populated[BUFSIZ];
	for (;;) {
		// Single event (update cgroup.procs), block indefinitely
		int ready = epoll_wait(epollfd, &event, 1, -1);
		if (ready == -1) {
			err(1, "epoll_wait cgroup.events");
		}

		// We need a new fd to read from the cgroup.events
		int eventsfd = openat(subcgroupfd, "cgroup.events", O_RDONLY);
		if (eventsfd == -1) {
			err(1,"unable to open cgroup.events");
		}

		FILE *eventsfp = fdopen(eventsfd, "r");
		if (eventsfp == NULL) {
			err(1, "unable to open file pointer to cgroup.events");
		}

		// The order of elements in cgroup.events is not neccesarily specified 
		while (fgets(populated, BUFSIZ, eventsfp) != NULL) {
			if (strnlen(populated, sizeof(populated)) == sizeof(populated)) {
				err(1, "exceeded cgroup.events line read buffer");
			}
			// Check if there are no procs within the bst cgroup -- if so, delete it
			if (strncmp(populated, "populated 0", 11) == 0) {
				cgroup_clean(cgroupfd, rootpid);
				close(subcgroupfd);
				close(cevent);
				fclose(eventsfp);
				return;
			}
		}

		fclose(eventsfp);
	}
}

/* outer_helper_spawn spawns a new process whose only purpose is to modify
   the uid and gid mappings of our target process (TP).

   The outer helper thus runs as a sibling of the TP, and provides some basic
   synchronization routines to make sure the TP waits for its sibling to complete
   before calling setgroups/setgid/setuid.

   The reason why this helper is necessary is because once we enter the user
   namespace, we drop CAP_SET[UG]ID on the host namespace, which means we
   can't map arbitrary sub[ug]id ranges. We could setuid bst itself and
   do these mappings from a regular fork(), but this means that we can no
   longer do the right thing w.r.t unprivileged user namespaces, not to mention
   that I'm not happy with having a rootkit that everyone can use on my own
   machine.

   The canonical way to do all of this on a modern Linux distribution is to
   call the newuidmap and newgidmap utilities, which are generic interfaces
   that do exactly what bst--outer-helper does, which is writing to
   /proc/pid/[ug]id_map any id ranges that a user is allowed to map by looking
   allocated IDs for that user in /etc/sub[ug]id. We obviously don't want
   to rely on any external program that may or may not be installed on the
   host system, so we reimplement that functionality here. */
void outer_helper_spawn(struct outer_helper *helper)
{
	enum {
		SOCKET_PARENT,
		SOCKET_CHILD,
	};
	int fdpair[2];
	if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fdpair) == -1) {
		err(1, "outer_helper: socketpair");
	}

	pid_t rootpid = getpid();

	pid_t pid = fork();
	if (pid == -1) {
		err(1, "outer_helper: fork");
	}

	if (pid) {
		close(fdpair[SOCKET_CHILD]);
		helper->pid = pid;
		helper->fd  = fdpair[SOCKET_PARENT];
		return;
	}

	if (helper->cgroup_enabled) {
		int cgroupfd = recv_fd(fdpair[SOCKET_CHILD]);
		pid_t pid = fork();
		if (pid == -1) {
			err(1, "outer_helper: cgroup cleanup fork");
		}

		/* This process is intentionally left to leak as the bst root process must have exited
			 and thus been removed from bst's cgroup.procs for the cgroup hierarchy to be removed */
		if (pid == 0) {

			// If cleanup is needed, fork process to epoll cgroup.events
			if (cgroupfd != -1) {
				cgroup_helper(cgroupfd, rootpid);
				_exit(0);
			}
		}
	}

	if (prctl(PR_SET_PDEATHSIG, SIGKILL) == -1) {
		err(1, "prctl PR_SET_PDEATHSIG");
	}

	sigset_t mask;
	sigemptyset(&mask);

	if (sigprocmask(SIG_SETMASK, &mask, NULL) == -1) {
		err(1, "sigprocmask");
	}

	close(fdpair[SOCKET_PARENT]);
	int fd = fdpair[SOCKET_CHILD];

	pid_t child_pid;
	ssize_t rdbytes = read(fd, &child_pid, sizeof (child_pid));
	if (rdbytes == -1) {
		err(1, "outer_helper: read child pid");
	}

	/* This typically happens when the parent dies, e.g. Ctrl-C. Not worth
	   warning against. */
	if (rdbytes != sizeof (child_pid)) {
		_exit(1);
	}

	if (helper->unshare_user) {
		burn_uidmap_gidmap(child_pid, helper->uid_desired, helper->gid_desired);
	}

	persist_ns_files(child_pid, helper->persist);

	if (helper->unshare_net) {
		create_nics(child_pid, helper->nics, helper->nnics);
	}

	/* Notify sibling that we're done persisting their proc files
	   and/or changing their [ug]id map */
	int ok = 1;
	ssize_t count = write(fd, &ok, sizeof (ok));
	assert((ssize_t)(sizeof (ok)) == count);

	_exit(0);
}

void outer_helper_sendpid(const struct outer_helper *helper, pid_t pid)
{
	/* Unblock the privileged helper to set our own [ug]id maps */
	if (write(helper->fd, &pid, sizeof (pid)) == -1) {
		err(1, "outer_helper_sendpid_and_wait: write");
	}
}

void outer_helper_sync(const struct outer_helper *helper)
{
	int ok;
	switch (read(helper->fd, &ok, sizeof (ok))) {
	case -1:
		err(1, "outer_helper_wait: read");
	case 0:
		/* Outer helper died before setting all of our attributes. */
		exit(1);
	}
}

void outer_helper_close(struct outer_helper *helper)
{
	close(helper->fd);
}
