/*
 * claw-term — PTY-backed terminal emulator / spawner for claw-linux.
 *
 * Spawns a command (default: $SHELL) inside a POSIX pseudo-terminal (PTY)
 * and bridges the PTY master to the invoking terminal.  On the XFCE desktop
 * this binary is launched by xfce4-terminal or called directly from the
 * agent; in headless mode it works as a pty-backed subprocess runner that
 * captures the full output of a command.
 *
 * Corresponds to: openclaw/openclaw src/terminal/ (PTY/terminal emulation)
 *
 * Usage
 * -----
 *   claw-term [OPTIONS] [-- COMMAND [ARGS…]]
 *
 * Options
 * -------
 *   -r ROWS    Terminal rows (default: 24)
 *   -c COLS    Terminal columns (default: 80)
 *   -t TITLE   Terminal title (written to xterm title escape)
 *   -C         Capture mode — run command, capture output, exit (no bridge)
 *   -h         Show this help
 *
 * Without a command, $SHELL (or /bin/sh) is started interactively.
 * With -C, the command output is written to stdout and claw-term exits
 * with the child's exit code.
 *
 * Build:
 *   cc -O2 -Wall -Wextra -o claw-term main.c
 */

#define _XOPEN_SOURCE 700
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <termios.h>
#include <poll.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/ioctl.h>

/* ---- tunables ------------------------------------------------------------ */
#define DEFAULT_ROWS     24
#define DEFAULT_COLS     80
#define BUF_SIZE       4096

/* ---- globals ------------------------------------------------------------- */
static volatile int  g_child_done = 0;
static pid_t         g_child_pid  = -1;
static struct termios g_saved_termios;
static int           g_raw_mode  = 0;

static void on_sigchld(int s)
{
    (void)s;
    /* Just mark that the child has changed state; main will reap it. */
    g_child_done = 1;
}

static void on_sigterm(int s) { (void)s; g_child_done = 1; }

/* ---- raw terminal -------------------------------------------------------- */

static void enter_raw(int fd)
{
    if (!isatty(fd)) return;
    if (tcgetattr(fd, &g_saved_termios) < 0) return;
    struct termios raw = g_saved_termios;
    raw.c_iflag &= ~(unsigned)(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~(unsigned)OPOST;
    raw.c_cflag |=  (unsigned)CS8;
    raw.c_lflag &= ~(unsigned)(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN]  = 1;
    raw.c_cc[VTIME] = 0;
    tcsetattr(fd, TCSAFLUSH, &raw);
    g_raw_mode = 1;
}

static void leave_raw(int fd)
{
    if (g_raw_mode && isatty(fd))
        tcsetattr(fd, TCSAFLUSH, &g_saved_termios);
    g_raw_mode = 0;
}

/* ---- PTY helpers --------------------------------------------------------- */

/*
 * Open a POSIX PTY master using posix_openpt / grantpt / unlockpt.
 * Returns master fd, sets *slave_path to the slave device path.
 */
static int open_pty_master(char *slave_path, size_t slave_path_size)
{
    int master = posix_openpt(O_RDWR | O_NOCTTY);
    if (master < 0) return -1;
    if (grantpt(master)  < 0) { close(master); return -1; }
    if (unlockpt(master) < 0) { close(master); return -1; }
    const char *name = ptsname(master);
    if (!name) { close(master); return -1; }
    snprintf(slave_path, slave_path_size, "%s", name);
    return master;
}

/*
 * Resize the PTY window.
 */
static void set_pty_size(int master_fd, int rows, int cols)
{
    struct winsize ws;
    memset(&ws, 0, sizeof(ws));
    ws.ws_row = (unsigned short)rows;
    ws.ws_col = (unsigned short)cols;
    ioctl(master_fd, TIOCSWINSZ, &ws);
}

/* ---- interactive bridge -------------------------------------------------- */

/*
 * Bridge stdin/stdout to/from the PTY master until the child exits.
 */
static void bridge_pty(int master_fd)
{
    char buf[BUF_SIZE];

    struct pollfd fds[2];
    fds[0].fd     = STDIN_FILENO;
    fds[0].events = POLLIN;
    fds[1].fd     = master_fd;
    fds[1].events = POLLIN;

    while (!g_child_done) {
        int n = poll(fds, 2, 100);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }

        /* stdin → PTY master */
        if (fds[0].revents & POLLIN) {
            ssize_t nr = read(STDIN_FILENO, buf, sizeof(buf));
            if (nr > 0) {
                ssize_t nw = write(master_fd, buf, (size_t)nr);
                (void)nw;
            }
        }

        /* PTY master → stdout */
        if (fds[1].revents & POLLIN) {
            ssize_t nr = read(master_fd, buf, sizeof(buf));
            if (nr > 0) {
                ssize_t nw = write(STDOUT_FILENO, buf, (size_t)nr);
                (void)nw;
            } else if (nr == 0 || (nr < 0 && errno != EAGAIN && errno != EINTR)) {
                break;  /* slave closed */
            }
        }

        if (fds[1].revents & (POLLHUP | POLLERR)) break;
    }

    /* Drain remaining output */
    int flags = fcntl(master_fd, F_GETFL, 0);
    fcntl(master_fd, F_SETFL, flags | O_NONBLOCK);
    ssize_t nr;
    while ((nr = read(master_fd, buf, sizeof(buf))) > 0) {
        ssize_t nw = write(STDOUT_FILENO, buf, (size_t)nr);
        (void)nw;
    }
}

/* ---- capture mode -------------------------------------------------------- */

/*
 * In capture mode: run child and write all its output to stdout,
 * then exit with child's exit status.
 */
static int capture_pty(int master_fd, pid_t child_pid)
{
    char buf[BUF_SIZE];

    /* Set master non-blocking for final drain */
    int flags = fcntl(master_fd, F_GETFL, 0);
    fcntl(master_fd, F_SETFL, flags | O_NONBLOCK);

    while (!g_child_done) {
        ssize_t nr = read(master_fd, buf, sizeof(buf));
        if (nr > 0) {
            ssize_t nw = write(STDOUT_FILENO, buf, (size_t)nr);
            (void)nw;
        } else if (nr < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct timespec ts = { 0, 10000000L }; /* 10 ms */
            nanosleep(&ts, NULL);
        } else if (nr == 0 || (nr < 0 && errno != EINTR)) {
            break;
        }
    }

    /* Final drain */
    ssize_t nr;
    while ((nr = read(master_fd, buf, sizeof(buf))) > 0) {
        ssize_t nw = write(STDOUT_FILENO, buf, (size_t)nr);
        (void)nw;
    }

    int status = 0;
    waitpid(child_pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}

/* ---- usage --------------------------------------------------------------- */

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [OPTIONS] [-- COMMAND [ARGS…]]\n"
        "\n"
        "Options:\n"
        "  -r ROWS    Terminal rows (default: %d)\n"
        "  -c COLS    Terminal columns (default: %d)\n"
        "  -t TITLE   Set terminal title\n"
        "  -C         Capture mode — run command, capture all output, exit\n"
        "  -h         Show this help\n"
        "\n"
        "Without a command, $SHELL (or /bin/sh) is started interactively.\n",
        prog, DEFAULT_ROWS, DEFAULT_COLS);
}

/* ---- main ---------------------------------------------------------------- */

int main(int argc, char *argv[])
{
    int   rows    = DEFAULT_ROWS;
    int   cols    = DEFAULT_COLS;
    int   capture = 0;
    const char *title = NULL;

    int opt;
    while ((opt = getopt(argc, argv, "r:c:t:Ch")) != -1) {
        switch (opt) {
            case 'r': rows    = atoi(optarg); break;
            case 'c': cols    = atoi(optarg); break;
            case 't': title   = optarg;       break;
            case 'C': capture = 1;            break;
            case 'h': usage(argv[0]); return 0;
            default:  usage(argv[0]); return 1;
        }
    }

    /* Remaining argv after -- or after options is the command */
    char **cmd_argv = NULL;
    if (optind < argc) {
        cmd_argv = argv + optind;
    }

    /* Default command: $SHELL or /bin/sh */
    char *default_cmd[] = { NULL, NULL };
    if (!cmd_argv) {
        const char *sh = getenv("SHELL");
        default_cmd[0] = (char *)(sh ? sh : "/bin/sh");
        cmd_argv = default_cmd;
    }

    /* Open PTY */
    char slave_path[64] = {0};
    int master_fd = open_pty_master(slave_path, sizeof(slave_path));
    if (master_fd < 0) {
        fprintf(stderr, "claw-term: failed to open PTY: %s\n", strerror(errno));
        return 1;
    }
    set_pty_size(master_fd, rows, cols);

    /* Propagate terminal size from real terminal */
    if (isatty(STDIN_FILENO)) {
        struct winsize ws;
        if (ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 && ws.ws_row > 0) {
            rows = ws.ws_row;
            cols = ws.ws_col;
            set_pty_size(master_fd, rows, cols);
        }
    }

    /* Set title if provided */
    if (title && isatty(STDOUT_FILENO))
        printf("\033]0;%s\007", title);

    /* Set up signals */
    signal(SIGCHLD, on_sigchld);
    signal(SIGTERM, on_sigterm);
    signal(SIGPIPE, SIG_IGN);

    /* Fork child */
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        close(master_fd);
        return 1;
    }

    if (pid == 0) {
        /* Child: attach to PTY slave as controlling terminal */
        close(master_fd);

        /* Create new session */
        setsid();

        /* Open slave */
        int slave_fd = open(slave_path, O_RDWR);
        if (slave_fd < 0) {
            perror("claw-term: open slave");
            _exit(1);
        }

        /* Set slave as controlling terminal */
        ioctl(slave_fd, TIOCSCTTY, 0);

        /* Dup slave to stdin/stdout/stderr */
        dup2(slave_fd, STDIN_FILENO);
        dup2(slave_fd, STDOUT_FILENO);
        dup2(slave_fd, STDERR_FILENO);
        if (slave_fd > 2) close(slave_fd);

        /* Set PTY window size */
        struct winsize ws;
        memset(&ws, 0, sizeof(ws));
        ws.ws_row = (unsigned short)rows;
        ws.ws_col = (unsigned short)cols;
        ioctl(STDIN_FILENO, TIOCSWINSZ, &ws);

        /* Export TERM */
        if (!getenv("TERM")) setenv("TERM", "xterm-256color", 1);

        execvp(cmd_argv[0], cmd_argv);
        fprintf(stderr, "claw-term: exec(%s): %s\n", cmd_argv[0], strerror(errno));
        _exit(127);
    }

    /* Parent */
    g_child_pid = pid;

    int exit_code;
    if (capture) {
        exit_code = capture_pty(master_fd, pid);
    } else {
        /* Interactive bridge */
        enter_raw(STDIN_FILENO);
        bridge_pty(master_fd);
        leave_raw(STDIN_FILENO);

        int status = 0;
        waitpid(pid, &status, 0);
        exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
    }

    close(master_fd);
    return exit_code;
}
