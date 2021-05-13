/* Copyright © 2021 Arista Networks, Inc. All rights reserved.
 *
 * Use of this source code is governed by the MIT license that can be found
 * in the LICENSE file.
 */

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pty.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>
#include <util.h>

#include "sig.h"
#include "tty.h"

void recv_fd(int socket, int *pFd) {
	char buf[1];
	struct iovec iov[1] = {
		[0] = {.iov_base = buf, .iov_len = 1 }
	};
	union {
		struct cmsghdr _align;
		char ctrl[CMSG_SPACE(sizeof(int))];
	} uCtrl;
	struct msghdr msg = {
		.msg_control = uCtrl.ctrl,
		.msg_controllen = sizeof(uCtrl.ctrl),
		.msg_name = NULL,
		.msg_namelen = 0,
		.msg_iov = iov,
		.msg_iovlen = 1,
	};

	if ((recvmsg(socket, &msg, 0)) <= 0) {
		err(1, "recv_fd: recvmsg");
	}

	struct cmsghdr *pCm;
	if (((pCm = CMSG_FIRSTHDR(&msg)) != NULL) &&
		 pCm->cmsg_len == CMSG_LEN(sizeof(int))) {
		if (pCm->cmsg_level != SOL_SOCKET) {
			errx(1, "recv_fd: control level != SOL_SOCKET");
		}
		if (pCm->cmsg_type != SCM_RIGHTS) {
			errx(1, "recv_fd: control type != SCM_RIGHTS");
		}
		*pFd = *((int*) CMSG_DATA(pCm));
	} else {
		errx(1, "recv_fd: no descriptor passed");
	}
}

void send_fd(int socket, int fd) {
	char buf[1] = {0};
	struct iovec iov[1] = {
		[0] = {.iov_base = buf, .iov_len = 1 }
	};
	union {
		struct cmsghdr _align;
		char ctrl[CMSG_SPACE(sizeof(int))];
	} uCtrl;
	struct msghdr msg = {
		.msg_control = uCtrl.ctrl,
		.msg_controllen = sizeof(uCtrl.ctrl),
		.msg_name = NULL,
		.msg_namelen = 0,
		.msg_iov = iov,
		.msg_iovlen = 1,
	};
	struct cmsghdr *pCm = CMSG_FIRSTHDR(&msg);
	pCm->cmsg_len = CMSG_LEN(sizeof(int));
	pCm->cmsg_level = SOL_SOCKET;
	pCm->cmsg_type = SCM_RIGHTS;
	*((int*) CMSG_DATA(pCm)) = fd;
	if (sendmsg(socket, &msg, 0) < 0) {
		err(1, "send_fd: sendmsg");
	}
}

#define R_NFDS 5
#define R_STDIN 0
#define R_TERM 1
#define R_SIG 2
#define R_INPIPE 3
#define R_OUTPIPE 4

#define W_NFDS 4
#define W_STDOUT 0
#define W_TERM 1
#define W_INPIPE 2
#define W_OUTPIPE 3

static struct tty_parent_info_s {
	int termfd;
	int sigfd;
	int inpipe[2];
	int outpipe[2];
	struct pollfd rfds[R_NFDS];
	struct pollfd wfds[W_NFDS];
	struct termios orig;
	bool stdinIsatty;
} info = {
	.termfd = -1,
	.sigfd = -1,
	.rfds = {
		[R_STDIN] = {
			.fd = STDIN_FILENO,
			.events = POLLIN,
		},
		[R_TERM] = {
			.events = POLLIN,
		},
		[R_SIG] = {
			.events = POLLIN,
		},
		[R_INPIPE] = {
			.events = POLLIN,
		},
		[R_OUTPIPE] = {
			.events = POLLIN,
		},
	},
	.wfds = {
		[W_STDOUT] = {
			.fd = STDOUT_FILENO,
			.events = POLLOUT,
		},
		[W_TERM] = {
			.events = POLLOUT,
		},
		[W_INPIPE] = {
			.events = POLLOUT,
		},
		[W_OUTPIPE] = {
			.events = POLLOUT,
		},
	},
};

void tty_setup_socketpair(int *pParentSock, int *pChildSock) {
	int socks[2];
	if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, socks) < 0) {
		err(1, "tty_setup: socketpair");
	}
	*pParentSock = socks[0];
	*pChildSock = socks[1];
}

void tty_parent_cleanup() {
	if (info.termfd >= 0) {
		close(info.termfd);
	}
	if (info.stdinIsatty) {
		tcsetattr(STDIN_FILENO, TCSADRAIN, &info.orig);
	}
}

void tty_set_winsize() {
	struct winsize wsize;
	if (ioctl(STDIN_FILENO, TIOCGWINSZ, (char*) &wsize) < 0) {
		err(1, "reading window size");
	}
	if (ioctl(info.termfd, TIOCSWINSZ, (char*) &wsize) < 0) {
		err(1, "writing window size");
	}
}

bool tty_handle_sig(siginfo_t *siginfo) {
	switch (siginfo->si_signo) {
	case SIGWINCH:
		if (!info.stdinIsatty) return false;
		tty_set_winsize();
		return true;
	}
	return false;
}

bool tty_parent_select(pid_t pid) {
	const size_t buflen = 1024;
	bool rtn = false;

	int rc = poll(info.rfds, R_NFDS, -1);
	if (rc == 0) {
		return false;
	}
	if (rc < 0) {
		if (errno == EINTR) {
			return false;
		}
		err(1, "select");
	}
	if (poll(info.wfds, W_NFDS, 0) <= 0) {
		return false;
	}
	if ((info.rfds[R_STDIN].revents & POLLIN) && (info.wfds[W_INPIPE].revents & POLLOUT)) {
		ssize_t nread = splice(STDIN_FILENO, NULL, info.inpipe[1], NULL, buflen, 0);
		if (nread <= 0) {
			if (nread < 0) {
				warn("reading from stdin");
			}
			info.rfds[R_STDIN].revents &= ~POLLIN;
			info.rfds[W_INPIPE].revents &= ~POLLOUT;
			close(info.inpipe[1]);
		}
		return false;
	}
	if ((info.rfds[R_INPIPE].revents & POLLIN) && (info.wfds[W_TERM].revents & POLLOUT)) {
		ssize_t nread = splice(info.inpipe[0], NULL, info.termfd, NULL, buflen, 0);
		if (nread <= 0) {
			if (nread < 0) {
				warn("reading from inpipe");
			}
			info.rfds[R_INPIPE].revents &= ~POLLIN;
			info.rfds[W_TERM].revents &= ~POLLOUT;
			if (write(info.inpipe[1], &(char){4}, 1) < 0) {
				warn("writing EOT to terminal");
			}
		}
	}
	if ((info.rfds[R_TERM].revents & POLLIN) && (info.wfds[W_OUTPIPE].revents & POLLOUT)) {
		ssize_t nread = splice(info.termfd, NULL, info.outpipe[1], NULL, buflen, 0);
		if (nread <= 0) {
			if (nread < 0 && errno != EIO) {
				warn("reading from terminal");
			}
			info.rfds[R_TERM].revents &= ~POLLIN;
			info.rfds[W_OUTPIPE].revents &= ~POLLOUT;
			close(info.outpipe[1]);
		}
		return false;
	}
	if ((info.rfds[R_OUTPIPE].revents & POLLIN) && (info.wfds[W_STDOUT].revents & POLLOUT)) {
		ssize_t nread = splice(info.outpipe[0], NULL, STDOUT_FILENO, NULL, buflen, 0);
		if (nread <= 0) {
			if (nread < 0) {
				warn("reading from outpipe");
			}
			info.rfds[R_OUTPIPE].revents &= ~POLLIN;
			info.rfds[W_STDOUT].revents &= ~POLLOUT;
		}
	}
	if (info.rfds[R_SIG].revents & POLLIN) {
		struct signalfd_siginfo sigfd_info;
		if (read(info.sigfd, &sigfd_info, sizeof(sigfd_info)) == sizeof(sigfd_info)) {
			siginfo_t siginfo;
			siginfo.si_signo = (int) sigfd_info.ssi_signo;
			siginfo.si_code = sigfd_info.ssi_code;
			if (!tty_handle_sig(&siginfo)) {
				sig_forward(&siginfo, pid);
			}
			rtn = (sigfd_info.ssi_signo == SIGCHLD);
		}
	}
	return rtn;
}

void tty_parent_setup(int socket) {
	// Put the parent's stdin in raw mode, except add CRLF handling.
	struct termios tios;
	if ((info.stdinIsatty = isatty(STDIN_FILENO))) {
		if (tcgetattr(STDIN_FILENO, &tios) < 0) {
			err(1, "tty_parent: tcgetattr");
		}
		info.orig = tios;
		cfmakeraw(&tios);
		// keep c_oflag the same
		tios.c_oflag = info.orig.c_oflag;
		if (tcsetattr(STDIN_FILENO, TCSANOW, &tios) < 0) {
			err(1, "tty_parent: tcsetattr");
		}
	}
	atexit(tty_parent_cleanup);

	// Wait for the child to create the pty pair and pass the master back.
	// Turn off CRLF handling since that gives us ^Ms in output.
	recv_fd(socket, &info.termfd);
	if (tcgetattr(info.termfd, &tios) < 0) {
		err(1, "tty_parent: tcgetattr");
	}
	tios.c_oflag &= ~((tcflag_t){OPOST});
	if (tcsetattr(info.termfd, TCSAFLUSH, &tios) < 0) {
		err(1, "tty_parent: tcsetattr");
	}

	sigset_t sigmask;
	sigfillset(&sigmask);
	if (sigprocmask(SIG_BLOCK, &sigmask, NULL) < 0) {
		err(1, "tty_parent: sigprocmask");
	}
	if ((info.sigfd = signalfd(-1, &sigmask, 0)) < 0) {
		err(1, "tty_parent: signalfd");
	}

	if (pipe(info.inpipe) < 0) {
		err(1, "tty_parent: pipe(inpipe)");
	}
	if (pipe(info.outpipe) < 0) {
		err(1, "tty_parent: pipe(outpipe)");
	}
	fcntl(STDOUT_FILENO, F_SETFL, fcntl(STDOUT_FILENO, F_GETFL) & ~O_APPEND);

	info.rfds[R_TERM].fd = info.wfds[W_TERM].fd = info.termfd;
	info.rfds[R_SIG].fd = info.sigfd;
	info.rfds[R_INPIPE].fd = info.inpipe[0];
	info.wfds[W_INPIPE].fd = info.inpipe[1];
	info.rfds[R_OUTPIPE].fd = info.outpipe[0];
	info.wfds[W_OUTPIPE].fd = info.outpipe[1];

	if (info.stdinIsatty) {
		tty_set_winsize();
	}
}

void tty_child(int socket) {
	int mfd = open("/dev/pts/ptmx", O_RDWR);
	if (mfd < 0) {
		err(1, "tty_child: open ptmx");
	}
	int unlock = 0;
	if (ioctl(mfd, TIOCSPTLCK, &unlock) < 0) {
		err(1, "tty_child: ioctl(TIOCSPTLCK)");
	}
	int sfd = ioctl(mfd, TIOCGPTPEER, O_RDWR);
	if (sfd < 0) {
		err(1, "tty_child: ioctl(TIOCGPTPEER)");
	}
	send_fd(socket, mfd);
	close(mfd);

	setsid();
	if (ioctl(sfd, TIOCSCTTY, NULL) < 0) {
		err(1, "tty_child: ioctl(TIOCSCTTY)");
	}
	if (dup2(sfd, STDIN_FILENO) < 0) {
		err(1, "tty_child: dup2(stdin)");
	}
	if (dup2(sfd, STDOUT_FILENO) < 0) {
		err(1, "tty_child: dup2(stdout)");
	}
	if (dup2(sfd, STDERR_FILENO) < 0) {
		err(1, "tty_child: dup2(stderr)");
	}
	if (sfd > STDERR_FILENO) {
		close(sfd);
	}
}
