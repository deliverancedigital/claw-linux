/*
 * claw-shell — Shell command execution skill binary for claw-linux.
 *
 * Protocol (stdin → stdout, one JSON object each):
 *
 *   Input:
 *     { "command": "<shell command>", "timeout": <seconds> }
 *
 *   Output (success):
 *     { "ok": true, "exit_code": <int>, "stdout": "<text>", "stderr": "<text>" }
 *
 *   Output (blocked or error):
 *     { "ok": false, "error": "<reason>" }
 *
 * The binary runs the command via /bin/sh -c inside a child process and
 * captures both stdout and stderr.  A SIGALRM fires after `timeout` seconds
 * and kills the child to enforce the time limit.
 *
 * Build:
 *   cc -O2 -Wall -Wextra -o claw-shell main.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/types.h>

#include "../common/claw_json.h"

/* ---- tunables ------------------------------------------------------------ */
#define MAX_INPUT_BYTES   65536
#define MAX_CMD_BYTES      4096
#define MAX_OUTPUT_BYTES  (1024 * 1024)   /* 1 MiB per stream */
#define DEFAULT_TIMEOUT        30         /* seconds */
#define MAX_TIMEOUT          3600         /* 1 hour hard cap */

/* ---- blocked command prefixes -------------------------------------------- */
static const char *BLOCKED_PREFIXES[] = {
    "rm -rf /",
    "mkfs",
    "dd if=/dev/zero",
    ":(){:|:&};:",   /* fork bomb */
    "chmod -R 777 /",
    "wget -O- | sh",
    "curl | sh",
    NULL
};

/* ---- helpers ------------------------------------------------------------- */

static void emit_error(const char *msg)
{
    char esc[4096];
    json_escape(msg, esc, sizeof(esc));
    printf("{\"ok\":false,\"error\":\"%s\"}\n", esc);
    fflush(stdout);
}

static int command_is_blocked(const char *cmd)
{
    /* Normalize: strip leading whitespace, then collapse interior runs of
     * whitespace to single spaces, so that "rm  -rf /" matches "rm -rf /". */
    char norm[MAX_CMD_BYTES];
    size_t j = 0;
    int in_space = 1;   /* treat leading whitespace as a gap to skip */

    for (size_t i = 0; cmd[i] && j < sizeof(norm) - 1; i++) {
        char c = cmd[i];
        if (c == ' ' || c == '\t') {
            if (!in_space && j < sizeof(norm) - 1)
                norm[j++] = ' ';
            in_space = 1;
        } else {
            norm[j++] = c;
            in_space = 0;
        }
    }
    /* Strip trailing space added above */
    if (j > 0 && norm[j - 1] == ' ') j--;
    norm[j] = '\0';

    for (int i = 0; BLOCKED_PREFIXES[i]; i++) {
        if (strncmp(norm, BLOCKED_PREFIXES[i],
                    strlen(BLOCKED_PREFIXES[i])) == 0)
            return 1;
    }
    return 0;
}

/*
 * Read up to max_bytes from fd into a freshly allocated, NUL-terminated
 * buffer.  Sets *out and returns bytes read, or -1 on allocation failure.
 */
static ssize_t drain_fd(int fd, char **out, size_t max_bytes)
{
    size_t capacity = 4096;
    size_t used = 0;
    char *buf = malloc(capacity + 1);
    if (!buf) return -1;

    while (used < max_bytes) {
        /* Grow buffer if needed */
        if (used == capacity) {
            size_t next = capacity * 2;
            if (next > max_bytes) next = max_bytes;
            char *tmp = realloc(buf, next + 1);
            if (!tmp) { free(buf); return -1; }
            buf = tmp;
            capacity = next;
        }
        ssize_t n = read(fd, buf + used, capacity - used);
        if (n <= 0) break;
        used += (size_t)n;
    }
    buf[used] = '\0';
    *out = buf;
    return (ssize_t)used;
}

/* ---- SIGALRM handler — kills the grandchild process group ---------------- */
static volatile pid_t child_pid = 0;

static void alarm_handler(int sig)
{
    (void)sig;
    if (child_pid > 0)
        kill(-child_pid, SIGKILL); /* kill the entire process group */
}

/* ---- main ---------------------------------------------------------------- */

int main(void)
{
    /* 1. Read JSON request from stdin */
    char input[MAX_INPUT_BYTES];
    size_t total = 0;
    ssize_t n;
    while (total < sizeof(input) - 1) {
        n = read(STDIN_FILENO, input + total, sizeof(input) - 1 - total);
        if (n <= 0) break;
        total += (size_t)n;
    }
    input[total] = '\0';

    /* 2. Parse fields */
    char command[MAX_CMD_BYTES] = {0};
    if (!json_get_string(input, "command", command, sizeof(command))) {
        emit_error("Missing or invalid 'command' field");
        return 0;
    }

    long timeout = json_get_long(input, "timeout", DEFAULT_TIMEOUT);
    if (timeout <= 0 || timeout > MAX_TIMEOUT)
        timeout = DEFAULT_TIMEOUT;

    /* 3. Policy check */
    if (command_is_blocked(command)) {
        emit_error("Command blocked by security policy");
        return 0;
    }

    /* 4. Create pipes for child stdout and stderr */
    int pout[2], perr[2];
    if (pipe(pout) < 0 || pipe(perr) < 0) {
        emit_error("pipe() failed");
        return 1;
    }

    /* 5. Fork child */
    pid_t pid = fork();
    if (pid < 0) {
        emit_error("fork() failed");
        return 1;
    }

    if (pid == 0) {
        /* Child: set new process group so we can kill it wholesale */
        setpgid(0, 0);

        /* Redirect stdout / stderr to pipes */
        close(pout[0]); close(perr[0]);
        dup2(pout[1], STDOUT_FILENO);
        dup2(perr[1], STDERR_FILENO);
        close(pout[1]); close(perr[1]);

        /* Redirect stdin to /dev/null */
        int devnull = open("/dev/null", O_RDONLY);
        if (devnull >= 0) { dup2(devnull, STDIN_FILENO); close(devnull); }

        execl("/bin/sh", "sh", "-c", command, (char *)NULL);
        _exit(127);
    }

    /* Parent */
    child_pid = pid;
    close(pout[1]); close(perr[1]);

    /* 6. Install alarm */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = alarm_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGALRM, &sa, NULL);
    alarm((unsigned int)timeout);

    /* 7. Drain pipes */
    char *out_buf = NULL, *err_buf = NULL;
    ssize_t out_len = drain_fd(pout[0], &out_buf, MAX_OUTPUT_BYTES);
    ssize_t err_len = drain_fd(perr[0], &err_buf, MAX_OUTPUT_BYTES);
    alarm(0);

    close(pout[0]); close(perr[0]);

    /* 8. Reap child */
    int status = 0;
    waitpid(pid, &status, 0);
    child_pid = 0;

    int exit_code = WIFEXITED(status)   ? WEXITSTATUS(status) :
                    WIFSIGNALED(status) ? 128 + WTERMSIG(status) : -1;

    if (out_len < 0) { free(out_buf); out_buf = NULL; out_len = 0; }
    if (err_len < 0) { free(err_buf); err_buf = NULL; err_len = 0; }
    if (!out_buf) { out_buf = malloc(1); if (out_buf) out_buf[0] = '\0'; }
    if (!err_buf) { err_buf = malloc(1); if (err_buf) err_buf[0] = '\0'; }

    /* 9. JSON-escape outputs and emit result */
    size_t esc_size = (size_t)(out_len + err_len) * 2 + 16;
    if (esc_size < 64) esc_size = 64;
    char *out_esc = malloc(esc_size);
    char *err_esc = malloc(esc_size);

    if (!out_esc || !err_esc) {
        emit_error("Memory allocation failed");
        free(out_buf); free(err_buf); free(out_esc); free(err_esc);
        return 1;
    }

    json_escape(out_buf ? out_buf : "", out_esc, esc_size);
    json_escape(err_buf ? err_buf : "", err_esc, esc_size);

    printf("{\"ok\":true,\"exit_code\":%d,\"stdout\":\"%s\",\"stderr\":\"%s\"}\n",
           exit_code, out_esc, err_esc);
    fflush(stdout);

    free(out_buf); free(err_buf); free(out_esc); free(err_esc);
    return 0;
}
