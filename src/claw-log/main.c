/*
 * claw-log — Centralised log viewer/streamer for claw-linux.
 *
 * Tails and displays log files from /var/log/claw/.  Supports filtering by
 * service name and following new output in real time (like tail -f).
 *
 * Corresponds to: openclaw/openclaw src/logging/ (centralised log aggregator)
 *
 * Usage
 * -----
 *   claw-log [-s SERVICE] [-n LINES] [-f] [-d LOG_DIR]
 *
 * Options
 * -------
 *   -s SERVICE  Show only logs for the named service (gateway, channel, cron,
 *               agent, daemon, tui, …).  Default: all services.
 *   -n LINES    Number of trailing lines to show on start (default: 50).
 *   -f          Follow mode — keep printing new lines as they appear.
 *   -d DIR      Log directory (default: /var/log/claw).
 *   -h          Show this help.
 *
 * Log file format
 * ---------------
 *   Each service writes to /var/log/claw/<service>.log.
 *   Lines are prefixed with the service name and coloured for readability.
 *
 * Build:
 *   cc -O2 -Wall -Wextra -o claw-log main.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>

/* ---- tunables ------------------------------------------------------------ */
#define DEFAULT_LOG_DIR  "/var/log/claw"
#define DEFAULT_LINES    50
#define MAX_SERVICES     32
#define MAX_PATH_LEN    256
#define MAX_LINE_BYTES  4096
#define POLL_INTERVAL_MS 200   /* follow-mode poll interval */

/* ---- ANSI colours -------------------------------------------------------- */
#define ANSI_RESET   "\033[0m"
#define ANSI_BOLD    "\033[1m"
#define ANSI_DIM     "\033[2m"

/* One colour per service slot (cycles if more than 6 services) */
static const char *SERVICE_COLOURS[] = {
    "\033[1;36m",  /* cyan    — gateway */
    "\033[1;32m",  /* green   — channel */
    "\033[1;33m",  /* yellow  — cron    */
    "\033[1;35m",  /* magenta — agent   */
    "\033[1;34m",  /* blue    — daemon  */
    "\033[1;31m",  /* red     — tui     */
    "\033[1;37m",  /* white   — other   */
};
#define NUM_COLOURS ((int)(sizeof(SERVICE_COLOURS)/sizeof(SERVICE_COLOURS[0])))

/* ---- globals ------------------------------------------------------------- */
static volatile int g_running = 1;
static int          g_follow  = 0;
static int          g_isatty  = 0;

static void on_sigint(int s) { (void)s; g_running = 0; }

/* ---- service descriptor -------------------------------------------------- */

typedef struct {
    char  name[64];
    char  path[MAX_PATH_LEN];
    FILE *fp;
    long  offset;        /* last seek position (follow mode) */
    int   colour_idx;
} Service;

static Service g_svc[MAX_SERVICES];
static int     g_nsvc = 0;

/* ---- helpers ------------------------------------------------------------- */

static const char *svc_colour(int idx)
{
    if (!g_isatty) return "";
    return SERVICE_COLOURS[idx % NUM_COLOURS];
}

/*
 * Scan log_dir for *.log files, optionally filtering to filter_name.
 * Populates g_svc[] and g_nsvc.
 */
static int discover_services(const char *log_dir, const char *filter_name)
{
    DIR *d = opendir(log_dir);
    if (!d) {
        fprintf(stderr, "claw-log: cannot open log directory %s: %s\n",
                log_dir, strerror(errno));
        return -1;
    }

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL && g_nsvc < MAX_SERVICES) {
        /* only *.log files */
        size_t nlen = strlen(ent->d_name);
        if (nlen < 5 || strcmp(ent->d_name + nlen - 4, ".log") != 0)
            continue;

        /* extract service name (strip .log) */
        char svc_name[64] = {0};
        size_t svc_len = nlen - 4;
        if (svc_len >= sizeof(svc_name)) svc_len = sizeof(svc_name) - 1;
        memcpy(svc_name, ent->d_name, svc_len);
        svc_name[svc_len] = '\0';

        /* apply filter if given */
        if (filter_name && strcmp(svc_name, filter_name) != 0)
            continue;

        Service *s = &g_svc[g_nsvc];
        memcpy(s->name, svc_name, sizeof(s->name));
        snprintf(s->path, sizeof(s->path), "%s/%s", log_dir, ent->d_name);
        s->fp         = NULL;
        s->offset     = 0;
        s->colour_idx = g_nsvc;
        g_nsvc++;
    }
    closedir(d);
    return g_nsvc;
}

/*
 * Count lines in file and return the file offset where the last n_lines
 * begin (or 0 if the file has fewer lines).
 */
static long tail_offset(FILE *fp, int n_lines)
{
    fseek(fp, 0, SEEK_END);
    long end = ftell(fp);
    if (end <= 0) return 0;

    int  lines_seen = 0;
    long pos        = end - 1;

    while (pos >= 0 && lines_seen <= n_lines) {
        fseek(fp, pos, SEEK_SET);
        int c = fgetc(fp);
        if (c == '\n') lines_seen++;
        pos--;
    }
    return (lines_seen > n_lines) ? pos + 2 : 0;
}

/*
 * Print lines from fp starting at offset, prefixed with service label.
 * Returns the new file offset.
 */
static long print_from(FILE *fp, long offset, const char *svc_name,
                        const char *colour)
{
    fseek(fp, offset, SEEK_SET);
    char line[MAX_LINE_BYTES];
    const char *reset = g_isatty ? ANSI_RESET : "";
    const char *dim   = g_isatty ? ANSI_DIM   : "";

    while (fgets(line, sizeof(line), fp)) {
        /* strip trailing newline for uniform formatting */
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n') line[--len] = '\0';

        printf("%s[%s]%s %s%s%s\n",
               colour, svc_name, reset,
               dim, line, reset);
    }
    long pos = ftell(fp);
    return pos;
}

/* ---- usage --------------------------------------------------------------- */

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [OPTIONS]\n"
        "\n"
        "Options:\n"
        "  -s SERVICE   Show only the named service (e.g. gateway, agent)\n"
        "  -n LINES     Trailing lines to show on start (default: %d)\n"
        "  -f           Follow mode — stream new log lines in real time\n"
        "  -d DIR       Log directory (default: %s)\n"
        "  -h           Show this help\n",
        prog, DEFAULT_LINES, DEFAULT_LOG_DIR);
}

/* ---- main ---------------------------------------------------------------- */

int main(int argc, char *argv[])
{
    const char *log_dir     = DEFAULT_LOG_DIR;
    const char *filter_name = NULL;
    int         n_lines     = DEFAULT_LINES;

    int opt;
    while ((opt = getopt(argc, argv, "s:n:fd:h")) != -1) {
        switch (opt) {
            case 's': filter_name = optarg;         break;
            case 'n': n_lines     = atoi(optarg);   break;
            case 'f': g_follow    = 1;               break;
            case 'd': log_dir     = optarg;          break;
            case 'h': usage(argv[0]); return 0;
            default:  usage(argv[0]); return 1;
        }
    }

    g_isatty = isatty(STDOUT_FILENO);

    signal(SIGINT,  on_sigint);
    signal(SIGTERM, on_sigint);
    signal(SIGPIPE, SIG_IGN);

    if (discover_services(log_dir, filter_name) < 0)
        return 1;

    if (g_nsvc == 0) {
        if (filter_name)
            fprintf(stderr, "claw-log: no log file found for service '%s' in %s\n",
                    filter_name, log_dir);
        else
            fprintf(stderr, "claw-log: no log files found in %s\n", log_dir);
        return 1;
    }

    /* Open all log files and seek to tail start position */
    for (int i = 0; i < g_nsvc; i++) {
        g_svc[i].fp = fopen(g_svc[i].path, "r");
        if (!g_svc[i].fp) {
            fprintf(stderr, "claw-log: cannot open %s: %s\n",
                    g_svc[i].path, strerror(errno));
            continue;
        }
        long off = tail_offset(g_svc[i].fp, n_lines);
        g_svc[i].offset = print_from(g_svc[i].fp, off,
                                     g_svc[i].name,
                                     svc_colour(g_svc[i].colour_idx));
    }
    fflush(stdout);

    if (!g_follow) goto done;

    /* Follow mode: poll each file for new lines */
    if (g_isatty)
        fprintf(stderr, "claw-log: following %d service(s) — Ctrl-C to stop\n",
                g_nsvc);

    while (g_running) {
        int any_new = 0;
        for (int i = 0; i < g_nsvc; i++) {
            if (!g_svc[i].fp) continue;
            /* Check if the file grew */
            struct stat st;
            if (stat(g_svc[i].path, &st) < 0) continue;
            if ((long)st.st_size > g_svc[i].offset) {
                g_svc[i].offset = print_from(g_svc[i].fp, g_svc[i].offset,
                                             g_svc[i].name,
                                             svc_colour(g_svc[i].colour_idx));
                any_new = 1;
            }
        }
        if (any_new) fflush(stdout);
        struct timespec ts = { 0, POLL_INTERVAL_MS * 1000000L };
        nanosleep(&ts, NULL);
    }

done:
    for (int i = 0; i < g_nsvc; i++)
        if (g_svc[i].fp) fclose(g_svc[i].fp);

    return 0;
}
