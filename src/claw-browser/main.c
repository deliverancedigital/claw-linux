/*
 * claw-browser — Browser integration for claw-linux.
 *
 * Opens URLs in a web browser.  On XFCE desktop deployments it uses
 * xdg-open to hand off to the user's preferred browser (Chromium, Firefox,
 * etc.).  On headless deployments it falls back to a text-mode browser
 * (w3m, lynx, elinks) if available, or to claw-fetch for raw content.
 *
 * Corresponds to: openclaw/openclaw src/browser/ (browser automation)
 *
 * Usage (CLI)
 * -----------
 *   claw-browser <URL>
 *   claw-browser -b chromium <URL>   — force a specific browser
 *   claw-browser -d <URL>            — dump page text to stdout (headless)
 *   claw-browser -h                  — show this help
 *
 * Usage (JSON skill, stdin → stdout)
 * -----------------------------------
 *   Input:
 *     {"url":"https://example.com","browser":"auto","dump":false}
 *
 *   Fields:
 *     url      — URL to open (required)
 *     browser  — "auto" (default), "xdg-open", "chromium", "firefox",
 *                "w3m", "lynx", "elinks"
 *     dump     — if true, write page text to stdout (default: false)
 *
 *   Output:
 *     {"ok":true,"url":"https://example.com","browser":"xdg-open"}
 *     {"ok":false,"error":"..."}
 *
 * Browser selection logic
 * -----------------------
 *   1. If $DISPLAY or $WAYLAND_DISPLAY is set (GUI available):
 *        xdg-open > chromium > chromium-browser > firefox > surf
 *   2. Headless fallback:
 *        w3m > lynx > elinks
 *   3. If no browser found and dump=true: use claw-fetch
 *
 * Build:
 *   cc -O2 -Wall -Wextra -o claw-browser main.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/wait.h>

#include "../common/claw_json.h"

/* ---- tunables ------------------------------------------------------------ */
#define MAX_INPUT_BYTES  4096
#define MAX_URL_BYTES    2048
#define MAX_BROWSER_BYTES  64

/* ---- helpers ------------------------------------------------------------- */

/* Return 1 if a command exists in PATH. */
static int cmd_exists(const char *cmd)
{
    char path[512];
    /* Check common binary dirs */
    const char *dirs[] = {
        "/usr/bin", "/usr/local/bin", "/bin",
        "/usr/sbin", "/usr/local/sbin", NULL
    };
    for (int i = 0; dirs[i]; i++) {
        snprintf(path, sizeof(path), "%s/%s", dirs[i], cmd);
        if (access(path, X_OK) == 0) return 1;
    }
    /* Also try PATH via execvp with /dev/null stdin */
    return 0;
}

static int has_display(void)
{
    return (getenv("DISPLAY") != NULL || getenv("WAYLAND_DISPLAY") != NULL);
}

/*
 * Select the best available browser.
 * Returns a static string with the browser command, or NULL if none found.
 */
static const char *select_browser(const char *requested)
{
    if (requested && strcmp(requested, "auto") != 0) {
        /* User explicitly requested a browser — use it as-is */
        return requested;
    }

    if (has_display()) {
        /* GUI browsers in preference order */
        static const char *gui[] = {
            "xdg-open", "chromium", "chromium-browser",
            "google-chrome", "firefox", "surf", NULL
        };
        for (int i = 0; gui[i]; i++)
            if (cmd_exists(gui[i])) return gui[i];
    }

    /* Text-mode browsers */
    static const char *tui_browsers[] = { "w3m", "lynx", "elinks", NULL };
    for (int i = 0; tui_browsers[i]; i++)
        if (cmd_exists(tui_browsers[i])) return tui_browsers[i];

    return NULL;
}

/*
 * Open a URL in a browser.
 * In background mode (GUI), fork+exec without waiting.
 * In text mode (TUI), wait for the child to exit.
 * Returns 0 on success.
 */
static int open_url(const char *browser, const char *url, int wait_for_child)
{
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        /* Child */
        const char *argv[] = { browser, url, NULL };
        execvp(browser, (char *const *)argv);
        _exit(127);
    }

    if (wait_for_child) {
        int status = 0;
        waitpid(pid, &status, 0);
        if (WIFEXITED(status) && WEXITSTATUS(status) == 127) return -1;
        return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
    }
    /* Background: don't wait */
    return 0;
}

/*
 * Dump page via claw-fetch as fallback (raw HTTP content to stdout).
 */
static int dump_via_fetch(const char *url)
{
    const char *fetch = "/usr/local/bin/claw-fetch";
    if (access(fetch, X_OK) != 0) fetch = "claw-fetch";

    /* JSON-escape the URL before embedding it in the JSON payload */
    char esc_url[MAX_URL_BYTES * 2] = {0};
    json_escape(url, esc_url, sizeof(esc_url));

    char input[sizeof(esc_url) + 16];
    snprintf(input, sizeof(input), "{\"url\":\"%s\"}", esc_url);

    /* Write to temp file, then exec claw-fetch */
    char tmp[] = "/tmp/claw-browser-XXXXXX";
    int tfd = mkstemp(tmp);
    if (tfd < 0) return -1;
    ssize_t nwr1 = write(tfd, input, strlen(input));
    ssize_t nwr2 = write(tfd, "\n", 1);
    (void)nwr1; (void)nwr2;
    close(tfd);

    pid_t pid = fork();
    if (pid < 0) { unlink(tmp); return -1; }
    if (pid == 0) {
        int fd = open(tmp, O_RDONLY);
        if (fd >= 0) { dup2(fd, STDIN_FILENO); close(fd); }
        const char *argv[] = { fetch, NULL };
        execvp(fetch, (char *const *)argv);
        _exit(127);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    unlink(tmp);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}

/* ---- skill mode ---------------------------------------------------------- */

static int skill_mode(void)
{
    char input[MAX_INPUT_BYTES] = {0};
    if (!fgets(input, sizeof(input), stdin)) {
        puts("{\"ok\":false,\"error\":\"No input\"}");
        return 1;
    }

    char url[MAX_URL_BYTES]        = {0};
    char browser_req[MAX_BROWSER_BYTES] = "auto";
    long dump = 0;

    if (!json_get_string(input, "url", url, sizeof(url)) || url[0] == '\0') {
        puts("{\"ok\":false,\"error\":\"Missing 'url' field\"}");
        return 1;
    }
    json_get_string(input, "browser", browser_req, sizeof(browser_req));
    dump = json_get_long(input, "dump", 0);

    if (dump) {
        /* Dump mode: use text browser in non-interactive dump mode.
         * Stdout is reserved for page content. Do not print JSON status
         * lines here to avoid mixing them with the dump output. */
        const char *br = select_browser("w3m");  /* prefer w3m for dump */
        if (!br) br = select_browser(NULL);
        if (br && strcmp(br, "xdg-open") != 0) {
            /* Build -dump argv for each known text browser */
            pid_t pid = fork();
            if (pid < 0) {
                fprintf(stderr, "claw-browser: fork failed: %s\n", strerror(errno));
                return 1;
            }
            if (pid == 0) {
                if (strcmp(br, "w3m") == 0) {
                    const char *av[] = { "w3m", "-dump", url, NULL };
                    execvp("w3m", (char *const *)av);
                } else if (strcmp(br, "lynx") == 0) {
                    const char *av[] = { "lynx", "-dump", url, NULL };
                    execvp("lynx", (char *const *)av);
                } else if (strcmp(br, "elinks") == 0) {
                    const char *av[] = { "elinks", "-dump", url, NULL };
                    execvp("elinks", (char *const *)av);
                } else {
                    const char *av[] = { br, url, NULL };
                    execvp(br, (char *const *)av);
                }
                _exit(127);
            }
            int st = 0;
            waitpid(pid, &st, 0);
            return WIFEXITED(st) ? WEXITSTATUS(st) : 1;
        }
        /* Fallback: claw-fetch (raw HTTP) */
        int rc = dump_via_fetch(url);
        if (rc != 0)
            fprintf(stderr, "claw-browser: no suitable text browser or claw-fetch available\n");
        return rc;
    }

    const char *browser = select_browser(browser_req);
    if (!browser) {
        puts("{\"ok\":false,\"error\":\"No browser found — install xdg-utils, w3m, or lynx\"}");
        return 1;
    }

    /* GUI browsers run in background; TUI browsers wait */
    int wait = (strcmp(browser, "xdg-open") != 0 && !has_display());
    int rc = open_url(browser, url, wait);

    char esc_url[MAX_URL_BYTES*2]   = {0};
    char esc_br[MAX_BROWSER_BYTES*2] = {0};
    json_escape(url,     esc_url, sizeof(esc_url));
    json_escape(browser, esc_br,  sizeof(esc_br));

    if (rc < 0 || rc == 127) {
        printf("{\"ok\":false,\"error\":\"Failed to launch '%s'\",\"url\":\"%s\"}\n",
               browser, esc_url);
        return 1;
    }
    printf("{\"ok\":true,\"url\":\"%s\",\"browser\":\"%s\"}\n", esc_url, esc_br);
    return 0;
}

/* ---- usage --------------------------------------------------------------- */

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [OPTIONS] <URL>\n"
        "       echo '{\"url\":\"https://...\"}' | %s\n"
        "\n"
        "Options:\n"
        "  -b BROWSER   Browser to use: auto (default), xdg-open, chromium,\n"
        "               firefox, w3m, lynx, elinks\n"
        "  -d           Dump page text to stdout (headless)\n"
        "  -h           Show this help\n"
        "\n"
        "On XFCE desktop (DISPLAY/WAYLAND_DISPLAY set): uses xdg-open.\n"
        "Headless fallback: w3m → lynx → elinks → claw-fetch.\n",
        prog, prog);
}

/* ---- main ---------------------------------------------------------------- */

int main(int argc, char *argv[])
{
    /* Skill mode: stdin not a tty and no args */
    if (argc == 1 && !isatty(STDIN_FILENO))
        return skill_mode();

    char browser_req[MAX_BROWSER_BYTES] = "auto";
    int  dump = 0;

    int opt;
    while ((opt = getopt(argc, argv, "b:dh")) != -1) {
        switch (opt) {
            case 'b': snprintf(browser_req, sizeof(browser_req), "%s", optarg); break;
            case 'd': dump = 1; break;
            case 'h': usage(argv[0]); return 0;
            default:  usage(argv[0]); return 1;
        }
    }

    if (optind >= argc) {
        fprintf(stderr, "claw-browser: no URL provided\n");
        usage(argv[0]);
        return 1;
    }

    const char *url = argv[optind];

    if (dump) {
        const char *br = select_browser("w3m");
        if (!br || strcmp(br, "xdg-open") == 0) br = NULL;
        if (br) return open_url(br, url, 1);
        return dump_via_fetch(url);
    }

    const char *browser = select_browser(browser_req);
    if (!browser) {
        fprintf(stderr, "claw-browser: no browser found — install xdg-utils, w3m, or lynx\n");
        return 1;
    }

    int wait = (strcmp(browser, "xdg-open") != 0 && !has_display());
    int rc   = open_url(browser, url, wait);
    if (rc == 127) {
        fprintf(stderr, "claw-browser: failed to launch '%s'\n", browser);
        return 1;
    }
    return rc;
}
