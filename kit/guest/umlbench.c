/*
 * umlbench: time the operations Android leans on, to see where UML spends
 * its time. Static binary; run as root inside Android (it is copied to
 * /dev/uml/umlbench) or natively for comparison.
 *
 *   syscall    getppid() round trip
 *   pipe       two processes passing a byte back and forth (= 2 syscalls
 *              and 2 process switches per round trip)
 *   binder     a Binder transaction and its reply between two processes,
 *              on a private binderfs device (needs root and binderfs)
 *   fault      first touch of an anonymous page
 *   fork       fork() + _exit() + waitpid()
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/android/binder.h>
#include <linux/android/binderfs.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static double now_us(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1e6 + ts.tv_nsec / 1e3;
}

static void report(const char *name, double total_us, long n, const char *what)
{
	printf("BENCH %-8s %9.2f us per %s  (%ld in %.0f ms)\n", name, total_us / n, what,
	       n, total_us / 1000);
	fflush(stdout);
}

static void bench_syscall(long n)
{
	double t = now_us();

	for (long i = 0; i < n; i++)
		syscall(SYS_getppid);
	report("syscall", now_us() - t, n, "call");
}

static long long ts_ns(const struct timespec *t)
{
	return t->tv_sec * 1000000000LL + t->tv_nsec;
}

/* clock_gettime() through libc (the vDSO when there is one), checked
 * against the real system call: the two must agree and never go backwards. */
static void bench_clock(long n)
{
	static const int clocks[] = { CLOCK_MONOTONIC, CLOCK_REALTIME, CLOCK_BOOTTIME };
	struct timespec a, b;
	long backwards = 0;
	long long last = 0, maxdiff = 0;
	double t = now_us();

	for (long i = 0; i < n; i++) {
		clock_gettime(CLOCK_MONOTONIC, &a);
		if (ts_ns(&a) < last)
			backwards++;
		last = ts_ns(&a);
	}
	report("clock", now_us() - t, n, "clock_gettime");
	for (int c = 0; c < 3; c++) {
		for (int i = 0; i < 200; i++) {
			long long d;

			syscall(SYS_clock_gettime, clocks[c], &a);
			clock_gettime(clocks[c], &b);
			d = ts_ns(&b) - ts_ns(&a);
			if (d < 0)
				backwards++;
			if (d < 0)
				d = -d;
			if (d > maxdiff)
				maxdiff = d;
		}
	}
	printf("BENCH clockchk  %ld backwards steps, max libc/syscall gap %.1f us\n",
	       backwards, maxdiff / 1000.0);
	fflush(stdout);
}

static void bench_pipe(long n)
{
	int a[2], b[2];
	char c = 0;
	pid_t pid;
	double t;

	if (pipe(a) || pipe(b))
		return;
	pid = fork();
	if (pid == 0) {
		for (long i = 0; i < n; i++) {
			if (read(a[0], &c, 1) != 1 || write(b[1], &c, 1) != 1)
				_exit(1);
		}
		_exit(0);
	}
	t = now_us();
	for (long i = 0; i < n; i++) {
		if (write(a[1], &c, 1) != 1 || read(b[0], &c, 1) != 1)
			break;
	}
	report("pipe", now_us() - t, n, "round trip");
	waitpid(pid, NULL, 0);
	close(a[0]); close(a[1]); close(b[0]); close(b[1]);
}

/* ---- binder ---- */

struct binder_conn {
	int fd;
	void *map;
};

static int binder_open(const char *dev, struct binder_conn *b)
{
	b->fd = open(dev, O_RDWR | O_CLOEXEC);
	if (b->fd < 0)
		return -1;
	b->map = mmap(NULL, 1 << 20, PROT_READ, MAP_PRIVATE, b->fd, 0);
	return b->map == MAP_FAILED ? -1 : 0;
}

static int binder_rw(struct binder_conn *b, void *wbuf, size_t wlen, void *rbuf, size_t rlen,
		     size_t *got)
{
	struct binder_write_read bwr = {
		.write_buffer = (uintptr_t)wbuf, .write_size = wlen,
		.read_buffer = (uintptr_t)rbuf, .read_size = rlen,
	};
	int r;

	do {
		r = ioctl(b->fd, BINDER_WRITE_READ, &bwr);
	} while (r < 0 && errno == EINTR);
	if (got)
		*got = bwr.read_consumed;
	return r;
}

/* Waits for BR_TRANSACTION or BR_REPLY; returns its data buffer pointer. */
static int binder_wait(struct binder_conn *b, uint32_t want, binder_uintptr_t *buffer)
{
	uint8_t rbuf[256];

	for (;;) {
		size_t got = 0, off = 0;

		if (binder_rw(b, NULL, 0, rbuf, sizeof(rbuf), &got) < 0)
			return -1;
		while (off + 4 <= got) {
			uint32_t cmd;

			memcpy(&cmd, rbuf + off, 4);
			off += 4;
			if (cmd == BR_TRANSACTION || cmd == BR_REPLY) {
				struct binder_transaction_data tr;

				memcpy(&tr, rbuf + off, sizeof(tr));
				off += sizeof(tr);
				if (cmd == want) {
					*buffer = tr.data.ptr.buffer;
					return 0;
				}
			} else if (cmd == BR_NOOP || cmd == BR_TRANSACTION_COMPLETE ||
				   cmd == BR_SPAWN_LOOPER) {
				/* nothing to do */
			} else {
				fprintf(stderr, "binder: unexpected command 0x%x\n", cmd);
				return -1;
			}
		}
	}
}

static void binder_send(struct binder_conn *b, uint32_t cmd, binder_uintptr_t free_buf)
{
	struct send_cmds {
		uint32_t free_cmd;
		binder_uintptr_t free_ptr;
		uint32_t cmd;
		struct binder_transaction_data tr;
	} __attribute__((packed)) w = {
		.free_cmd = BC_FREE_BUFFER, .free_ptr = free_buf, .cmd = cmd,
	};
	size_t skip = free_buf ? 0 : offsetof(struct send_cmds, cmd);

	w.tr.target.handle = 0;
	w.tr.code = 1;
	binder_rw(b, (char *)&w + skip, sizeof(w) - skip, NULL, 0, NULL);
}

static void bench_binder(long n)
{
	const char *ctl = "/dev/binderfs/binder-control", *dev = "/dev/binderfs/umlbench";
	struct binderfs_device bd = { .name = "umlbench" };
	struct binder_conn srv, cli;
	int ready[2], fd;
	char c;
	pid_t pid;
	double t;

	fd = open(ctl, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		printf("BENCH binder   skipped (no %s: %s)\n", ctl, strerror(errno));
		return;
	}
	if (ioctl(fd, BINDER_CTL_ADD, &bd) < 0 && errno != EEXIST) {
		printf("BENCH binder   skipped (cannot create %s: %s)\n", dev, strerror(errno));
		close(fd);
		return;
	}
	close(fd);
	if (pipe(ready))
		return;
	pid = fork();
	if (pid == 0) {
		binder_uintptr_t buf = 0;
		uint32_t enter = BC_ENTER_LOOPER;

		if (binder_open(dev, &srv) || ioctl(srv.fd, BINDER_SET_CONTEXT_MGR, 0) < 0)
			_exit(1);
		binder_rw(&srv, &enter, sizeof(enter), NULL, 0, NULL);
		if (write(ready[1], "r", 1) != 1)
			_exit(1);
		for (long i = 0; i < n; i++) {
			if (binder_wait(&srv, BR_TRANSACTION, &buf))
				_exit(1);
			binder_send(&srv, BC_REPLY, buf);
		}
		_exit(0);
	}
	if (read(ready[0], &c, 1) != 1 || binder_open(dev, &cli)) {
		printf("BENCH binder   skipped (server did not start)\n");
		kill(pid, SIGKILL);
		waitpid(pid, NULL, 0);
		return;
	}
	t = now_us();
	{
		binder_uintptr_t buf = 0;

		for (long i = 0; i < n; i++) {
			binder_send(&cli, BC_TRANSACTION, buf);
			if (binder_wait(&cli, BR_REPLY, &buf))
				break;
		}
	}
	report("binder", now_us() - t, n, "transaction+reply");
	waitpid(pid, NULL, 0);
}

static void bench_fault(long pages)
{
	long ps = sysconf(_SC_PAGESIZE);
	char *p = mmap(NULL, pages * ps, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	double t;

	if (p == MAP_FAILED)
		return;
	t = now_us();
	for (long i = 0; i < pages; i++)
		p[i * ps] = 1;
	report("fault", now_us() - t, pages, "page");
	munmap(p, pages * ps);
}

static void bench_fork(long n)
{
	double t = now_us();

	for (long i = 0; i < n; i++) {
		pid_t pid = fork();

		if (pid == 0)
			_exit(0);
		waitpid(pid, NULL, 0);
	}
	report("fork", now_us() - t, n, "fork+exit+wait");
}

/* ---- off-CPU sampler: umlbench waits <seconds> ---- */
#include <dirent.h>

struct wait_entry { char key[96]; long count; };
static struct wait_entry waits[4096];
static int nwaits;

static void count_wait(const char *key)
{
	for (int i = 0; i < nwaits; i++)
		if (!strcmp(waits[i].key, key)) { waits[i].count++; return; }
	if (nwaits < 4096) {
		snprintf(waits[nwaits].key, sizeof(waits[0].key), "%s", key);
		waits[nwaits++].count = 1;
	}
}

static int read_small(const char *path, char *buf, size_t n)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	ssize_t r;

	if (fd < 0)
		return -1;
	r = read(fd, buf, n - 1);
	close(fd);
	if (r < 0)
		return -1;
	buf[r] = 0;
	while (r > 0 && (buf[r - 1] == '\n' || buf[r - 1] == ' '))
		buf[--r] = 0;
	return 0;
}

static int cmp_wait(const void *a, const void *b)
{
	return ((const struct wait_entry *)b)->count - ((const struct wait_entry *)a)->count;
}

/* Every 20 ms, record for each thread of each non-kernel process its state
 * and kernel wait channel, keyed "process/thread state wchan". */
static void sample_waits(double seconds)
{
	double end = now_us() + seconds * 1e6;
	long samples = 0;

	while (now_us() < end) {
		DIR *proc = opendir("/proc");
		struct dirent *p;

		while (proc && (p = readdir(proc))) {
			char path[128], comm[32], tcomm[32], stat[512], wchan[64], key[160];
			DIR *tasks;
			struct dirent *t;

			if (p->d_name[0] < '1' || p->d_name[0] > '9')
				continue;
			snprintf(path, sizeof(path), "/proc/%s/cmdline", p->d_name);
			if (read_small(path, stat, sizeof(stat)) || !stat[0])
				continue;	/* kernel thread */
			snprintf(path, sizeof(path), "/proc/%s/comm", p->d_name);
			if (read_small(path, comm, sizeof(comm)))
				continue;
			snprintf(path, sizeof(path), "/proc/%s/task", p->d_name);
			tasks = opendir(path);
			while (tasks && (t = readdir(tasks))) {
				char *st;

				if (t->d_name[0] == '.')
					continue;
				snprintf(path, sizeof(path), "/proc/%s/task/%s/stat", p->d_name, t->d_name);
				if (read_small(path, stat, sizeof(stat)))
					continue;
				st = strrchr(stat, ')');
				if (!st || st[1] != ' ')
					continue;
				snprintf(path, sizeof(path), "/proc/%s/task/%s/comm", p->d_name, t->d_name);
				if (read_small(path, tcomm, sizeof(tcomm)))
					tcomm[0] = 0;
				if (st[2] == 'S' || st[2] == 'D') {
					snprintf(path, sizeof(path), "/proc/%s/task/%s/wchan", p->d_name, t->d_name);
					if (read_small(path, wchan, sizeof(wchan)))
						strcpy(wchan, "?");
				} else {
					wchan[0] = 0;
				}
				/* idle sleepers are not interesting; running and waits are */
				if (st[2] == 'S' && (!strcmp(wchan, "do_epoll_wait") ||
						     !strcmp(wchan, "binder_wait_for_work") ||
						     !strcmp(wchan, "futex_wait_queue") ||
						     !strcmp(wchan, "do_sys_poll") ||
						     !strcmp(wchan, "do_select") ||
						     !strcmp(wchan, "hrtimer_nanosleep") ||
						     !strcmp(wchan, "pipe_read") ||
						     !strcmp(wchan, "do_sigtimedwait") ||
						     !strcmp(wchan, "unix_stream_read_generic") ||
						     !strcmp(wchan, "do_wait") ||
						     !strcmp(wchan, "inotify_read") ||
						     !strcmp(wchan, "__skb_wait_for_more_packets")))
					continue;
				snprintf(key, sizeof(key), "%.15s/%.15s %c %s", comm, tcomm, st[2], wchan);
				count_wait(key);
			}
			if (tasks)
				closedir(tasks);
		}
		if (proc)
			closedir(proc);
		samples++;
		usleep(20000);
	}
	qsort(waits, nwaits, sizeof(waits[0]), cmp_wait);
	printf("WAITS %ld samples (20 ms apart); state R = running/runnable\n", samples);
	for (int i = 0; i < nwaits && i < 40; i++)
		printf("WAITS %5ld  %s\n", waits[i].count, waits[i].key);
	fflush(stdout);
}

int main(int argc, char **argv)
{
	if (argc > 2 && !strcmp(argv[1], "waits")) {
		sample_waits(atof(argv[2]));
		return 0;
	}

	long scale = argc > 1 ? atol(argv[1]) : 1;

	if (scale < 1)
		scale = 1;
	bench_syscall(100000 * scale);
	bench_clock(100000 * scale);
	bench_pipe(10000 * scale);
	bench_binder(10000 * scale);
	bench_fault(16384 * scale);
	bench_fork(200 * scale);
	return 0;
}
