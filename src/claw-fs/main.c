/*
 * claw-fs — Filesystem skill binary for claw-linux.
 *
 * Protocol (stdin → stdout, one JSON object each):
 *
 *   Read a file:
 *     IN:  { "op": "read",  "path": "/workspace/file.txt" }
 *     OUT: { "ok": true, "content": "<file contents>" }
 *
 *   Write a file:
 *     IN:  { "op": "write", "path": "/workspace/out.txt", "content": "data\n" }
 *     OUT: { "ok": true }
 *
 *   List a directory:
 *     IN:  { "op": "list",  "path": "/workspace" }
 *     OUT: { "ok": true, "entries": ["file.txt", "subdir/"] }
 *
 *   Error:
 *     OUT: { "ok": false, "error": "<reason>" }
 *
 * Path safety
 * -----------
 * Every requested path is resolved with realpath(3) before any I/O.
 * The resolved path must start with one of the configured ALLOWED prefixes
 * (hard-coded defaults; override at build time via -DALLOWED_PATHS=...).
 * Write operations are additionally checked against a READ_ONLY list.
 *
 * Build:
 *   cc -O2 -Wall -Wextra -o claw-fs main.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "../common/claw_json.h"

/* ---- tunables ------------------------------------------------------------ */
#define MAX_INPUT_BYTES    65536
#define MAX_PATH_BYTES      4096
#define MAX_OP_BYTES          32
#define MAX_CONTENT_BYTES (4 * 1024 * 1024)  /* 4 MiB read cap */

/*
 * Allowed path prefixes for all operations.
 * Paths that do not start with one of these are rejected.
 * Override at compile time: -DALLOWED_PATHS='"/workspace", "/var/lib/claw"'
 */
#ifndef ALLOWED_PATHS
#define ALLOWED_PATHS "/workspace", "/var/lib/claw"
#endif

/*
 * Read-only path prefixes — write operations to these are rejected even if
 * they fall within an ALLOWED prefix.
 */
#ifndef READONLY_PATHS
#define READONLY_PATHS "/etc", "/usr", "/bin", "/sbin", "/lib", "/proc", "/sys"
#endif

static const char *ALLOWED[]  = { ALLOWED_PATHS,  NULL };
static const char *READONLY[] = { READONLY_PATHS, NULL };

/* ---- helpers ------------------------------------------------------------- */

static void emit_error(const char *msg)
{
    char esc[4096];
    json_escape(msg, esc, sizeof(esc));
    printf("{\"ok\":false,\"error\":\"%s\"}\n", esc);
    fflush(stdout);
}

/*
 * Resolve `path` with realpath(3) and verify it starts with an ALLOWED prefix.
 * Populates `resolved` (PATH_MAX bytes).  Returns 1 on success, 0 on failure.
 */
static int resolve_and_check(const char *path, char *resolved, int write_op)
{
    /* realpath requires the path to exist for write ops we check the parent */
    if (write_op) {
        /* Resolve parent directory, then re-append the basename */
        char tmp[MAX_PATH_BYTES];
        strncpy(tmp, path, sizeof(tmp) - 1);
        tmp[sizeof(tmp) - 1] = '\0';

        char *slash = strrchr(tmp, '/');
        const char *basename_part = slash ? slash + 1 : tmp;
        if (slash) *slash = '\0';
        else { tmp[0] = '.'; tmp[1] = '\0'; }

        char parent_resolved[PATH_MAX];
        /* When tmp is empty the parent is the filesystem root ("/"),
         * not the current working directory ("."). */
        if (!realpath(tmp[0] != '\0' ? tmp : "/", parent_resolved)) {
            emit_error("Cannot resolve path");
            return 0;
        }
        /* Use strncat to avoid format-truncation; resolved is PATH_MAX bytes */
        strncpy(resolved, parent_resolved, PATH_MAX - 1);
        resolved[PATH_MAX - 1] = '\0';
        size_t pr_len = strlen(resolved);
        if (pr_len + 1 + strlen(basename_part) < (size_t)PATH_MAX) {
            resolved[pr_len] = '/';
            strncpy(resolved + pr_len + 1, basename_part,
                    PATH_MAX - pr_len - 2);
            resolved[PATH_MAX - 1] = '\0';
        } else {
            emit_error("Resolved path too long");
            return 0;
        }
    } else {
        if (!realpath(path, resolved)) {
            char errmsg[256];
            snprintf(errmsg, sizeof(errmsg), "Cannot resolve path: %s",
                     strerror(errno));
            emit_error(errmsg);
            return 0;
        }
    }

    /* Check against allowed prefixes */
    int allowed = 0;
    for (int i = 0; ALLOWED[i]; i++) {
        size_t plen = strlen(ALLOWED[i]);
        if (strncmp(resolved, ALLOWED[i], plen) == 0 &&
            (resolved[plen] == '/' || resolved[plen] == '\0')) {
            allowed = 1;
            break;
        }
    }
    if (!allowed) {
        emit_error("Path not in allowed directories");
        return 0;
    }

    /* For write ops, also check read-only list */
    if (write_op) {
        for (int i = 0; READONLY[i]; i++) {
            size_t plen = strlen(READONLY[i]);
            if (strncmp(resolved, READONLY[i], plen) == 0 &&
                (resolved[plen] == '/' || resolved[plen] == '\0')) {
                emit_error("Path is in a read-only directory");
                return 0;
            }
        }
    }
    return 1;
}

/* ---- operations ---------------------------------------------------------- */

static void op_read(const char *path)
{
    char resolved[PATH_MAX];
    if (!resolve_and_check(path, resolved, 0)) return;

    int fd = open(resolved, O_RDONLY);
    if (fd < 0) {
        char msg[256];
        snprintf(msg, sizeof(msg), "open: %s", strerror(errno));
        emit_error(msg);
        return;
    }

    char *buf = malloc(MAX_CONTENT_BYTES + 1);
    if (!buf) { close(fd); emit_error("Memory allocation failed"); return; }

    ssize_t total = 0, n;
    while (total < MAX_CONTENT_BYTES) {
        n = read(fd, buf + total, (size_t)(MAX_CONTENT_BYTES - total));
        if (n <= 0) break;
        total += n;
    }
    close(fd);
    buf[total] = '\0';

    size_t esc_size = (size_t)total * 2 + 16;
    if (esc_size < 64) esc_size = 64;
    char *esc = malloc(esc_size);
    if (!esc) { free(buf); emit_error("Memory allocation failed"); return; }

    json_escape(buf, esc, esc_size);
    printf("{\"ok\":true,\"content\":\"%s\"}\n", esc);
    fflush(stdout);
    free(buf); free(esc);
}

static void op_write(const char *path, const char *content)
{
    char resolved[PATH_MAX];
    if (!resolve_and_check(path, resolved, 1)) return;

    int fd = open(resolved, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        char msg[256];
        snprintf(msg, sizeof(msg), "open: %s", strerror(errno));
        emit_error(msg);
        return;
    }

    size_t len = strlen(content);
    ssize_t written = write(fd, content, len);
    close(fd);

    if (written < 0 || (size_t)written != len) {
        emit_error("write failed");
        return;
    }
    printf("{\"ok\":true}\n");
    fflush(stdout);
}

static void op_list(const char *path)
{
    char resolved[PATH_MAX];
    if (!resolve_and_check(path, resolved, 0)) return;

    DIR *dir = opendir(resolved);
    if (!dir) {
        char msg[256];
        snprintf(msg, sizeof(msg), "opendir: %s", strerror(errno));
        emit_error(msg);
        return;
    }

    /* Build JSON array of entries */
    size_t buf_cap = 4096;
    char *buf = malloc(buf_cap);
    if (!buf) { closedir(dir); emit_error("Memory allocation failed"); return; }
    size_t pos = 0;
    int first = 1;

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;

        /* Determine if entry is a directory */
        char full[PATH_MAX];
        size_t rlen = strlen(resolved);
        size_t dlen = strlen(ent->d_name);
        if (rlen + 1 + dlen < sizeof(full)) {
            memcpy(full, resolved, rlen);
            full[rlen] = '/';
            memcpy(full + rlen + 1, ent->d_name, dlen + 1);
        } else {
            full[0] = '\0';
        }
        struct stat st;
        int is_dir = (stat(full, &st) == 0 && S_ISDIR(st.st_mode));

        /* Build display name: append "/" for directories */
        char display[NAME_MAX + 2];
        snprintf(display, sizeof(display), "%s%s",
                 ent->d_name, is_dir ? "/" : "");

        /* JSON-escape the name */
        char esc[NAME_MAX * 2 + 4];
        json_escape(display, esc, sizeof(esc));
        size_t entry_len = strlen(esc) + 4; /* "esc", */

        /* Grow buffer if needed */
        if (pos + entry_len + 16 > buf_cap) {
            buf_cap = buf_cap * 2 + entry_len + 16;
            char *tmp = realloc(buf, buf_cap);
            if (!tmp) { free(buf); closedir(dir);
                        emit_error("Memory allocation failed"); return; }
            buf = tmp;
        }

        if (!first) { buf[pos++] = ','; }
        buf[pos++] = '"';
        memcpy(buf + pos, esc, strlen(esc));
        pos += strlen(esc);
        buf[pos++] = '"';
        first = 0;
    }
    closedir(dir);
    buf[pos] = '\0';

    printf("{\"ok\":true,\"entries\":[%s]}\n", buf);
    fflush(stdout);
    free(buf);
}

/* ---- main ---------------------------------------------------------------- */

int main(void)
{
    /* Read JSON request from stdin */
    char input[MAX_INPUT_BYTES];
    size_t total = 0;
    ssize_t n;
    while (total < sizeof(input) - 1) {
        n = read(STDIN_FILENO, input + total, sizeof(input) - 1 - total);
        if (n <= 0) break;
        total += (size_t)n;
    }
    input[total] = '\0';

    /* Parse op and path */
    char op[MAX_OP_BYTES]     = {0};
    char path[MAX_PATH_BYTES] = {0};

    if (!json_get_string(input, "op", op, sizeof(op))) {
        emit_error("Missing or invalid 'op' field");
        return 0;
    }
    if (!json_get_string(input, "path", path, sizeof(path))) {
        emit_error("Missing or invalid 'path' field");
        return 0;
    }

    if (strcmp(op, "read") == 0) {
        op_read(path);
    } else if (strcmp(op, "write") == 0) {
        char *content = malloc(MAX_CONTENT_BYTES + 1);
        if (!content) { emit_error("Memory allocation failed"); return 1; }
        if (!json_get_string(input, "content", content, MAX_CONTENT_BYTES + 1)) {
            emit_error("Missing or invalid 'content' field for write op");
            free(content);
            return 0;
        }
        op_write(path, content);
        free(content);
    } else if (strcmp(op, "list") == 0) {
        op_list(path);
    } else {
        emit_error("Unknown op — expected: read | write | list");
    }

    return 0;
}
