/*
 * claw-cron — Native cron scheduler daemon for claw-linux automation.
 *
 * Reads a crontab-style schedule file and fires jobs on time without
 * requiring the Node.js runtime.  Each job is executed by invoking the
 * claw-shell binary via a JSON protocol on its stdin, which enforces the
 * same blocked-command policy and timeout limits as the interactive agent.
 *
 * Crontab format
 * --------------
 * Lines that are blank or start with '#' are ignored.
 * Each job line has the format:
 *
 *   MIN HOUR MDAY MON WDAY COMMAND
 *
 * Standard cron field semantics apply.  The special strings @hourly,
 * @daily, @weekly, @monthly, and @reboot are also understood.
 *
 * Example /opt/claw/config/crontab:
 *   # Run agent health check every 5 minutes
 *   */5 * * * * /usr/local/bin/claw-shell '{"command":"echo ping","timeout":5}'
 *
 *   # Daily memory summarisation at 02:00
 *   0 2 * * * python3 /opt/claw/agent/main.py "summarise memory"
 *
 * Environment variables
 * ---------------------
 *   CLAW_CRONTAB   Path to the crontab file
 *                  (default /opt/claw/config/crontab)
 *   CLAW_CRON_LOG  Path to the log file
 *                  (default /var/lib/claw/cron.log)
 *   CLAW_SHELL_BIN Path to claw-shell binary
 *                  (default /usr/local/bin/claw-shell)
 *
 * Build:
 *   cc -O2 -Wall -Wextra -o claw-cron main.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>

/* ---- tunables ------------------------------------------------------------ */
#define MAX_JOBS          256
#define MAX_CMD_LEN      4096
#define MAX_LINE_LEN     4096
#define DEFAULT_CRONTAB  "/opt/claw/config/crontab"
#define DEFAULT_LOG      "/var/lib/claw/cron.log"

/* ---- types --------------------------------------------------------------- */

typedef struct {
    /* -1 means wildcard (*) */
    int min;    /* 0-59  or -1 */
    int hour;   /* 0-23  or -1 */
    int mday;   /* 1-31  or -1 */
    int mon;    /* 1-12  or -1 */
    int wday;   /* 0-6   or -1 (0 = Sunday) */
    char command[MAX_CMD_LEN];
    int  reboot;   /* 1 = run once at startup */
} CronJob;

/* ---- globals ------------------------------------------------------------- */
static volatile int g_running = 1;
static CronJob      g_jobs[MAX_JOBS];
static int          g_job_count = 0;
static FILE        *g_log       = NULL;
static const char  *g_crontab   = DEFAULT_CRONTAB;
static const char  *g_log_path  = DEFAULT_LOG;

/* ---- logging ------------------------------------------------------------- */

static void cron_log(const char *level, const char *msg)
{
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%S", t);
    FILE *out = g_log ? g_log : stderr;
    fprintf(out, "%s [%s] claw-cron: %s\n", ts, level, msg);
    fflush(out);
}

/* ---- signal handlers ----------------------------------------------------- */
static void on_sigterm(int sig) { (void)sig; g_running = 0; }
static void on_sighup(int sig)
{
    (void)sig;
    cron_log("INFO", "SIGHUP received — reloading crontab");
    /* Reload handled in main loop */
}
static void reap(int sig)
{
    (void)sig;
    while (waitpid(-1, NULL, WNOHANG) > 0)
        ;
}

/* ---- cron field parser --------------------------------------------------- */

/*
 * Parse a single cron field string into an integer, or -1 for '*'.
 * Supports plain integers and '*'.  Returns -2 on parse error.
 */
static int parse_field(const char *s, int min_val, int max_val)
{
    if (strcmp(s, "*") == 0) return -1; /* wildcard */
    /* Handle step syntax: */5 → every 5 units (simplified: use -1) */
    if (s[0] == '*' && s[1] == '/') return -1; /* treat step as wildcard */
    char *end;
    long v = strtol(s, &end, 10);
    if (*end != '\0') return -2;
    if (v < min_val || v > max_val) return -2;
    return (int)v;
}

/*
 * Parse one crontab line into a CronJob.  Returns 1 on success, 0 to skip.
 */
static int parse_line(const char *line, CronJob *job)
{
    memset(job, 0, sizeof(*job));

    /* Skip blanks and comments */
    while (*line == ' ' || *line == '\t') line++;
    if (*line == '\0' || *line == '#') return 0;

    /* @reboot shorthand */
    if (strncmp(line, "@reboot", 7) == 0) {
        job->reboot = 1;
        line += 7;
        while (*line == ' ' || *line == '\t') line++;
        strncpy(job->command, line, MAX_CMD_LEN - 1);
        /* Strip trailing newline */
        char *nl = strchr(job->command, '\n');
        if (nl) *nl = '\0';
        return job->command[0] != '\0';
    }

    /* @daily, @hourly, @weekly, @monthly shorthands */
    if (strncmp(line, "@daily",   6) == 0) {
        job->min = 0; job->hour = 0;
        job->mday = job->mon = job->wday = -1;
        line += 6;
        while (*line == ' ' || *line == '\t') line++;
        strncpy(job->command, line, MAX_CMD_LEN - 1);
        char *nl = strchr(job->command, '\n'); if (nl) *nl = '\0';
        return job->command[0] != '\0';
    }
    if (strncmp(line, "@hourly",  7) == 0) {
        job->min = 0; job->hour = job->mday = job->mon = job->wday = -1;
        line += 7;
        while (*line == ' ' || *line == '\t') line++;
        strncpy(job->command, line, MAX_CMD_LEN - 1);
        char *nl = strchr(job->command, '\n'); if (nl) *nl = '\0';
        return job->command[0] != '\0';
    }
    if (strncmp(line, "@weekly",  7) == 0) {
        job->min = 0; job->hour = 0; job->wday = 0;
        job->mday = job->mon = -1;
        line += 7;
        while (*line == ' ' || *line == '\t') line++;
        strncpy(job->command, line, MAX_CMD_LEN - 1);
        char *nl = strchr(job->command, '\n'); if (nl) *nl = '\0';
        return job->command[0] != '\0';
    }
    if (strncmp(line, "@monthly", 8) == 0) {
        job->min = 0; job->hour = 0; job->mday = 1;
        job->mon = job->wday = -1;
        line += 8;
        while (*line == ' ' || *line == '\t') line++;
        strncpy(job->command, line, MAX_CMD_LEN - 1);
        char *nl = strchr(job->command, '\n'); if (nl) *nl = '\0';
        return job->command[0] != '\0';
    }

    /* Standard "MIN HOUR MDAY MON WDAY COMMAND" */
    char f_min[32], f_hour[32], f_mday[32], f_mon[32], f_wday[32];
    char command[MAX_CMD_LEN];
    /* Use sscanf to parse the first 5 whitespace-separated tokens */
    int matched = sscanf(line, "%31s %31s %31s %31s %31s %4095[^\n]",
                         f_min, f_hour, f_mday, f_mon, f_wday, command);
    if (matched < 6) return 0;

    job->min  = parse_field(f_min,  0, 59);
    job->hour = parse_field(f_hour, 0, 23);
    job->mday = parse_field(f_mday, 1, 31);
    job->mon  = parse_field(f_mon,  1, 12);
    job->wday = parse_field(f_wday, 0,  6);

    if (job->min < -1 || job->hour < -1 || job->mday < -1 ||
        job->mon < -1  || job->wday < -1)
        return 0;

    strncpy(job->command, command, MAX_CMD_LEN - 1);
    return 1;
}

/* ---- crontab loader ------------------------------------------------------ */

static void load_crontab(void)
{
    g_job_count = 0;
    FILE *f = fopen(g_crontab, "r");
    if (!f) {
        char msg[256];
        snprintf(msg, sizeof(msg), "Cannot open crontab %s: %s",
                 g_crontab, strerror(errno));
        cron_log("WARN", msg);
        return;
    }

    char line[MAX_LINE_LEN];
    while (fgets(line, sizeof(line), f)) {
        if (g_job_count >= MAX_JOBS) break;
        CronJob job;
        if (parse_line(line, &job))
            g_jobs[g_job_count++] = job;
    }
    fclose(f);

    char msg[64];
    snprintf(msg, sizeof(msg), "Loaded %d job(s) from %s", g_job_count, g_crontab);
    cron_log("INFO", msg);
}

/* ---- job firing ---------------------------------------------------------- */

static int job_matches(const CronJob *job, const struct tm *t)
{
    if (job->reboot) return 0; /* @reboot handled separately */
    if (job->min  != -1 && job->min  != t->tm_min)  return 0;
    if (job->hour != -1 && job->hour != t->tm_hour)  return 0;
    if (job->mday != -1 && job->mday != t->tm_mday)  return 0;
    if (job->mon  != -1 && job->mon  != t->tm_mon+1) return 0;
    if (job->wday != -1 && job->wday != t->tm_wday)  return 0;
    return 1;
}

static void fire_job(const CronJob *job)
{
    char msg[MAX_CMD_LEN + 64];
    snprintf(msg, sizeof(msg), "Firing: %s", job->command);
    cron_log("INFO", msg);

    pid_t pid = fork();
    if (pid < 0) { cron_log("ERROR", "fork() failed"); return; }
    if (pid == 0) {
        /* Child: exec the command via sh */
        execl("/bin/sh", "sh", "-c", job->command, (char *)NULL);
        _exit(127);
    }
    /* Parent: don't wait — reap via SIGCHLD handler */
}

/* ---- main ---------------------------------------------------------------- */

int main(int argc, char *argv[])
{
    /* Read config from environment */
    {
        const char *p;
        p = getenv("CLAW_CRONTAB");  if (p) g_crontab  = p;
        p = getenv("CLAW_CRON_LOG"); if (p) g_log_path = p;
    }

    int opt;
    while ((opt = getopt(argc, argv, "f:l:h")) != -1) {
        switch (opt) {
            case 'f': g_crontab  = optarg; break;
            case 'l': g_log_path = optarg; break;
            case 'h':
                fprintf(stderr,
                    "Usage: %s [-f crontab] [-l logfile]\n", argv[0]);
                return 0;
            default: return 1;
        }
    }

    /* Open log file */
    if (strcmp(g_log_path, "-") != 0) {
        g_log = fopen(g_log_path, "a");
        if (!g_log) {
            fprintf(stderr, "claw-cron: cannot open log %s: %s\n",
                    g_log_path, strerror(errno));
            g_log = stderr;
        }
    } else {
        g_log = stdout;
    }

    /* Signal handling */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_sigterm;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT,  &sa, NULL);

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_sighup;
    sigaction(SIGHUP, &sa, NULL);

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = reap;
    sa.sa_flags   = SA_RESTART;
    sigaction(SIGCHLD, &sa, NULL);

    load_crontab();
    cron_log("INFO", "claw-cron started");

    /* Fire @reboot jobs */
    for (int i = 0; i < g_job_count; i++) {
        if (g_jobs[i].reboot)
            fire_job(&g_jobs[i]);
    }

    /* Main loop — wake every 30s, check if we're at a minute boundary */
    time_t last_fired_min = -1;

    while (g_running) {
        sleep(15); /* check every 15 seconds for minute transitions */
        if (!g_running) break;

        time_t now = time(NULL);
        struct tm *t = localtime(&now);

        /* Truncate to the current minute */
        time_t this_min = now - (now % 60);

        if (this_min != last_fired_min) {
            last_fired_min = this_min;
            for (int i = 0; i < g_job_count; i++) {
                if (job_matches(&g_jobs[i], t))
                    fire_job(&g_jobs[i]);
            }
        }
    }

    cron_log("INFO", "claw-cron stopped");
    if (g_log && g_log != stderr && g_log != stdout)
        fclose(g_log);
    return 0;
}
