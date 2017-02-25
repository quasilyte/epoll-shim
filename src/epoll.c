#include <sys/epoll.h>

#include <sys/types.h>

#include <sys/event.h>
#include <sys/param.h>
#include <sys/queue.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int
epoll_create(int size __unused)
{
	fprintf(stderr,
	    "ERROR: epoll_create() is deprecated, use "
	    "epoll_create1(EPOLL_CLOEXEC).\n");
	errno = EINVAL;
	return -1;
}

int
epoll_create1(int flags)
{
	if (flags != EPOLL_CLOEXEC) {
		fprintf(stderr, "ERROR: Use epoll_create1(EPOLL_CLOEXEC).\n");
		errno = EINVAL;
		return -1;
	}
	return kqueue();
}

static int poll_fd = -1;
static int poll_epoll_fd = -1;
static void *poll_ptr;

int
epoll_ctl(int fd, int op, int fd2, struct epoll_event *ev)
{
	if ((!ev && op != EPOLL_CTL_DEL) ||
	    (ev &&
		((ev->events & ~(EPOLLIN | EPOLLOUT | EPOLLHUP | EPOLLERR))
		    /* the user should really set one of EPOLLIN or EPOLLOUT
		     * such that EPOLLHUP and EPOLLERR work. Don't make this a
		     * hard error for now, though. */
		    /* || !(ev->events & (EPOLLIN | EPOLLOUT)) */))) {
		errno = EINVAL;
		return -1;
	}

	struct kevent kev[3];

	if (op == EPOLL_CTL_ADD) {
		/* Check if the fd already has been registered in this kqueue.
		 * See below for an explanation of this 'cookie' mechanism. */
		EV_SET(&kev[0], fd2, EVFILT_USER, 0, 0, 0, 0);
		if (!(kevent(fd, kev, 1, NULL, 0, NULL) == -1 &&
			errno == ENOENT)) {
			errno = EEXIST;
			return -1;
		}

		EV_SET(&kev[0], fd2, EVFILT_READ,
		    EV_ADD | (ev->events & EPOLLIN ? 0 : EV_DISABLE), 0, 0,
		    ev->data.ptr);
		EV_SET(&kev[1], fd2, EVFILT_WRITE,
		    EV_ADD | (ev->events & EPOLLOUT ? 0 : EV_DISABLE), 0, 0,
		    ev->data.ptr);
		/* We save a 'cookie' knote inside the kq to signal if the fd
		 * has been 'registered'. We need this because there is no way
		 * to ask a kqueue if a knote has been registered without
		 * modifying the udata. */
		EV_SET(&kev[2], fd2, EVFILT_USER, EV_ADD, 0, 0, 0);
	} else if (op == EPOLL_CTL_DEL) {
		if (poll_fd == fd2 && fd == poll_epoll_fd) {
			poll_fd = -1;
			poll_epoll_fd = -1;
			poll_ptr = NULL;
			return 0;
		}

		EV_SET(&kev[0], fd2, EVFILT_READ, EV_DELETE, 0, 0, 0);
		EV_SET(&kev[1], fd2, EVFILT_WRITE, EV_DELETE, 0, 0, 0);
		EV_SET(&kev[2], fd2, EVFILT_USER, EV_DELETE, 0, 0, 0);
	} else if (op == EPOLL_CTL_MOD) {
		EV_SET(&kev[0], fd2, EVFILT_READ,
		    ev->events & EPOLLIN ? EV_ENABLE : EV_DISABLE, 0, 0,
		    ev->data.ptr);
		EV_SET(&kev[1], fd2, EVFILT_WRITE,
		    ev->events & EPOLLOUT ? EV_ENABLE : EV_DISABLE, 0, 0,
		    ev->data.ptr);
		/* we don't really need this, but now we have 3 kevents in all
		 * branches which is nice */
		EV_SET(&kev[2], fd2, EVFILT_USER, 0, NOTE_FFCOPY, 0, 0);
	} else {
		errno = EINVAL;
		return -1;
	}

	for (int i = 0; i < 3; ++i) {
		kev[i].flags |= EV_RECEIPT;
	}

	int ret = kevent(fd, kev, 3, kev, 3, NULL);
	if (ret == -1) {
		return -1;
	}

	if (ret != 3) {
		errno = EINVAL;
		return -1;
	}

	for (int i = 0; i < 3; ++i) {
		if (kev[i].flags != EV_ERROR) {
			errno = EINVAL;
			return -1;
		}

		if (kev[i].data == ENODEV && poll_fd == -1) {
			poll_fd = fd2;
			poll_epoll_fd = fd;
			poll_ptr = ev->data.ptr;
			return 0;
		}

		/* ignore EVFILT_WRITE registration EINVAL errors (some fd
		 * types such as kqueues themselves don't support it) */
		if (i == 1 && kev[i].data == EINVAL) {
			continue;
		}

		if (kev[i].data != 0) {
			errno = kev[i].data;
			return -1;
		}
	}

	return 0;
}

#if 0
int
epoll_pwait(
    int fd, struct epoll_event *ev, int cnt, int to, const sigset_t *sigs)
{
	int r = __syscall(SYS_epoll_pwait, fd, ev, cnt, to, sigs, _NSIG / 8);
#ifdef SYS_epoll_wait
	if (r == -ENOSYS && !sigs)
		r = __syscall(SYS_epoll_wait, fd, ev, cnt, to);
#endif
	return __syscall_ret(r);
}
#endif

int
epoll_wait(int fd, struct epoll_event *ev, int cnt, int to)
{
	if (cnt < 1 || cnt > 32) {
		errno = EINVAL;
		return -1;
	}

	if (poll_fd != -1 && fd == poll_epoll_fd) {
		struct pollfd pfds[2];
		pfds[0].fd = poll_fd;
		pfds[0].events = POLLIN;
		pfds[1].fd = fd;
		pfds[1].events = POLLIN;
		int ret = poll(pfds, 2, to);
		if (ret <= 0) {
			return ret;
		}
		if (pfds[0].revents & POLLIN) {
			ev[0].events = EPOLLIN;
			ev[0].data.ptr = poll_ptr;
			return 1;
		}
		to = 0;
	}

	struct kevent evlist[cnt];
	memset(evlist, 0, sizeof(evlist));

	struct timespec timeout = {0, 0};
	if (to > 0) {
		timeout.tv_sec = to / 1000;
		timeout.tv_nsec = (to % 1000) * 1000 * 1000;
	}

	struct timespec *ptimeout = NULL;
	if (to >= 0) {
		ptimeout = &timeout;
	}

	int ret = kevent(fd, NULL, 0, evlist, cnt, ptimeout);
	if (ret == -1) {
		return ret;
	}

	for (int i = 0; i < ret; ++i) {
		int events = 0;
		if (evlist[i].filter == EVFILT_READ) {
			events |= EPOLLIN;
		} else if (evlist[i].filter == EVFILT_WRITE) {
			events |= EPOLLOUT;
		}

		if (evlist[i].flags & EV_ERROR) {
			events |= EPOLLERR;
		}

		if (evlist[i].flags & EV_EOF) {
			// socket errors live in fflags when EV_EOF is set
			if (evlist[i].fflags) {
				events |= EPOLLERR;
			}

			// if we got an EV_EOF, we _may_ have a POLLHUP
			// condition, need to check
			struct pollfd fds[1];
			fds[0].fd = evlist[i].ident;
			fds[0].events = 0;
			if (poll(fds, 1, 0) == -1) {
				// should not really happen
				return -1;
			}

			int hup_event = (fds[0].revents & (POLLHUP | POLLNVAL))
			    ? EPOLLHUP
			    : 0;

			if (hup_event) {
				// fprintf(stderr, "got fflags: %d\n",
				// evlist[i].fflags);

				int old_errno = errno;
				struct stat statbuf;
				int fstat_ret =
				    fstat(evlist[i].ident, &statbuf);
				errno = old_errno;

				if (fstat_ret == 0 &&
				    S_ISFIFO(statbuf.st_mode)) {
					if (evlist[i].filter == EVFILT_READ) {
						if (!evlist[i].data) {
							events &= ~EPOLLIN;
						}
					} else if (evlist[i].filter ==
					    EVFILT_WRITE) {
						hup_event = EPOLLERR;
					}
				}

				events |= hup_event;
			}
		}
		ev[i].events = events;
		ev[i].data.ptr = evlist[i].udata;
	}
	return ret;
}
