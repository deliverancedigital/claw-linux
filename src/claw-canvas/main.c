/*
 * claw-canvas — Canvas host for the claw-linux XFCE desktop.
 *
 * Provides an agent-controlled visual surface.  Accepts HTML, SVG, or plain
 * text content from the agent and displays it in the XFCE desktop environment
 * by writing to a temporary file and launching it in the system browser via
 * xdg-open.  On headless systems the content is written to stdout.
 *
 * Corresponds to: openclaw/openclaw src/canvas-host/
 *
 * Usage (CLI)
 * -----------
 *   claw-canvas [OPTIONS] [FILE]
 *
 * Options
 * -------
 *   -t TYPE    Content type: html (default), svg, text
 *   -T TITLE   Window/tab title
 *   -o FILE    Write canvas file to FILE instead of a temp path
 *   -p         Print the canvas file path to stdout instead of opening it
 *   -h         Show this help
 *
 * Usage (JSON skill, stdin → stdout)
 * -----------------------------------
 *   Input:
 *     {"op":"show","content":"<h1>Hello</h1>","type":"html","title":"My Canvas"}
 *     {"op":"show","file":"/path/to/existing.html"}
 *     {"op":"clear"}
 *     {"op":"status"}
 *
 *   Output:
 *     {"ok":true,"op":"show","file":"/tmp/claw-canvas-XXXX.html","pid":1234}
 *
 * Canvas file location
 * --------------------
 *   Temporary canvas files are written to /tmp/claw-canvas-*.html (or .svg
 *   or .txt) and opened in the browser.  The path is returned in the response
 *   so the agent can update it later.
 *
 * Build:
 *   cc -O2 -Wall -Wextra -o claw-canvas main.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "../common/claw_json.h"

/* ---- tunables ------------------------------------------------------------ */
#define MAX_INPUT_BYTES   (256 * 1024)  /* 256 KiB for content */
#define MAX_CONTENT_BYTES (256 * 1024)
#define MAX_PATH_BYTES     512
#define MAX_TITLE_BYTES    256
#define PID_FILE          "/var/run/claw/canvas.pid"

/* ---- HTML wrapper -------------------------------------------------------- */

static const char HTML_HEADER[] =
    "<!DOCTYPE html>\n"
    "<html>\n"
    "<head>\n"
    "  <meta charset=\"utf-8\">\n"
    "  <meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">\n"
    "  <title>%s</title>\n"
    "  <style>\n"
    "    body { font-family: system-ui, sans-serif; margin: 2em; line-height: 1.6;\n"
    "           background: #1e1e1e; color: #d4d4d4; }\n"
    "    h1,h2,h3 { color: #569cd6; }\n"
    "    code, pre { background: #252526; border-radius: 4px; padding: .2em .4em;\n"
    "                font-family: 'Cascadia Code', monospace; }\n"
    "    pre { padding: 1em; overflow: auto; }\n"
    "    a { color: #9cdcfe; }\n"
    "  </style>\n"
    "</head>\n"
    "<body>\n";

static const char HTML_FOOTER[] = "\n</body>\n</html>\n";

/* ---- helpers ------------------------------------------------------------- */

static int has_display(void)
{
    return (getenv("DISPLAY") != NULL || getenv("WAYLAND_DISPLAY") != NULL);
}

static int cmd_exists(const char *cmd)
{
    const char *dirs[] = { "/usr/bin", "/usr/local/bin", "/bin", NULL };
    char path[512];
    for (int i = 0; dirs[i]; i++) {
        snprintf(path, sizeof(path), "%s/%s", dirs[i], cmd);
        if (access(path, X_OK) == 0) return 1;
    }
    return 0;
}

/*
 * Write content to a temp file.  Returns a heap-allocated path. Caller must free().
 */
static char *write_canvas_file(const char *content, const char *type,
                               const char *title, const char *out_path)
{
    const char *ext = "html";
    if (strcmp(type, "svg")  == 0) ext = "svg";
    if (strcmp(type, "text") == 0) ext = "txt";

    char *path = NULL;
    int   fd   = -1;

    if (out_path && out_path[0]) {
        path = strdup(out_path);
        fd   = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    } else {
        path = malloc(MAX_PATH_BYTES);
        if (!path) return NULL;
        snprintf(path, MAX_PATH_BYTES, "/tmp/claw-canvas-XXXXXX.%s", ext);
        fd = mkstemps(path, (int)(strlen(ext) + 1));
    }

    if (fd < 0) { free(path); return NULL; }

    FILE *fp = fdopen(fd, "w");
    if (!fp) { close(fd); free(path); return NULL; }

    if (strcmp(type, "html") == 0) {
        fprintf(fp, HTML_HEADER, title && title[0] ? title : "Claw Canvas");
        fputs(content, fp);
        fputs(HTML_FOOTER, fp);
    } else if (strcmp(type, "svg") == 0) {
        fputs(content, fp);
    } else {
        /* Plain text — wrap in <pre> for readability if opened in a browser */
        fprintf(fp, HTML_HEADER, title && title[0] ? title : "Claw Canvas");
        fputs("<pre>", fp);
        /* Escape < and & for HTML */
        for (const char *p = content; *p; p++) {
            if (*p == '<')      fputs("&lt;", fp);
            else if (*p == '&') fputs("&amp;", fp);
            else                fputc(*p, fp);
        }
        fputs("</pre>", fp);
        fputs(HTML_FOOTER, fp);
    }

    fclose(fp);
    return path;
}

/*
 * Open a file with xdg-open (GUI) or print path to stdout (headless).
 * Returns the child PID (0 for headless).
 */
static pid_t open_canvas(const char *file_path)
{
    if (!has_display()) {
        fprintf(stdout, "Canvas file: %s\n", file_path);
        return 0;
    }

    const char *opener = NULL;
    if (cmd_exists("xdg-open"))    opener = "xdg-open";
    else if (cmd_exists("chromium")) opener = "chromium";
    else if (cmd_exists("firefox"))  opener = "firefox";

    if (!opener) {
        fprintf(stderr, "claw-canvas: no browser launcher found (install xdg-utils)\n");
        fprintf(stdout, "Canvas file: %s\n", file_path);
        return 0;
    }

    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        int dev_null = open("/dev/null", O_RDONLY);
        if (dev_null >= 0) { dup2(dev_null, STDIN_FILENO); close(dev_null); }
        const char *argv[] = { opener, file_path, NULL };
        execvp(opener, (char *const *)argv);
        _exit(127);
    }

    /* Write PID file */
    mkdir("/var/run/claw", 0755);
    FILE *pf = fopen(PID_FILE, "w");
    if (pf) { fprintf(pf, "%d\n", (int)pid); fclose(pf); }

    return pid;
}

/* ---- operations ---------------------------------------------------------- */

static int op_show(const char *content, const char *file_arg,
                   const char *type, const char *title,
                   const char *out_path, int print_only)
{
    char *canvas_path = NULL;

    if (file_arg && file_arg[0]) {
        /* Show an existing file */
        canvas_path = strdup(file_arg);
    } else if (content && content[0]) {
        canvas_path = write_canvas_file(content, type, title, out_path);
        if (!canvas_path) {
            puts("{\"ok\":false,\"error\":\"Failed to write canvas file\"}");
            return 1;
        }
    } else {
        puts("{\"ok\":false,\"error\":\"Missing 'content' or 'file' field\"}");
        return 1;
    }

    if (print_only) {
        char esc_path[MAX_PATH_BYTES * 2] = {0};
        json_escape(canvas_path, esc_path, sizeof(esc_path));
        printf("{\"ok\":true,\"op\":\"show\",\"file\":\"%s\",\"pid\":0}\n", esc_path);
        free(canvas_path);
        return 0;
    }

    pid_t pid = open_canvas(canvas_path);

    char esc_path[MAX_PATH_BYTES * 2] = {0};
    json_escape(canvas_path, esc_path, sizeof(esc_path));
    printf("{\"ok\":true,\"op\":\"show\",\"file\":\"%s\",\"pid\":%d}\n",
           esc_path, (int)pid);
    free(canvas_path);
    return 0;
}

static int op_clear(void)
{
    /* Read PID file and close the browser window, remove temp file */
    FILE *pf = fopen(PID_FILE, "r");
    if (pf) {
        int pid = -1;
        int r = fscanf(pf, "%d", &pid);
        (void)r;
        fclose(pf);
        if (pid > 0) kill((pid_t)pid, SIGTERM);
        unlink(PID_FILE);
    }
    puts("{\"ok\":true,\"op\":\"clear\"}");
    return 0;
}

static int op_status(void)
{
    FILE *pf = fopen(PID_FILE, "r");
    if (!pf) {
        puts("{\"ok\":true,\"op\":\"status\",\"active\":false}");
        return 0;
    }
    int pid = -1;
    int r = fscanf(pf, "%d", &pid);
    (void)r;
    fclose(pf);
    /* Check if process is still running */
    int running = (pid > 0 && kill((pid_t)pid, 0) == 0);
    printf("{\"ok\":true,\"op\":\"status\",\"active\":%s,\"pid\":%d}\n",
           running ? "true" : "false", pid);
    return 0;
}

/* ---- usage --------------------------------------------------------------- */

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [OPTIONS] [FILE]\n"
        "       echo '{\"op\":\"show\",\"content\":\"<h1>Hi</h1>\",\"type\":\"html\"}' | %s\n"
        "\n"
        "Options:\n"
        "  FILE       HTML/SVG/text file to display\n"
        "  -t TYPE    Content type: html (default), svg, text\n"
        "  -T TITLE   Canvas title\n"
        "  -o FILE    Save canvas to FILE (instead of temp path)\n"
        "  -p         Print canvas file path only; do not open browser\n"
        "  -h         Show this help\n"
        "\n"
        "On XFCE desktop (DISPLAY set): opens content in xdg-open.\n"
        "Headless: prints the canvas file path to stdout.\n",
        prog, prog);
}

/* ---- main ---------------------------------------------------------------- */

int main(int argc, char *argv[])
{
    signal(SIGPIPE, SIG_IGN);

    /* JSON skill mode */
    if (argc == 1 && !isatty(STDIN_FILENO)) {
        char *input = malloc(MAX_INPUT_BYTES);
        if (!input) { puts("{\"ok\":false,\"error\":\"Out of memory\"}"); return 1; }

        size_t total = 0;
        ssize_t nr;
        while ((nr = read(STDIN_FILENO, input + total,
                          MAX_INPUT_BYTES - 1 - total)) > 0)
            total += (size_t)nr;
        input[total] = '\0';

        if (total == 0) {
            puts("{\"ok\":false,\"error\":\"No input\"}");
            free(input); return 1;
        }

        char op[32]                    = {0};
        char content[MAX_CONTENT_BYTES] = {0};
        char type[16]                  = "html";
        char title[MAX_TITLE_BYTES]    = {0};
        char file_arg[MAX_PATH_BYTES]  = {0};
        char out_path[MAX_PATH_BYTES]  = {0};

        json_get_string(input, "op",      op,       sizeof(op));
        json_get_string(input, "content", content,  sizeof(content));
        json_get_string(input, "type",    type,     sizeof(type));
        json_get_string(input, "title",   title,    sizeof(title));
        json_get_string(input, "file",    file_arg, sizeof(file_arg));
        json_get_string(input, "output",  out_path, sizeof(out_path));
        free(input);

        if (!op[0] || strcmp(op, "show") == 0)
            return op_show(content, file_arg, type, title, out_path, 0);
        if (strcmp(op, "clear")  == 0) return op_clear();
        if (strcmp(op, "status") == 0) return op_status();

        char esc[64] = {0};
        json_escape(op, esc, sizeof(esc));
        printf("{\"ok\":false,\"error\":\"Unknown op '%s' — use show|clear|status\"}\n", esc);
        return 1;
    }

    /* CLI mode */
    char type[16]               = "html";
    char title[MAX_TITLE_BYTES] = "Claw Canvas";
    char out_path[MAX_PATH_BYTES] = {0};
    int  print_only = 0;

    int opt;
    while ((opt = getopt(argc, argv, "t:T:o:ph")) != -1) {
        switch (opt) {
            case 't': snprintf(type,     sizeof(type),     "%s", optarg); break;
            case 'T': snprintf(title,    sizeof(title),    "%s", optarg); break;
            case 'o': snprintf(out_path, sizeof(out_path), "%s", optarg); break;
            case 'p': print_only = 1; break;
            case 'h': usage(argv[0]); return 0;
            default:  usage(argv[0]); return 1;
        }
    }

    if (optind < argc) {
        /* Treat positional arg as existing file to open */
        return op_show(NULL, argv[optind], type, title, out_path, print_only);
    }

    /* Read content from stdin */
    char *content = malloc(MAX_CONTENT_BYTES);
    if (!content) { fprintf(stderr, "claw-canvas: out of memory\n"); return 1; }
    size_t total = 0;
    ssize_t nr;
    while ((nr = read(STDIN_FILENO, content + total,
                      MAX_CONTENT_BYTES - 1 - total)) > 0)
        total += (size_t)nr;
    content[total] = '\0';

    if (total == 0) {
        fprintf(stderr, "claw-canvas: no content — pipe HTML/SVG/text or provide a FILE\n");
        usage(argv[0]);
        free(content);
        return 1;
    }

    int rc = op_show(content, NULL, type, title, out_path, print_only);
    free(content);
    return rc;
}
