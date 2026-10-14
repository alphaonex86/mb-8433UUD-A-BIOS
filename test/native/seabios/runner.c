/* Runs every case of every area in its own forked process (pristine BIOS state, a crash only kills that
   case), one worker per CPU. A case passes, fails (wrong behaviour), hangs (virtual time budget, or 2 s of
   CPU without I/O) or crashes (signal, ASan/UBSan). SeaBIOS's printf and snprintf replace libc's here
   (seams.c), so this file prints with fprintf. usage: devfuzz [-v] [-a area] [-o failures.txt] [area index] */
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "devfuzz.h"

/* Batch runs skip symbolizing (slow); -v re-runs itself with stack traces (see main). */
const char *__asan_default_options(void)
{
	return "abort_on_error=1:detect_leaks=0:handle_segv=0:allow_user_segv_handler=1:print_summary=0:symbolize=0";
}

const char *__ubsan_default_options(void)
{
	return "abort_on_error=1:print_stacktrace=0";
}

static const struct fuzz_area *const Areas[] = { &fuzz_ata, &fuzz_fdc, &fuzz_pcirom, &fuzz_isa, &fuzz_usb, &fuzz_usbhc, &fuzz_cmos_area,
						 &fuzz_pnp, &fuzz_memtest };
#define NAREAS (int)(sizeof(Areas) / sizeof(Areas[0]))
#define MAX_CLASSES 64

unsigned long long fuzz_now_us, fuzz_budget_us = 300000000, fuzz_post_us;
int fuzz_verbose;
static unsigned Rng = 1;

unsigned fuzz_rand(void)
{
	Rng ^= Rng << 13;
	Rng ^= Rng >> 17;
	Rng ^= Rng << 5;
	return Rng;
}

void fuzz_seed(unsigned seed)
{
	Rng = seed * 2654435761u + 1;
	if (!Rng)
		Rng = 1;
}

void fuzz_exit(int status, const char *fmt, va_list args)
{
	fprintf(stdout, "R %d %llu ", status, fuzz_post_us ?: fuzz_now_us);
	vfprintf(stdout, fmt, args);
	fputc('\n', stdout);
	fflush(stdout);
	_exit(0);
}

void fuzz_fail(const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	fuzz_exit(FUZZ_FAIL, fmt, args);
}

void fuzz_hang(const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	fuzz_exit(FUZZ_HANG, fmt, args);
}

void fuzz_note(const char *fmt, ...)
{
	va_list args;
	if (!fuzz_verbose)
		return;
	va_start(args, fmt);
	vfprintf(stdout, fmt, args);
	fputc('\n', stdout);
	va_end(args);
}

void __sanitizer_print_stack_trace(void);

void fuzz_tick(unsigned long long us)
{
	fuzz_now_us += us;
	if (fuzz_now_us <= fuzz_budget_us)
		return;
	if (fuzz_verbose)
		__sanitizer_print_stack_trace();
	fuzz_hang("still running after %llu s of virtual time", fuzz_budget_us / 1000000);
}

/* The BIOS may read any physical address (ROM BARs, garbage pointers): unmapped memory floats (FFh). */
static void on_signal(int sig, siginfo_t *info, void *context)
{
	unsigned long addr = (unsigned long)info->si_addr;
	if (sig == SIGSEGV && info->si_code == SEGV_MAPERR && addr >= FUZZ_PHYS_END) {
		void *page = (void *)(addr & ~0xfffUL);
		if (mmap(page, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0)
		    == page) {
			memset(page, 0xff, 4096);
			return;
		}
	}
	if (fuzz_verbose && sig == SIGPROF)
		__sanitizer_print_stack_trace();
	char msg[96];
	int len = snprintf(msg, sizeof(msg), "\nX %s at address %08lx\n",
			   sig == SIGPROF ? "endless loop without I/O (0.2 s of CPU)" : strsignal(sig), addr);
	if (write(1, msg, len) < 0)
		_exit(3);
	signal(sig, SIG_DFL);
	raise(sig);
}

void fuzz_map_phys(unsigned base, unsigned size)
{
	void *at = (void *)base;
	if (mmap(at, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0) != at)
		fuzz_fail("harness: cannot map physical %08x", base);
}

/* More CPU for the rest of the case: microseconds (the setup's disk benchmark reads megabytes through the models). */
void fuzz_cpu_allow(unsigned us)
{
	struct itimerval cpu_limit = { .it_value = { us / 1000000, us % 1000000 } };
	if (!fuzz_verbose && setitimer(ITIMER_PROF, &cpu_limit, NULL))
		perror("setitimer");
}

static void run_case(const struct fuzz_area *area, unsigned index)
{
	static const int sigs[] = { SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGPROF };
	struct sigaction sa = { .sa_sigaction = on_signal, .sa_flags = SA_SIGINFO };
	struct itimerval cpu_limit = { .it_value = { 0, 200000 } };   /* a case takes a few ms of CPU */
	for (unsigned i = 0; i < sizeof(sigs) / sizeof(sigs[0]); i++)
		if (sigaction(sigs[i], &sa, NULL)) {
			perror("sigaction");
			_exit(3);
		}
	if (fuzz_verbose)
		cpu_limit.it_value = (struct timeval){ 2, 0 };
	if (setitimer(ITIMER_PROF, &cpu_limit, NULL)) {
		perror("setitimer");
		_exit(3);
	}
	fuzz_seed(index);
	area->run(index);
	fprintf(stdout, "R %d %llu\n", FUZZ_PASS, fuzz_post_us ?: fuzz_now_us);
	fflush(stdout);
	_exit(0);
}

enum { PASS, FAIL, HANG, CRASH, KINDS };
static const char *const KindNames[KINDS] = { "pass", "wrong behaviour", "hang", "crash" };

struct stats {
	unsigned cases[NAREAS][MAX_CLASSES], kinds[NAREAS][MAX_CLASSES][KINDS], example[NAREAS][MAX_CLASSES][KINDS];
	unsigned long long max_us[NAREAS][MAX_CLASSES];
	unsigned max_index[NAREAS][MAX_CLASSES];
};

/* Kind and message of a finished child from its output and wait status. */
static int child_result(char *out, int status, char *msg, int size, unsigned long long *us)
{
	char *r = strstr(out, "R ");
	while (r && r != out && r[-1] != '\n')
		r = strstr(r + 1, "R ");
	int kind = CRASH, n = 0;
	*us = 0;
	if (r && sscanf(r, "R %d %llu %n", &kind, us, &n) >= 2 && WIFEXITED(status) && !WEXITSTATUS(status)) {
		snprintf(msg, size, "%.*s", (int)strcspn(r + n, "\n"), r + n);
		return kind;
	}
	char *line = strstr(out, "runtime error");
	if (!line)
		line = strstr(out, "ERROR: AddressSanitizer");
	if (!line && (line = strstr(out, "\nX ")))
		line++;
	if (line) {
		while (line > out && line[-1] != '\n')
			line--;
		if (line[0] == 'X')
			line += 2;
		int len = snprintf(msg, size, "%.*s", (int)strcspn(line, "\n"), line);
		for (char *frame = strstr(line, " in "); frame; frame = strstr(frame + 4, " in ")) {
			int frame_len = strcspn(frame, "\n");  /* first stack frame in a C file: SeaBIOS or ours */
			char *file = strstr(frame, ".c:");
			if (file && file - frame < frame_len) {
				snprintf(msg + len, size - len, " |%.*s", frame_len, frame);
				break;
			}
		}
	} else {
		snprintf(msg, size, "killed by %s",
			 WIFSIGNALED(status) ? strsignal(WTERMSIG(status)) : "exit status");
	}
	return strstr(msg, "endless loop") ? HANG : CRASH;
}

static int run_child(const struct fuzz_area *area, unsigned index, char *out, int size, int *status)
{
	int fds[2];
	if (pipe(fds)) {
		perror("pipe");
		return -1;
	}
	pid_t pid = fork();
	if (pid < 0) {
		perror("fork");
		return -1;
	}
	if (!pid) {
		close(fds[0]);
		if (dup2(fds[1], 1) < 0 || dup2(fds[1], 2) < 0)
			_exit(3);
		run_case(area, index);
	}
	close(fds[1]);
	int len = 0;
	ssize_t got;
	while ((got = read(fds[0], out + len, size - 1 - len)) > 0 || (got < 0 && errno == EINTR))
		len += got > 0 ? got : 0;
	out[len] = 0;
	close(fds[0]);
	while (waitpid(pid, status, 0) < 0)
		if (errno != EINTR) {
			perror("waitpid");
			return -1;
		}
	return 0;
}

static const char *OnlyArea;

/* describe() rebuilds the case in the area's globals: done in a child so that later cases start clean. */
static void describe_isolated(const struct fuzz_area *area, unsigned index, char *desc, int size)
{
	int fds[2], status;
	desc[0] = 0;
	if (pipe(fds)) {
		perror("pipe");
		_exit(3);
	}
	pid_t pid = fork();
	if (pid < 0) {
		perror("fork");
		_exit(3);
	}
	if (!pid) {
		close(fds[0]);
		area->describe(index, desc, size);
		_exit(write(fds[1], desc, strlen(desc)) < 0);
	}
	close(fds[1]);
	int len = 0;
	ssize_t got;
	while ((got = read(fds[0], desc + len, size - 1 - len)) > 0 || (got < 0 && errno == EINTR))
		len += got > 0 ? got : 0;
	desc[len] = 0;
	close(fds[0]);
	if (waitpid(pid, &status, 0) < 0 || !WIFEXITED(status) || WEXITSTATUS(status)) {
		fprintf(stderr, "devfuzz: describing %s %u failed\n", area->name, index);
		_exit(3);
	}
}

static void worker(int id, int workers, int stats_fd, FILE *failures)
{
	static struct stats st;
	static char out[1 << 16];
	unsigned global = 0;
	for (int a = 0; a < NAREAS; a++) {
		const struct fuzz_area *area = Areas[a];
		unsigned count = OnlyArea && strcmp(OnlyArea, area->name) ? 0 : area->count();
		for (unsigned i = 0; i < count; i++, global++) {
			if (global % workers != (unsigned)id)
				continue;
			int status, cls = area->class_of(i);
			char msg[512], desc[512];
			unsigned long long us;
			if (run_child(area, i, out, sizeof(out), &status))
				_exit(3);
			int kind = child_result(out, status, msg, sizeof(msg), &us);
			st.cases[a][cls]++;
			if (!st.kinds[a][cls][kind]++)
				st.example[a][cls][kind] = i;
			if (us > st.max_us[a][cls]) {
				st.max_us[a][cls] = us;
				st.max_index[a][cls] = i;
			}
			if (kind != PASS) {
				describe_isolated(area, i, desc, sizeof(desc));
				if (fprintf(failures, "%s %u [%s] %s: %s | %s\n", area->name, i, area->class_names[cls],
					    KindNames[kind], msg, desc) < 0 || fflush(failures)) {
					perror("failures file");
					_exit(3);
				}
			}
		}
	}
	if (write(stats_fd, &st, sizeof(st)) != sizeof(st)) {
		perror("stats pipe");
		_exit(3);
	}
	_exit(0);
}

static void merge(struct stats *total, const struct stats *st)
{
	for (int a = 0; a < NAREAS; a++)
		for (int c = 0; c < MAX_CLASSES; c++) {
			if (st->max_us[a][c] > total->max_us[a][c]) {
				total->max_us[a][c] = st->max_us[a][c];
				total->max_index[a][c] = st->max_index[a][c];
			}
			for (int k = 0; k < KINDS; k++) {
				if (st->kinds[a][c][k] && !total->kinds[a][c][k])
					total->example[a][c][k] = st->example[a][c][k];
				total->kinds[a][c][k] += st->kinds[a][c][k];
			}
			total->cases[a][c] += st->cases[a][c];
		}
}

static int read_full(int fd, void *buf, size_t size)
{
	size_t done = 0;
	while (done < size) {
		ssize_t got = read(fd, (char *)buf + done, size - done);
		if (got < 0 && errno == EINTR)
			continue;
		if (got <= 0)
			return -1;
		done += got;
	}
	return 0;
}

static void map_low_memory(void)
{
	void *low = (void *)FUZZ_PHYS_LOW;
	if (mmap(low, FUZZ_PHYS_END - FUZZ_PHYS_LOW, PROT_READ | PROT_WRITE,
		 MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0) != low) {
		perror("devfuzz: mmap of physical 10000-10FFFF");
		exit(2);
	}
}

static int run_one(const char *name, unsigned index)
{
	for (int a = 0; a < NAREAS; a++)
		if (!strcmp(Areas[a]->name, name)) {
			char desc[512];
			if (index >= Areas[a]->count()) {
				fprintf(stderr, "devfuzz: %s has %u cases\n", name, Areas[a]->count());
				return 2;
			}
			Areas[a]->describe(index, desc, sizeof(desc));
			fprintf(stdout, "%s %u [%s]: %s\n", name, index, Areas[a]->class_names[Areas[a]->class_of(index)],
				desc);
			fflush(stdout);
			run_case(Areas[a], index);
		}
	fprintf(stderr, "devfuzz: no area %s\n", name);
	return 2;
}

int main(int argc, char **argv)
{
	const char *failures_path = "devfuzz-failures.txt";
	int opt;
	while ((opt = getopt(argc, argv, "va:o:")) != -1) {
		if (opt == 'v')
			fuzz_verbose = 1;
		else if (opt == 'a')
			OnlyArea = optarg;
		else if (opt == 'o')
			failures_path = optarg;
		else
			return 2;
	}
	if (fuzz_verbose && !getenv("DEVFUZZ_SYMBOLIZE")) {
		if (setenv("DEVFUZZ_SYMBOLIZE", "1", 1) || setenv("ASAN_OPTIONS", "symbolize=1", 1)
		    || setenv("UBSAN_OPTIONS", "print_stacktrace=1", 1))
			perror("setenv");
		else
			execv("/proc/self/exe", argv);
		perror("devfuzz: re-exec with stack traces");
	}
	struct rlimit no_core = { 0, 0 };            /* crashing cases are expected: no core dumps */
	if (setrlimit(RLIMIT_CORE, &no_core)) {
		perror("devfuzz: setrlimit");
		return 2;
	}
	map_low_memory();
	if (optind + 2 == argc)
		return run_one(argv[optind], strtoul(argv[optind + 1], NULL, 0));
	if (optind != argc) {
		fprintf(stderr, "usage: devfuzz [-v] [-a area] [-o failures.txt] [area index]\n");
		return 2;
	}

	FILE *failures = fopen(failures_path, "w");
	long workers = sysconf(_SC_NPROCESSORS_ONLN);
	int fds[2];
	struct timespec t0, t1;
	if (!failures || workers < 1 || pipe(fds)) {
		perror("devfuzz");
		return 2;
	}
	setvbuf(failures, NULL, _IOLBF, 0);
	clock_gettime(CLOCK_MONOTONIC, &t0);
	for (long w = 0; w < workers; w++) {
		pid_t pid = fork();
		if (pid < 0) {
			perror("fork");
			return 2;
		}
		if (!pid)
			worker(w, workers, fds[1], failures);
	}
	close(fds[1]);
	static struct stats total, st;
	for (long w = 0; w < workers; w++) {
		if (read_full(fds[0], &st, sizeof(st))) {
			fprintf(stderr, "devfuzz: a worker died\n");
			return 2;
		}
		merge(&total, &st);
	}
	int status;
	while (wait(&status) > 0)
		if (!WIFEXITED(status) || WEXITSTATUS(status)) {
			fprintf(stderr, "devfuzz: worker failed\n");
			return 2;
		}
	clock_gettime(CLOCK_MONOTONIC, &t1);
	if (fclose(failures)) {
		perror(failures_path);
		return 2;
	}

	double seconds = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
	unsigned cases = 0, bad = 0;
	for (int a = 0; a < NAREAS; a++)
		for (int c = 0; c < Areas[a]->nclasses; c++) {
			cases += total.cases[a][c];
			bad += total.cases[a][c] - total.kinds[a][c][PASS];
		}
	fprintf(stdout, "%u cases in %.2f s: %.0f cases/s, %ld workers\n", cases, seconds, cases / seconds, workers);
	for (int a = 0; a < NAREAS; a++)
		for (int c = 0; c < Areas[a]->nclasses; c++) {
			fprintf(stdout, "  %-6s %-40s %6u cases, slowest POST %6.1f s (#%u)", Areas[a]->name,
				Areas[a]->class_names[c], total.cases[a][c], total.max_us[a][c] / 1e6,
				total.max_index[a][c]);
			for (int k = FAIL; k < KINDS; k++)
				if (total.kinds[a][c][k])
					fprintf(stdout, ", %u %s (e.g. #%u)", total.kinds[a][c][k], KindNames[k],
						total.example[a][c][k]);
			fputc('\n', stdout);
		}
	fprintf(stdout, "%u of %u cases fail (list: %s, rerun one: devfuzz -v <area> <index>)\n", bad, cases,
		failures_path);
	return bad ? 1 : 0;
}
