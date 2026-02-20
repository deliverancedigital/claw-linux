/*
 * claw-md — Markdown renderer for claw-linux.
 *
 * Reads a Markdown document from stdin or a file and writes an ANSI-styled
 * rendition to stdout for display in a terminal.  When stdout is not a
 * terminal the ANSI escapes are omitted so the output can be piped safely.
 *
 * Corresponds to: openclaw/openclaw src/markdown/ (Markdown renderer)
 *
 * Supported Markdown elements
 * ---------------------------
 *   # … ######     ATX headings (bold + underline, level 1 = large)
 *   **…** / __…__  Bold
 *   *…*  / _…_     Italic
 *   `…`            Inline code (reverse video)
 *   ```…```        Fenced code block (indented, dim)
 *   > …            Blockquote (│ prefix, italic)
 *   - / * / + …    Unordered list item (• bullet)
 *   1. …           Ordered list item
 *   ---            Horizontal rule
 *   [text](url)    Link (text highlighted, url dimmed)
 *
 * Usage
 * -----
 *   claw-md [FILE]          — render FILE (or stdin if omitted)
 *   claw-md -w WIDTH [FILE] — wrap at WIDTH columns (default: 80)
 *   claw-md -h              — show this help
 *
 * Build:
 *   cc -O2 -Wall -Wextra -o claw-md main.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <ctype.h>

/* ---- tunables ------------------------------------------------------------ */
#define MAX_LINE_BYTES  4096
#define DEFAULT_WIDTH    80

/* ---- ANSI helpers -------------------------------------------------------- */
#define A_RESET     "\033[0m"
#define A_BOLD      "\033[1m"
#define A_DIM       "\033[2m"
#define A_ITALIC    "\033[3m"
#define A_UNDERLINE "\033[4m"
#define A_REVERSE   "\033[7m"
#define A_CYAN      "\033[36m"
#define A_YELLOW    "\033[33m"
#define A_GREEN     "\033[32m"
#define A_BLUE      "\033[34m"
#define A_MAGENTA   "\033[35m"

static int g_color = 0;  /* 1 if stdout is a tty */

static const char *C(const char *code)
{
    return g_color ? code : "";
}

/* ---- state --------------------------------------------------------------- */
static int g_in_code_block  = 0;  /* inside ``` fence */
static int g_ordered_idx    = 0;  /* current ordered list counter */

/* ---- inline renderer ----------------------------------------------------- */

/*
 * Render inline spans (**bold**, *italic*, `code`, [link](url)) within a
 * line of text and write to stdout.
 */
static void render_inline(const char *s)
{
    while (*s) {
        /* Fenced inline code `...` */
        if (s[0] == '`' && s[1] != '`') {
            s++;
            printf("%s%s", C(A_REVERSE), C(A_CYAN));
            while (*s && *s != '`') { putchar(*s++); }
            printf("%s", C(A_RESET));
            if (*s == '`') s++;
            continue;
        }

        /* Bold **…** or __…__ */
        if ((s[0] == '*' && s[1] == '*') || (s[0] == '_' && s[1] == '_')) {
            char d = s[0]; s += 2;
            printf("%s%s", C(A_BOLD), C(A_YELLOW));
            while (*s) {
                if (s[0] == d && s[1] == d) { s += 2; break; }
                putchar(*s++);
            }
            printf("%s", C(A_RESET));
            continue;
        }

        /* Italic *…* or _…_ (single) */
        if ((s[0] == '*' || s[0] == '_') && s[1] != ' ') {
            char d = s[0]; s++;
            printf("%s", C(A_ITALIC));
            while (*s && *s != d) { putchar(*s++); }
            printf("%s", C(A_RESET));
            if (*s == d) s++;
            continue;
        }

        /* Link [text](url) */
        if (s[0] == '[') {
            const char *text_start = s + 1;
            const char *text_end   = strchr(text_start, ']');
            if (text_end && text_end[1] == '(') {
                const char *url_start = text_end + 2;
                const char *url_end   = strchr(url_start, ')');
                if (url_end) {
                    /* Print text */
                    printf("%s%s", C(A_BOLD), C(A_CYAN));
                    fwrite(text_start, 1, (size_t)(text_end - text_start), stdout);
                    printf("%s", C(A_RESET));
                    /* Print URL dimmed */
                    printf(" %s(", C(A_DIM));
                    fwrite(url_start, 1, (size_t)(url_end - url_start), stdout);
                    printf(")%s", C(A_RESET));
                    s = url_end + 1;
                    continue;
                }
            }
        }

        putchar(*s++);
    }
}

/* ---- line renderer ------------------------------------------------------- */

static void render_line(const char *line)
{
    /* Strip trailing newline */
    char buf[MAX_LINE_BYTES];
    size_t len = strlen(line);
    if (len >= sizeof(buf)) len = sizeof(buf) - 1;
    memcpy(buf, line, len);
    while (len > 0 && (buf[len-1] == '\n' || buf[len-1] == '\r')) len--;
    buf[len] = '\0';
    const char *s = buf;

    /* ── Code fence toggle ────────────────────────────────────────────── */
    if (strncmp(s, "```", 3) == 0) {
        g_in_code_block = !g_in_code_block;
        if (g_in_code_block) {
            /* Print language hint if present */
            const char *lang = s + 3;
            while (*lang == ' ') lang++;
            if (*lang)
                printf("%s┌─ %s%s\n", C(A_DIM), lang, C(A_RESET));
            else
                printf("%s┌─────────%s\n", C(A_DIM), C(A_RESET));
        } else {
            printf("%s└─────────%s\n", C(A_DIM), C(A_RESET));
        }
        return;
    }

    /* ── Inside code block ────────────────────────────────────────────── */
    if (g_in_code_block) {
        printf("%s│ %s%s\n", C(A_DIM), buf, C(A_RESET));
        return;
    }

    /* ── Horizontal rule ─────────────────────────────────────────────── */
    if (strcmp(s, "---") == 0 || strcmp(s, "***") == 0 || strcmp(s, "___") == 0 ||
        (len >= 3 && s[0] == '-' && s[1] == '-' && s[2] == '-')) {
        printf("%s%s%s\n", C(A_DIM),
               "────────────────────────────────────────────────────────────────────────────────",
               C(A_RESET));
        return;
    }

    /* ── ATX Headings # … ###### ─────────────────────────────────────── */
    if (s[0] == '#') {
        int level = 0;
        while (s[level] == '#') level++;
        const char *text = s + level;
        while (*text == ' ') text++;

        if (level == 1) {
            printf("\n%s%s%s%s%s\n\n",
                   C(A_BOLD), C(A_UNDERLINE), C(A_CYAN), text, C(A_RESET));
        } else if (level == 2) {
            printf("\n%s%s%s%s\n", C(A_BOLD), C(A_YELLOW), text, C(A_RESET));
            /* Underline with dashes */
            size_t tlen = strlen(text);
            for (size_t i = 0; i < tlen; i++) putchar('-');
            printf("\n");
        } else {
            printf("%s%s%s%s\n", C(A_BOLD), C(A_GREEN), text, C(A_RESET));
        }
        g_ordered_idx = 0;
        return;
    }

    /* ── Blockquote > … ──────────────────────────────────────────────── */
    if (s[0] == '>') {
        const char *text = s + 1;
        while (*text == ' ') text++;
        printf("%s│%s %s", C(A_DIM), C(A_ITALIC), C(A_RESET));
        render_inline(text);
        printf("%s\n", C(A_RESET));
        return;
    }

    /* ── Unordered list - / * / + ────────────────────────────────────── */
    if ((s[0] == '-' || s[0] == '*' || s[0] == '+') && s[1] == ' ') {
        const char *text = s + 2;
        printf("%s  • %s", C(A_BOLD), C(A_RESET));
        render_inline(text);
        putchar('\n');
        g_ordered_idx = 0;
        return;
    }

    /* ── Ordered list  1. 2. … ──────────────────────────────────────── */
    if (isdigit((unsigned char)s[0])) {
        const char *dot = strchr(s, '.');
        if (dot && dot[1] == ' ') {
            int idx = atoi(s);
            const char *text = dot + 2;
            if (idx == 1) g_ordered_idx = 1;
            else g_ordered_idx++;
            printf("%s  %d.%s ", C(A_BOLD), g_ordered_idx, C(A_RESET));
            render_inline(text);
            putchar('\n');
            return;
        }
    }

    /* ── Blank line ──────────────────────────────────────────────────── */
    if (len == 0) {
        putchar('\n');
        g_ordered_idx = 0;
        return;
    }

    /* ── Normal paragraph text ───────────────────────────────────────── */
    render_inline(s);
    putchar('\n');
}

/* ---- usage --------------------------------------------------------------- */

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [OPTIONS] [FILE]\n"
        "\n"
        "Render Markdown to the terminal with ANSI styling.\n"
        "\n"
        "Options:\n"
        "  FILE    Markdown file to render (default: read from stdin)\n"
        "  -h      Show this help\n",
        prog);
}

/* ---- main ---------------------------------------------------------------- */

int main(int argc, char *argv[])
{
    const char *filename = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(argv[0]); return 0;
        } else if (argv[i][0] != '-') {
            filename = argv[i];
        }
    }

    g_color = isatty(STDOUT_FILENO);

    FILE *fp = stdin;
    if (filename) {
        fp = fopen(filename, "r");
        if (!fp) {
            fprintf(stderr, "claw-md: cannot open '%s': %s\n",
                    filename, strerror(errno));
            return 1;
        }
    }

    char line[MAX_LINE_BYTES];
    while (fgets(line, sizeof(line), fp))
        render_line(line);

    /* Close open code block if file ended mid-fence */
    if (g_in_code_block)
        printf("%s└─────────%s\n", C(A_DIM), C(A_RESET));

    if (filename) fclose(fp);
    return 0;
}
