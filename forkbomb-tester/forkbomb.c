#include <err.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

int main(int argc, char** argv) {
	if (argc != 2)
		errx(EXIT_FAILURE, "usage: %s <iteration-cnt>", argv[0] ?: "<argv[0] missing>");

	unsigned iteration_cnt;
	{
		char* endptr;
		iteration_cnt = strtol(argv[1], &endptr, 0);
		if (*endptr && *endptr != ' ' && *endptr != '\n')
			errx(EXIT_FAILURE, "'%c' is not a valid digit. Abort.", *endptr);
	}

	pid_t* pids = (pid_t*)malloc(sizeof(pid_t) * iteration_cnt);
	if (!pids)
		err(EXIT_FAILURE, "Could not malloc");

	size_t pids_cnt = 0;
	for (; iteration_cnt; iteration_cnt--) {
		pid_t p = fork();
		if (p < 0) {
			warn("Could not fork");
			// break;
		} else if (p == 0) {
			pids_cnt = 0;
		} else {
			pids[pids_cnt++] = p;
		}
	}

	for (; pids_cnt > 0;) {
		pid_t p = waitpid(pids[--pids_cnt], NULL, 0);
		if (p < 0)
			warn("Could not wait for pid %d", pids[pids_cnt]);
	}
}
