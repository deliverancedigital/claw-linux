/*
 * claw-plugin — Plugin loader and registry for claw-linux.
 *
 * Discovers, lists, inspects, and executes plugins stored under the plugin
 * directory.  Plugins are executable files (scripts or binaries) and may
 * have an optional JSON metadata sidecar (<name>.json) that describes their
 * interface.
 *
 * Corresponds to: openclaw/openclaw src/plugins/ (plugin registry and SDK)
 *
 * Plugin directory layout
 * -----------------------
 *   /opt/claw/plugins/
 *     my-plugin           ← executable (script or binary)
 *     my-plugin.json      ← optional metadata:
 *                            {
 *                              "name":        "my-plugin",
 *                              "version":     "1.0.0",
 *                              "description": "Does something useful",
 *                              "author":      "Alice",
 *                              "skill":       true   ← reads JSON from stdin
 *                            }
 *
 * Usage
 * -----
 *   claw-plugin list                 — list available plugins
 *   claw-plugin info <name>          — show plugin metadata
 *   claw-plugin run  <name> [args…]  — execute a plugin
 *   claw-plugin install <path>       — install a plugin from a path
 *   claw-plugin remove  <name>       — remove an installed plugin
 *
 * Configuration (environment variables)
 * --------------------------------------
 *   CLAW_PLUGIN_DIR   Plugin directory (default: /opt/claw/plugins)
 *
 * Build:
 *   cc -O2 -Wall -Wextra -o claw-plugin main.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "../common/claw_json.h"

/* ---- tunables ------------------------------------------------------------ */
#define DEFAULT_PLUGIN_DIR  "/opt/claw/plugins"
#define MAX_PLUGINS          128
#define MAX_NAME_LEN          64
#define MAX_PATH_LEN         256
#define MAX_META_BYTES      4096
#define MAX_DESC_BYTES       512

/* ---- plugin descriptor --------------------------------------------------- */

typedef struct {
    char  name[MAX_NAME_LEN];
    char  path[MAX_PATH_LEN];
    char  meta_path[MAX_PATH_LEN];
    int   has_meta;
    /* parsed metadata */
    char  version[32];
    char  description[MAX_DESC_BYTES];
    char  author[128];
    int   is_skill;   /* reads JSON from stdin if true */
} Plugin;

static Plugin g_plugins[MAX_PLUGINS];
static int    g_nplugins = 0;

/* ---- helpers ------------------------------------------------------------- */

static const char *plugin_dir(void)
{
    const char *env = getenv("CLAW_PLUGIN_DIR");
    return env ? env : DEFAULT_PLUGIN_DIR;
}

/*
 * Read file into a heap buffer.  Caller must free().
 */
static char *read_file(const char *path)
{
    FILE *fp = fopen(path, "r");
    if (!fp) return NULL;
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    rewind(fp);
    if (sz <= 0 || sz > MAX_META_BYTES) { fclose(fp); return NULL; }
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(fp); return NULL; }
    size_t nr = fread(buf, 1, (size_t)sz, fp);
    buf[nr] = '\0';
    fclose(fp);
    return buf;
}

/*
 * Ensure the plugin directory exists.
 */
static void ensure_plugin_dir(void)
{
    const char *dir = plugin_dir();
    char tmp[MAX_PATH_LEN];
    snprintf(tmp, sizeof(tmp), "%s", dir);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    mkdir(dir, 0755);
}

/*
 * Load metadata from a .json sidecar into p.
 */
static void load_meta(Plugin *p)
{
    char *buf = read_file(p->meta_path);
    if (!buf) return;
    p->has_meta = 1;
    json_get_string(buf, "version",     p->version,     sizeof(p->version));
    json_get_string(buf, "description", p->description, sizeof(p->description));
    json_get_string(buf, "author",      p->author,      sizeof(p->author));
    p->is_skill = (int)json_get_long(buf, "skill", 0);
    free(buf);
}

/*
 * Discover all plugins in the plugin directory.
 */
static int discover_plugins(void)
{
    const char *dir = plugin_dir();
    DIR *d = opendir(dir);
    if (!d) return 0;  /* dir might not exist yet */

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL && g_nplugins < MAX_PLUGINS) {
        const char *name = ent->d_name;

        /* skip dotfiles and .json sidecars */
        if (name[0] == '.') continue;
        size_t nlen = strlen(name);
        if (nlen > 5 && strcmp(name + nlen - 5, ".json") == 0) continue;

        char full_path[MAX_PATH_LEN];
        snprintf(full_path, sizeof(full_path), "%s/%s", dir, name);

        /* must be a regular executable file */
        struct stat st;
        if (stat(full_path, &st) < 0) continue;
        if (!S_ISREG(st.st_mode)) continue;
        if (!(st.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH))) continue;

        Plugin *p = &g_plugins[g_nplugins];
        memset(p, 0, sizeof(*p));
        snprintf(p->name,      sizeof(p->name),      "%s", name);
        snprintf(p->path,      sizeof(p->path),      "%s", full_path);
        snprintf(p->meta_path, sizeof(p->meta_path), "%s/%s.json", dir, name);
        load_meta(p);
        g_nplugins++;
    }
    closedir(d);
    return g_nplugins;
}

static Plugin *find_plugin(const char *name)
{
    for (int i = 0; i < g_nplugins; i++)
        if (strcmp(g_plugins[i].name, name) == 0)
            return &g_plugins[i];
    return NULL;
}

/* ---- sub-commands -------------------------------------------------------- */

static void cmd_list(void)
{
    discover_plugins();

    if (g_nplugins == 0) {
        printf("No plugins found in %s\n\n"
               "To install a plugin, copy an executable to that directory\n"
               "or use: claw-plugin install <path>\n", plugin_dir());
        return;
    }

    printf("%-20s  %-10s  %-8s  %s\n",
           "NAME", "VERSION", "SKILL", "DESCRIPTION");
    printf("%-20s  %-10s  %-8s  %s\n",
           "----", "-------", "-----", "-----------");
    for (int i = 0; i < g_nplugins; i++) {
        Plugin *p = &g_plugins[i];
        printf("%-20s  %-10s  %-8s  %s\n",
               p->name,
               p->version[0]     ? p->version     : "—",
               p->is_skill       ? "yes"           : "no",
               p->description[0] ? p->description  : "(no description)");
    }
    printf("\n%d plugin(s) in %s\n", g_nplugins, plugin_dir());
}

static void cmd_info(const char *name)
{
    discover_plugins();
    Plugin *p = find_plugin(name);
    if (!p) {
        fprintf(stderr, "claw-plugin: plugin '%s' not found in %s\n",
                name, plugin_dir());
        exit(1);
    }

    printf("Name:        %s\n", p->name);
    printf("Path:        %s\n", p->path);
    if (p->has_meta) {
        if (p->version[0])     printf("Version:     %s\n", p->version);
        if (p->author[0])      printf("Author:      %s\n", p->author);
        if (p->description[0]) printf("Description: %s\n", p->description);
        printf("Skill mode:  %s\n", p->is_skill ? "yes (reads JSON from stdin)" : "no");
        printf("Metadata:    %s\n", p->meta_path);
    } else {
        printf("(no metadata sidecar found at %s)\n", p->meta_path);
    }
}

static void cmd_run(const char *name, char *const args[], int nargs)
{
    discover_plugins();
    Plugin *p = find_plugin(name);
    if (!p) {
        fprintf(stderr, "claw-plugin: plugin '%s' not found in %s\n",
                name, plugin_dir());
        exit(1);
    }

    /* Build argv: [plugin_path, args..., NULL] */
    char **exec_argv = malloc(sizeof(char *) * (size_t)(nargs + 2));
    if (!exec_argv) {
        fprintf(stderr, "claw-plugin: out of memory\n");
        exit(1);
    }
    exec_argv[0] = p->path;
    for (int i = 0; i < nargs; i++) exec_argv[i + 1] = args[i];
    exec_argv[nargs + 1] = NULL;

    /* Export CLAW_PLUGIN_NAME so the plugin knows its own name */
    setenv("CLAW_PLUGIN_NAME", p->name, 1);

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        free(exec_argv);
        exit(1);
    }
    if (pid == 0) {
        execv(p->path, exec_argv);
        fprintf(stderr, "claw-plugin: exec(%s): %s\n", p->path, strerror(errno));
        _exit(127);
    }
    free(exec_argv);
    int status = 0;
    waitpid(pid, &status, 0);
    exit(WIFEXITED(status) ? WEXITSTATUS(status) : 1);
}

static void cmd_install(const char *src_path)
{
    ensure_plugin_dir();

    /* Extract basename */
    const char *name = strrchr(src_path, '/');
    name = name ? name + 1 : src_path;

    /* Strip .json if accidentally given */
    char clean_name[MAX_NAME_LEN];
    snprintf(clean_name, sizeof(clean_name), "%s", name);
    size_t nl = strlen(clean_name);
    if (nl > 5 && strcmp(clean_name + nl - 5, ".json") == 0)
        clean_name[nl - 5] = '\0';

    char dst[MAX_PATH_LEN];
    snprintf(dst, sizeof(dst), "%s/%s", plugin_dir(), clean_name);

    /* Copy file */
    int src_fd = open(src_path, O_RDONLY);
    if (src_fd < 0) {
        fprintf(stderr, "claw-plugin: cannot open %s: %s\n",
                src_path, strerror(errno));
        exit(1);
    }
    int dst_fd = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0755);
    if (dst_fd < 0) {
        fprintf(stderr, "claw-plugin: cannot create %s: %s\n",
                dst, strerror(errno));
        close(src_fd);
        exit(1);
    }

    char buf[65536];
    ssize_t nr;
    while ((nr = read(src_fd, buf, sizeof(buf))) > 0) {
        ssize_t nw = write(dst_fd, buf, (size_t)nr);
        if (nw != nr) {
            fprintf(stderr, "claw-plugin: write error: %s\n", strerror(errno));
            close(src_fd); close(dst_fd); exit(1);
        }
    }
    close(src_fd);
    close(dst_fd);

    /* Also install .json sidecar if present alongside source */
    char src_meta[MAX_PATH_LEN], dst_meta[MAX_PATH_LEN];
    snprintf(src_meta, sizeof(src_meta), "%s.json", src_path);
    snprintf(dst_meta, sizeof(dst_meta), "%s.json", dst);
    src_fd = open(src_meta, O_RDONLY);
    if (src_fd >= 0) {
        dst_fd = open(dst_meta, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (dst_fd >= 0) {
            while ((nr = read(src_fd, buf, sizeof(buf))) > 0) {
                ssize_t nw = write(dst_fd, buf, (size_t)nr);
                (void)nw;
            }
            close(dst_fd);
        }
        close(src_fd);
    }

    printf("Plugin '%s' installed to %s\n", clean_name, dst);
}

static void cmd_remove(const char *name)
{
    char path[MAX_PATH_LEN], meta[MAX_PATH_LEN];
    snprintf(path, sizeof(path), "%s/%s",      plugin_dir(), name);
    snprintf(meta, sizeof(meta), "%s/%s.json", plugin_dir(), name);

    int removed = 0;
    if (unlink(path) == 0) removed = 1;
    unlink(meta);  /* ignore error if no sidecar */

    if (!removed) {
        fprintf(stderr, "claw-plugin: plugin '%s' not found in %s\n",
                name, plugin_dir());
        exit(1);
    }
    printf("Plugin '%s' removed.\n", name);
}

/* ---- usage --------------------------------------------------------------- */

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s <command> [args]\n"
        "\n"
        "Commands:\n"
        "  list                — list all installed plugins\n"
        "  info <name>         — show plugin metadata\n"
        "  run  <name> [args…] — execute a plugin\n"
        "  install <path>      — install a plugin from a file path\n"
        "  remove  <name>      — remove an installed plugin\n"
        "\n"
        "Environment:\n"
        "  CLAW_PLUGIN_DIR     Plugin directory (default: %s)\n",
        prog, DEFAULT_PLUGIN_DIR);
}

/* ---- main ---------------------------------------------------------------- */

int main(int argc, char *argv[])
{
    if (argc < 2) { usage(argv[0]); return 1; }

    const char *cmd = argv[1];

    if (strcmp(cmd, "list") == 0) {
        cmd_list();
    } else if (strcmp(cmd, "info") == 0) {
        if (argc < 3) { fprintf(stderr, "claw-plugin info: missing plugin name\n"); return 1; }
        cmd_info(argv[2]);
    } else if (strcmp(cmd, "run") == 0) {
        if (argc < 3) { fprintf(stderr, "claw-plugin run: missing plugin name\n"); return 1; }
        cmd_run(argv[2], argv + 3, argc - 3);
    } else if (strcmp(cmd, "install") == 0) {
        if (argc < 3) { fprintf(stderr, "claw-plugin install: missing path\n"); return 1; }
        cmd_install(argv[2]);
    } else if (strcmp(cmd, "remove") == 0) {
        if (argc < 3) { fprintf(stderr, "claw-plugin remove: missing plugin name\n"); return 1; }
        cmd_remove(argv[2]);
    } else if (strcmp(cmd, "-h") == 0 || strcmp(cmd, "--help") == 0) {
        usage(argv[0]); return 0;
    } else {
        fprintf(stderr, "claw-plugin: unknown command '%s'\n", cmd);
        usage(argv[0]);
        return 1;
    }

    return 0;
}
