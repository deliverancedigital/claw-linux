/*
 * claw-daemon — Service lifecycle manager for claw-linux.
 *
 * Manages the four core claw-linux daemons (gateway, channel, cron, agent)
 * without requiring Node.js or a full init system.  Uses PID files to track
 * running processes and log files for each service's output.
 *
 * Corresponds to: openclaw/openclaw src/daemon/ (daemon lifecycle management)
 *
 * Commands
 * --------
 *   claw-daemon start   <service>   — start a service in the background
 *   claw-daemon stop    <service>   — send SIGTERM to a running service
 *   claw-daemon restart <service>   — stop then start
 *   claw-daemon status  [service]   — print running/stopped status (all if omitted)
 *   claw-daemon reload  <service>   — send SIGHUP (claw-cron supports live reload)
 *
 * Services
 * --------
 *   gateway   — claw-gateway HTTP control-plane
 *   channel   — claw-channel webhook adapter
 *   cron      — claw-cron automation scheduler
 *   agent     — Python AI agent (python3 /opt/claw/agent/main.py)
 *
 * Paths
 * -----
 *   PID files : /var/run/claw/<service>.pid
 *   Log files : /var/log/claw/<service>.log
 *   Binaries  : /usr/local/bin/claw-<service>  (gateway/channel/cron)
 *               python3 /opt/claw/agent/main.py (agent)
 *
 * Override binary paths with environment variables:
 *   CLAW_BIN_GATEWAY, CLAW_BIN_CHANNEL, CLAW_BIN_CRON, CLAW_BIN_AGENT
 *
 * Build:
 *   cc -O2 -Wall -Wextra -o claw-daemon main.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>

/* ---- tunables ------------------------------------------------------------ */
#define PID_DIR      "/var/run/claw"
#define LOG_DIR      "/var/log/claw"
#define MAX_SERVICES   4
#define MAX_PATH_LEN  256
#define STOP_TIMEOUT_S  15   /* seconds to wait for graceful stop */

/* ---- service table ------------------------------------------------------- */

typedef struct {
    const char *name;           /* service identifier              */
    const char *default_bin;    /* default binary / interpreter    */
    const char *default_args;   /* space-separated args, or NULL   */
    const char *env_bin_var;    /* env var to override the binary  */
} ServiceDef;

static const ServiceDef SERVICES[MAX_SERVICES] = {
    { "gateway", "/usr/local/bin/claw-gateway", NULL,                        "CLAW_BIN_GATEWAY" },
    { "channel", "/usr/local/bin/claw-channel", NULL,                        "CLAW_BIN_CHANNEL" },
    { "cron",    "/usr/local/bin/claw-cron",    NULL,                        "CLAW_BIN_CRON"    },
    { "agent",   "python3",                     "/opt/claw/agent/main.py",   "CLAW_BIN_AGENT"   },
};

/* ---- helpers ------------------------------------------------------------- */

static void pid_path(const char *svc, char *buf, size_t sz)
{
    snprintf(buf, sz, "%s/%s.pid", PID_DIR, svc);
}

static void log_path(const char *svc, char *buf, size_t sz)
{
    snprintf(buf, sz, "%s/%s.log", LOG_DIR, svc);
}

/*
 * Read the PID from a PID file.  Returns the PID on success, -1 if the
 * file does not exist or cannot be parsed.
 */
static pid_t read_pid(const char *svc)
{
    char path[MAX_PATH_LEN];
    pid_path(svc, path, sizeof(path));

    FILE *f = fopen(path, "r");
    if (!f) return -1;

    long pid = -1;
    if (fscanf(f, "%ld", &pid) != 1) pid = -1;
    fclose(f);
    return (pid_t)pid;
}

/*
 * Write a PID file.  Returns 0 on success.
 */
static int write_pid(const char *svc, pid_t pid)
{
    char path[MAX_PATH_LEN];
    pid_path(svc, path, sizeof(path));

    FILE *f = fopen(path, "w");
    if (!f) return -1;
    fprintf(f, "%ld\n", (long)pid);
    fclose(f);
    return 0;
}

static void remove_pid(const char *svc)
{
    char path[MAX_PATH_LEN];
    pid_path(svc, path, sizeof(path));
    unlink(path);
}

/*
 * Check whether a process with the given PID is alive.
 */
static int pid_alive(pid_t pid)
{
    if (pid <= 0) return 0;
    return (kill(pid, 0) == 0 || errno == EPERM);
}

/*
 * Look up a service by name.  Returns pointer into SERVICES[], or NULL.
 */
static const ServiceDef *find_service(const char *name)
{
    for (int i = 0; i < MAX_SERVICES; i++) {
        if (strcmp(SERVICES[i].name, name) == 0)
            return &SERVICES[i];
    }
    return NULL;
}

static void ensure_dirs(void)
{
    mkdir(PID_DIR, 0755);
    mkdir(LOG_DIR, 0755);
}

/* ---- commands ------------------------------------------------------------ */

/*
 * Start a service.  Forks a child that:
 *   1. opens the log file for stdout/stderr
 *   2. execs the service binary
 *   3. writes the grandchild PID so the daemon fully detaches
 */
static int cmd_start(const char *name)
{
    const ServiceDef *svc = find_service(name);
    if (!svc) {
        fprintf(stderr, "claw-daemon: unknown service '%s'\n", name);
        return 1;
    }

    /* Check if already running */
    pid_t existing = read_pid(name);
    if (existing > 0 && pid_alive(existing)) {
        printf("claw-daemon: %s is already running (pid %ld)\n",
               name, (long)existing);
        return 0;
    }
    if (existing > 0) {
        remove_pid(name); /* stale PID file */
    }

    /* Resolve binary (allow env override) */
    const char *bin = svc->default_bin;
    const char *env_bin = getenv(svc->env_bin_var);
    if (env_bin && *env_bin) bin = env_bin;

    /* Open log file */
    char lpath[MAX_PATH_LEN];
    log_path(name, lpath, sizeof(lpath));
    int log_fd = open(lpath, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (log_fd < 0) {
        fprintf(stderr, "claw-daemon: cannot open log %s: %s\n",
                lpath, strerror(errno));
        return 1;
    }

    /* Double-fork to fully detach the service from our process group */
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        close(log_fd);
        return 1;
    }

    if (pid == 0) {
        /* ----- Intermediate child ----- */

        /* Detach from the terminal / parent session */
        setsid();

        pid_t gpid = fork();
        if (gpid < 0) _exit(1);
        if (gpid > 0) {
            /* Intermediate child writes the grandchild PID and exits */
            write_pid(name, gpid);
            _exit(0);
        }

        /* ----- Grandchild: the actual service process ----- */

        /* Redirect stdout and stderr to the log file */
        dup2(log_fd, STDOUT_FILENO);
        dup2(log_fd, STDERR_FILENO);
        close(log_fd);

        /* Redirect stdin to /dev/null */
        int devnull = open("/dev/null", O_RDONLY);
        if (devnull >= 0) { dup2(devnull, STDIN_FILENO); close(devnull); }

        /* Exec the service — pass default_args regardless of binary override */
        if (svc->default_args) {
            /* e.g. python3 /opt/claw/agent/main.py */
            execl(bin, bin, svc->default_args, (char *)NULL);
        } else {
            execl(bin, bin, (char *)NULL);
        }
        fprintf(stderr, "claw-daemon: exec(%s) failed: %s\n", bin, strerror(errno));
        _exit(127);
    }

    /* Parent: wait for the intermediate child to write the PID and exit */
    close(log_fd);
    int wstatus;
    waitpid(pid, &wstatus, 0);

    /* Give the grandchild a moment to start */
    usleep(100000); /* 100 ms */

    pid_t gpid = read_pid(name);
    if (gpid > 0 && pid_alive(gpid)) {
        printf("claw-daemon: started %s (pid %ld) → log: %s\n",
               name, (long)gpid, lpath);
        return 0;
    }

    fprintf(stderr, "claw-daemon: %s failed to start — check %s\n", name, lpath);
    remove_pid(name);
    return 1;
}

static int cmd_stop(const char *name)
{
    const ServiceDef *svc = find_service(name);
    if (!svc) {
        fprintf(stderr, "claw-daemon: unknown service '%s'\n", name);
        return 1;
    }

    pid_t pid = read_pid(name);
    if (pid <= 0 || !pid_alive(pid)) {
        printf("claw-daemon: %s is not running\n", name);
        remove_pid(name);
        return 0;
    }

    /* SIGTERM first */
    kill(pid, SIGTERM);
    printf("claw-daemon: sent SIGTERM to %s (pid %ld)\n", name, (long)pid);

    /* Wait up to STOP_TIMEOUT_S seconds */
    for (int i = 0; i < STOP_TIMEOUT_S * 10; i++) {
        usleep(100000); /* 100 ms */
        if (!pid_alive(pid)) break;
    }

    if (pid_alive(pid)) {
        kill(pid, SIGKILL);
        printf("claw-daemon: sent SIGKILL to %s (pid %ld)\n", name, (long)pid);
        usleep(200000);
    }

    remove_pid(name);
    printf("claw-daemon: stopped %s\n", name);
    return 0;
}

static int cmd_restart(const char *name)
{
    cmd_stop(name);
    return cmd_start(name);
}

static int cmd_reload(const char *name)
{
    const ServiceDef *svc = find_service(name);
    if (!svc) {
        fprintf(stderr, "claw-daemon: unknown service '%s'\n", name);
        return 1;
    }

    pid_t pid = read_pid(name);
    if (pid <= 0 || !pid_alive(pid)) {
        fprintf(stderr, "claw-daemon: %s is not running\n", name);
        return 1;
    }

    kill(pid, SIGHUP);
    printf("claw-daemon: sent SIGHUP to %s (pid %ld)\n", name, (long)pid);
    return 0;
}

static void print_status_one(const char *name)
{
    pid_t pid = read_pid(name);
    if (pid > 0 && pid_alive(pid)) {
        printf("  %-10s  running   pid %-7ld\n", name, (long)pid);
    } else {
        if (pid > 0) remove_pid(name);
        printf("  %-10s  stopped\n", name);
    }
}

static int cmd_status(const char *name)
{
    if (name) {
        if (!find_service(name)) {
            fprintf(stderr, "claw-daemon: unknown service '%s'\n", name);
            return 1;
        }
        print_status_one(name);
    } else {
        printf("claw-daemon service status:\n");
        for (int i = 0; i < MAX_SERVICES; i++)
            print_status_one(SERVICES[i].name);
    }
    return 0;
}

/* ---- usage --------------------------------------------------------------- */

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s <command> [service]\n"
        "\n"
        "Commands:\n"
        "  start   <service>   Start a service in the background\n"
        "  stop    <service>   Stop a running service\n"
        "  restart <service>   Stop then start a service\n"
        "  reload  <service>   Send SIGHUP (live config reload)\n"
        "  status  [service]   Show status (all services if omitted)\n"
        "\n"
        "Services:  gateway  channel  cron  agent\n"
        "\n"
        "Environment overrides:\n"
        "  CLAW_BIN_GATEWAY   Path to claw-gateway binary\n"
        "  CLAW_BIN_CHANNEL   Path to claw-channel binary\n"
        "  CLAW_BIN_CRON      Path to claw-cron binary\n"
        "  CLAW_BIN_AGENT     Interpreter for the agent (default: python3)\n",
        prog);
}

/* ---- main ---------------------------------------------------------------- */

int main(int argc, char *argv[])
{
    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    ensure_dirs();

    const char *cmd = argv[1];
    const char *svc = (argc >= 3) ? argv[2] : NULL;

    if (strcmp(cmd, "start")   == 0 && svc) return cmd_start(svc);
    if (strcmp(cmd, "stop")    == 0 && svc) return cmd_stop(svc);
    if (strcmp(cmd, "restart") == 0 && svc) return cmd_restart(svc);
    if (strcmp(cmd, "reload")  == 0 && svc) return cmd_reload(svc);
    if (strcmp(cmd, "status")  == 0)        return cmd_status(svc);

    /* If start/stop/restart/reload called without a service name */
    if (strcmp(cmd, "start")   == 0 ||
        strcmp(cmd, "stop")    == 0 ||
        strcmp(cmd, "restart") == 0 ||
        strcmp(cmd, "reload")  == 0) {
        fprintf(stderr, "claw-daemon: '%s' requires a service name\n", cmd);
        usage(argv[0]);
        return 1;
    }

    fprintf(stderr, "claw-daemon: unknown command '%s'\n", cmd);
    usage(argv[0]);
    return 1;
}
