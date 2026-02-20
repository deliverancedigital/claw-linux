/*
 * claw-gateway — Native HTTP control-plane gateway for claw-linux.
 *
 * Replaces the Node.js OpenClaw Gateway with a zero-dependency C daemon.
 * Listens on a TCP port (default 18789, matching the openclaw default) and
 * provides a minimal JSON-over-HTTP API that the agent and channel adapters
 * use to exchange messages without the Node.js runtime.
 *
 * Endpoints
 * ---------
 *   POST /api/message           — submit a user message; agent processes and replies
 *   POST /api/event             — post a channel event (from claw-channel)
 *   GET  /api/health            — liveness probe
 *   GET  /api/status            — current gateway status JSON
 *   POST /api/hook/register     — register an event hook (URL to POST to on events)
 *   GET  /api/hooks             — list registered hooks
 *   DELETE /api/hook/<id>       — remove a hook
 *   GET  /api/sessions          — list active sessions
 *   GET  /api/session/<id>      — get session details
 *
 * Protocol
 * --------
 *   All request and response bodies are JSON objects.  The gateway
 *   speaks HTTP/1.1 over plain TCP; TLS termination is handled upstream
 *   (nginx / Tailscale Serve / SSH tunnel).
 *
 * Architecture
 * ------------
 *   • accept loop runs in the main thread
 *   • each connection is handled by a freshly forked child process so that
 *     the gateway survives a misbehaving handler
 *   • message dispatch writes to a POSIX named pipe (FIFO) that the agent
 *     reads; the agent writes its response to a separate reply FIFO
 *
 * Build:
 *   cc -O2 -Wall -Wextra -o claw-gateway main.c
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
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <curl/curl.h>

#include "../common/claw_json.h"

/* ---- tunables ------------------------------------------------------------ */
#define DEFAULT_PORT        18789
#define DEFAULT_BACKLOG        16
#define MAX_REQUEST_BYTES   65536
#define MAX_RESPONSE_BYTES  65536
#define AGENT_FIFO_IN   "/var/lib/claw/gateway.in"    /* gateway → agent   */
#define AGENT_FIFO_OUT  "/var/lib/claw/gateway.out"   /* agent  → gateway  */
#define FIFO_TIMEOUT_S       30
#define HOOKS_FILE      "/var/lib/claw/hooks.json"
#define SESSIONS_DIR    "/var/lib/claw/sessions"
#define MAX_HOOK_ID_LEN   32
#define MAX_HOOK_URL_LEN 512
#define MAX_SESSION_ID_LEN 256

/* ---- globals ------------------------------------------------------------- */
static volatile int g_running   = 1;
static int          g_port      = DEFAULT_PORT;
static const char  *g_bind_addr = "0.0.0.0";

/* ---- signal handlers ----------------------------------------------------- */
static void on_sigterm(int sig) { (void)sig; g_running = 0; }

static void reap_children(int sig)
{
    (void)sig;
    while (waitpid(-1, NULL, WNOHANG) > 0)
        ;
}

/* ---- hooks: file-backed registry ----------------------------------------- */

/*
 * Read hooks file into a heap buffer.  Caller must free().
 * Returns "{\"hooks\":[]}" on missing file.
 */
static char *read_hooks(void)
{
    FILE *fp = fopen(HOOKS_FILE, "r");
    if (!fp) return strdup("{\"hooks\":[]}");
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp); rewind(fp);
    if (sz <= 0 || sz > 65536) { fclose(fp); return strdup("{\"hooks\":[]}"); }
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(fp); return NULL; }
    size_t nr = fread(buf, 1, (size_t)sz, fp);
    buf[nr] = '\0';
    fclose(fp);
    return buf;
}

static int write_hooks(const char *content)
{
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s.tmp", HOOKS_FILE);
    FILE *fp = fopen(tmp, "w");
    if (!fp) {
        mkdir("/var/lib/claw", 0750);
        fp = fopen(tmp, "w");
        if (!fp) return -1;
    }
    fputs(content, fp); fputc('\n', fp);
    fclose(fp);
    return rename(tmp, HOOKS_FILE);
}

/*
 * Fire all hooks whose event matches event_name.
 * POSTs payload to each hook URL using a forked child so the gateway
 * request handler is not blocked.
 */
static void fire_hooks(const char *event_name, const char *payload)
{
    char *hooks = read_hooks();
    if (!hooks) return;

    /* Walk hook objects: {"id":"…","event":"…","url":"…"} */
    const char *p = hooks;
    while ((p = strstr(p, "\"event\":")) != NULL) {
        /* Find enclosing object */
        const char *obj = p;
        while (obj > hooks && *obj != '{') obj--;

        char ev[64]                 = {0};
        char url[MAX_HOOK_URL_LEN]  = {0};
        json_get_string(obj, "event", ev,  sizeof(ev));
        json_get_string(obj, "url",   url, sizeof(url));

        if (url[0] && (strcmp(ev, "any") == 0 || strcmp(ev, event_name) == 0)) {
            /* Fork a delivery child */
            pid_t pid = fork();
            if (pid == 0) {
                /* Child: POST payload to url */
                CURL *curl = curl_easy_init();
                if (curl) {
                    struct curl_slist *hdrs = NULL;
                    hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
                    curl_easy_setopt(curl, CURLOPT_URL,           url);
                    curl_easy_setopt(curl, CURLOPT_POSTFIELDS,    payload);
                    curl_easy_setopt(curl, CURLOPT_HTTPHEADER,    hdrs);
                    curl_easy_setopt(curl, CURLOPT_TIMEOUT,       10L);
                    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
                    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
                    curl_easy_perform(curl);
                    curl_slist_free_all(hdrs);
                    curl_easy_cleanup(curl);
                }
                _exit(0);
            }
            /* Parent: don't wait — hooks are fire-and-forget */
        }
        p++;
    }
    free(hooks);
}

/* ---- sessions: per-session state on disk --------------------------------- */

static void ensure_sessions_dir(void)
{
    mkdir("/var/lib/claw", 0750);
    mkdir(SESSIONS_DIR, 0750);
}

/*
 * Write/update a session file when a message is processed.
 */
static void session_update(const char *session_id, const char *message,
                           const char *reply)
{
    if (!session_id || !session_id[0]) return;
    ensure_sessions_dir();

    char path[768];
    snprintf(path, sizeof(path), "%s/%s.json", SESSIONS_DIR, session_id);

    /* Read existing file or create empty */
    FILE *rfp = fopen(path, "r");
    long created_at = (long)time(NULL);
    if (rfp) {
        /* Try to get existing created_at */
        char rbuf[4096] = {0};
        size_t nr = fread(rbuf, 1, sizeof(rbuf) - 1, rfp);
        rbuf[nr] = '\0';
        fclose(rfp);
        long ca = json_get_long(rbuf, "created_at", 0);
        if (ca > 0) created_at = ca;
    }

    char esc_sid[MAX_SESSION_ID_LEN * 2]  = {0};
    char esc_msg[MAX_RESPONSE_BYTES / 4]  = {0};
    char esc_rep[MAX_RESPONSE_BYTES / 4]  = {0};

    json_escape(session_id, esc_sid, sizeof(esc_sid));
    if (message) json_escape(message, esc_msg, sizeof(esc_msg));
    if (reply)   json_escape(reply,   esc_rep, sizeof(esc_rep));

    char tmp_path[780];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);
    FILE *fp = fopen(tmp_path, "w");
    if (!fp) return;

    fprintf(fp,
        "{\"session_id\":\"%s\","
        "\"created_at\":%ld,"
        "\"updated_at\":%ld,"
        "\"last_message\":\"%s\","
        "\"last_reply\":\"%s\"}\n",
        esc_sid, created_at, (long)time(NULL), esc_msg, esc_rep);
    fclose(fp);
    rename(tmp_path, path);
}

/* ---- helpers ------------------------------------------------------------- */

static void emit_json(int fd, int status, const char *body)
{
    char hdr[512];
    int hlen = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n",
        status,
        status == 200 ? "OK" : status == 400 ? "Bad Request" : "Internal Server Error",
        strlen(body));
    if (hlen > 0) {
        ssize_t r = write(fd, hdr, (size_t)hlen);
        (void)r;
    }
    ssize_t r = write(fd, body, strlen(body));
    (void)r;
}

/*
 * Read a full HTTP request from fd into buf (up to buf_size-1 bytes).
 * Returns total bytes read, or -1 on error.
 */
static ssize_t read_request(int fd, char *buf, size_t buf_size)
{
    size_t total = 0;
    while (total < buf_size - 1) {
        ssize_t n = read(fd, buf + total, buf_size - 1 - total);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) break;
        total += (size_t)n;
        buf[total] = '\0';
        /* Stop once we have the full header + body */
        const char *hdr_end = strstr(buf, "\r\n\r\n");
        if (hdr_end) {
            /* Check Content-Length to see if body is complete */
            const char *cl = strstr(buf, "Content-Length:");
            if (!cl) cl = strstr(buf, "content-length:");
            if (cl) {
                long clen = strtol(cl + 15, NULL, 10);
                size_t body_start = (size_t)(hdr_end - buf) + 4;
                if (total >= body_start + (size_t)clen)
                    break;
            } else {
                break; /* no body */
            }
        }
    }
    buf[total] = '\0';
    return (ssize_t)total;
}

/*
 * Extract the JSON body from a raw HTTP request string.
 * Returns a pointer into req, or NULL if not found.
 */
static const char *extract_body(const char *req)
{
    const char *p = strstr(req, "\r\n\r\n");
    if (!p) return NULL;
    return p + 4;
}

/*
 * Extract the URL path from the first request line.
 * Writes into path (path_size bytes).  Returns 1 on success.
 */
static int extract_path(const char *req, char *path, size_t path_size)
{
    /* GET /api/health HTTP/1.1\r\n */
    const char *sp1 = strchr(req, ' ');
    if (!sp1) return 0;
    sp1++;
    const char *sp2 = strchr(sp1, ' ');
    if (!sp2) return 0;
    size_t len = (size_t)(sp2 - sp1);
    if (len >= path_size) len = path_size - 1;
    memcpy(path, sp1, len);
    path[len] = '\0';
    return 1;
}

/*
 * Extract the HTTP method from the first request line.
 */
static int extract_method(const char *req, char *method, size_t method_size)
{
    const char *sp = strchr(req, ' ');
    if (!sp) return 0;
    size_t len = (size_t)(sp - req);
    if (len >= method_size) len = method_size - 1;
    memcpy(method, req, len);
    method[len] = '\0';
    return 1;
}

/* ---- agent IPC via FIFOs ------------------------------------------------- */

/*
 * Send a message to the agent via the request FIFO and read the reply.
 * Returns a heap-allocated string with the agent's JSON reply, or NULL
 * on failure.  Caller must free().
 */
static char *dispatch_to_agent(const char *json_msg)
{
    /* Open the request FIFO for writing */
    int in_fd = open(AGENT_FIFO_IN, O_WRONLY | O_NONBLOCK);
    if (in_fd < 0) {
        return strdup("{\"ok\":false,\"error\":\"Agent FIFO not available — is the agent running?\"}");
    }

    /* Write the JSON message followed by a newline delimiter */
    size_t msglen = strlen(json_msg);
    ssize_t wr;
    wr = write(in_fd, json_msg, msglen); (void)wr;
    wr = write(in_fd, "\n", 1);         (void)wr;
    close(in_fd);

    /* Open the reply FIFO and wait for the agent's response */
    int out_fd = open(AGENT_FIFO_OUT, O_RDONLY | O_NONBLOCK);
    if (out_fd < 0) {
        return strdup("{\"ok\":false,\"error\":\"Agent reply FIFO not available\"}");
    }

    /* Poll with timeout */
    time_t deadline = time(NULL) + FIFO_TIMEOUT_S;
    char *buf = malloc(MAX_RESPONSE_BYTES);
    if (!buf) { close(out_fd); return NULL; }
    size_t total = 0;

    while (time(NULL) < deadline) {
        ssize_t n = read(out_fd, buf + total, MAX_RESPONSE_BYTES - 1 - total);
        if (n > 0) {
            total += (size_t)n;
            buf[total] = '\0';
            if (strchr(buf, '\n')) break; /* got full line */
        } else if (n == 0 || errno == EAGAIN) {
            struct timespec ts = { 0, 50000000L }; /* 50 ms */
            nanosleep(&ts, NULL);
        } else if (errno != EINTR) {
            break;
        }
    }
    close(out_fd);
    buf[total] = '\0';

    /* Strip trailing newline */
    if (total > 0 && buf[total - 1] == '\n')
        buf[--total] = '\0';

    return buf;
}

/* ---- request handlers ---------------------------------------------------- */

static void handle_health(int client_fd)
{
    emit_json(client_fd, 200, "{\"ok\":true,\"service\":\"claw-gateway\",\"status\":\"healthy\"}");
}

static void handle_status(int client_fd)
{
    char body[512];
    snprintf(body, sizeof(body),
        "{\"ok\":true,\"service\":\"claw-gateway\","
        "\"port\":%d,\"fifo_in\":\"%s\",\"fifo_out\":\"%s\"}",
        g_port, AGENT_FIFO_IN, AGENT_FIFO_OUT);
    emit_json(client_fd, 200, body);
}

static void handle_message(int client_fd, const char *body)
{
    if (!body || *body == '\0') {
        emit_json(client_fd, 400, "{\"ok\":false,\"error\":\"Empty request body\"}");
        return;
    }

    /* Validate minimal JSON: must have a "message" field */
    char msg[8192] = {0};
    if (!json_get_string(body, "message", msg, sizeof(msg))) {
        emit_json(client_fd, 400,
            "{\"ok\":false,\"error\":\"Missing 'message' field in JSON body\"}");
        return;
    }

    /* Extract session ID for persistence */
    char session_id[MAX_SESSION_ID_LEN] = {0};
    json_get_string(body, "session", session_id, sizeof(session_id));
    if (!session_id[0]) snprintf(session_id, sizeof(session_id), "default");

    /* Forward to agent */
    char *reply = dispatch_to_agent(body);
    if (!reply) {
        emit_json(client_fd, 500, "{\"ok\":false,\"error\":\"Internal error dispatching message\"}");
        return;
    }

    /* Persist session state */
    session_update(session_id, msg, reply);

    /* Fire hooks */
    fire_hooks("message", body);

    emit_json(client_fd, 200, reply);
    free(reply);
}

static void handle_event(int client_fd, const char *body)
{
    if (!body || *body == '\0') {
        emit_json(client_fd, 400, "{\"ok\":false,\"error\":\"Empty event body\"}");
        return;
    }

    /* Events are forwarded to the agent as-is */
    char *reply = dispatch_to_agent(body);
    if (!reply) {
        emit_json(client_fd, 500, "{\"ok\":false,\"error\":\"Internal error dispatching event\"}");
        return;
    }

    /* Fire hooks */
    fire_hooks("event", body);

    emit_json(client_fd, 200, reply);
    free(reply);
}

/* ---- hook endpoints ------------------------------------------------------ */

static void handle_hook_register(int client_fd, const char *body)
{
    if (!body || !*body) {
        emit_json(client_fd, 400, "{\"ok\":false,\"error\":\"Empty request body\"}");
        return;
    }

    char url[MAX_HOOK_URL_LEN] = {0};
    char event[64]             = "any";

    if (!json_get_string(body, "url", url, sizeof(url)) || !url[0]) {
        emit_json(client_fd, 400, "{\"ok\":false,\"error\":\"Missing 'url' field\"}");
        return;
    }
    json_get_string(body, "event", event, sizeof(event));

    /* Generate a unique hook ID from time + pid + random byte */
    unsigned char rnd[4] = {0};
    int ufd = open("/dev/urandom", O_RDONLY);
    if (ufd >= 0) { ssize_t nr = read(ufd, rnd, sizeof(rnd)); (void)nr; close(ufd); }
    char hook_id[MAX_HOOK_ID_LEN];
    snprintf(hook_id, sizeof(hook_id), "%lx%x%02x%02x%02x%02x",
             (unsigned long)time(NULL), (unsigned)getpid(),
             rnd[0], rnd[1], rnd[2], rnd[3]);

    char *hooks = read_hooks();
    if (!hooks) {
        emit_json(client_fd, 500, "{\"ok\":false,\"error\":\"Cannot read hooks registry\"}");
        return;
    }

    /* Find end of the hooks array */
    char *arr_end = strrchr(hooks, ']');
    if (!arr_end) { free(hooks); emit_json(client_fd, 500, "{\"ok\":false,\"error\":\"Corrupt hooks file\"}"); return; }

    /* Detect if array has entries already */
    char *arr_start = strchr(hooks, '[');
    int has_entries = 0;
    if (arr_start) {
        const char *p = arr_start + 1;
        while (*p == ' ' || *p == '\n') p++;
        has_entries = (*p != ']');
    }

    char esc_url[MAX_HOOK_URL_LEN * 2] = {0};
    char esc_ev[128] = {0};
    json_escape(url,   esc_url, sizeof(esc_url));
    json_escape(event, esc_ev,  sizeof(esc_ev));

    char entry[1024];
    snprintf(entry, sizeof(entry),
        "%s{\"id\":\"%s\",\"event\":\"%s\",\"url\":\"%s\",\"registered_at\":%ld}",
        has_entries ? "," : "", hook_id, esc_ev, esc_url, (long)time(NULL));

    size_t prefix = (size_t)(arr_end - hooks);
    size_t elen   = strlen(entry);
    size_t tail   = strlen(arr_end);
    char *newreg  = malloc(prefix + elen + tail + 2);
    if (!newreg) { free(hooks); emit_json(client_fd, 500, "{\"ok\":false,\"error\":\"Out of memory\"}"); return; }
    memcpy(newreg, hooks, prefix);
    memcpy(newreg + prefix, entry, elen);
    memcpy(newreg + prefix + elen, arr_end, tail);
    newreg[prefix + elen + tail] = '\0';
    free(hooks);

    if (write_hooks(newreg) < 0) {
        free(newreg);
        emit_json(client_fd, 500, "{\"ok\":false,\"error\":\"Failed to write hooks file\"}");
        return;
    }
    free(newreg);

    char resp[512];
    snprintf(resp, sizeof(resp),
        "{\"ok\":true,\"id\":\"%s\",\"event\":\"%s\",\"url\":\"%s\"}",
        hook_id, esc_ev, esc_url);
    emit_json(client_fd, 200, resp);
}

static void handle_hook_delete(int client_fd, const char *hook_id)
{
    char *hooks = read_hooks();
    if (!hooks) { emit_json(client_fd, 500, "{\"ok\":false,\"error\":\"Cannot read hooks\"}"); return; }

    /* Find and remove the hook with matching id */
    char needle[MAX_HOOK_ID_LEN + 16];
    snprintf(needle, sizeof(needle), "\"id\":\"%.*s\"", MAX_HOOK_ID_LEN, hook_id);

    const char *found = strstr(hooks, needle);
    if (!found) {
        free(hooks);
        emit_json(client_fd, 404, "{\"ok\":false,\"error\":\"Hook not found\"}");
        return;
    }

    /* Find enclosing { … } */
    const char *obj_start = found;
    while (obj_start > hooks && *obj_start != '{') obj_start--;
    const char *obj_end = obj_start + 1;
    int depth = 1;
    while (*obj_end && depth > 0) {
        if (*obj_end == '{') depth++;
        if (*obj_end == '}') depth--;
        obj_end++;
    }

    /* Remove and strip surrounding comma */
    size_t pre = (size_t)(obj_start - hooks);
    while (pre > 0 && (hooks[pre-1] == ',' || hooks[pre-1] == '\n' || hooks[pre-1] == ' ')) pre--;

    size_t tail_off = (size_t)(obj_end - hooks);
    size_t tail_len = strlen(hooks + tail_off);
    char *newreg = malloc(pre + tail_len + 1);
    if (!newreg) { free(hooks); emit_json(client_fd, 500, "{\"ok\":false,\"error\":\"Out of memory\"}"); return; }
    memcpy(newreg, hooks, pre);
    memcpy(newreg + pre, hooks + tail_off, tail_len);
    newreg[pre + tail_len] = '\0';
    free(hooks);

    write_hooks(newreg);
    free(newreg);
    emit_json(client_fd, 200, "{\"ok\":true}");
}

static void handle_hooks_list(int client_fd)
{
    char *hooks = read_hooks();
    if (!hooks) { emit_json(client_fd, 500, "{\"ok\":false,\"error\":\"Cannot read hooks\"}"); return; }
    emit_json(client_fd, 200, hooks);
    free(hooks);
}

/* ---- session endpoints --------------------------------------------------- */

static void handle_sessions_list(int client_fd)
{
    ensure_sessions_dir();

    DIR *d = opendir(SESSIONS_DIR);
    if (!d) {
        emit_json(client_fd, 200, "{\"ok\":true,\"sessions\":[]}");
        return;
    }

    char buf[MAX_RESPONSE_BYTES] = {0};
    size_t pos = 0;
    int count = 0;

    pos += (size_t)snprintf(buf + pos, sizeof(buf) - pos, "{\"ok\":true,\"sessions\":[");

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        size_t nlen = strlen(ent->d_name);
        if (nlen < 6 || strcmp(ent->d_name + nlen - 5, ".json") != 0) continue;

        char sid[MAX_SESSION_ID_LEN] = {0};
        size_t slen = nlen - 5;
        if (slen >= sizeof(sid)) slen = sizeof(sid) - 1;
        memcpy(sid, ent->d_name, slen);
        sid[slen] = '\0';

        char esc_sid[MAX_SESSION_ID_LEN * 2] = {0};
        json_escape(sid, esc_sid, sizeof(esc_sid));
        pos += (size_t)snprintf(buf + pos, sizeof(buf) - pos,
                                "%s\"%s\"", count > 0 ? "," : "", esc_sid);
        count++;
    }
    closedir(d);

    snprintf(buf + pos, sizeof(buf) - pos, "],\"count\":%d}", count);
    emit_json(client_fd, 200, buf);
}

static void handle_session_get(int client_fd, const char *session_id)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s.json", SESSIONS_DIR, session_id);

    FILE *fp = fopen(path, "r");
    if (!fp) {
        char esc[MAX_SESSION_ID_LEN * 2] = {0};
        json_escape(session_id, esc, sizeof(esc));
        char resp[512];
        snprintf(resp, sizeof(resp),
            "{\"ok\":false,\"error\":\"Session not found\",\"session_id\":\"%s\"}", esc);
        emit_json(client_fd, 404, resp);
        return;
    }
    char content[MAX_RESPONSE_BYTES] = {0};
    size_t nr = fread(content, 1, sizeof(content) - 1, fp);
    content[nr] = '\0';
    fclose(fp);
    emit_json(client_fd, 200, content);
}

static void handle_not_found(int client_fd)
{
    emit_json(client_fd, 404,
        "{\"ok\":false,\"error\":\"Not found\","
        "\"endpoints\":[\"/api/health\",\"/api/status\",\"/api/message\","
        "\"/api/event\",\"/api/hook/register\",\"/api/hooks\","
        "\"/api/sessions\",\"/api/session/<id>\"]}");
}

/* ---- connection handler (runs in child process) -------------------------- */

static void handle_connection(int client_fd)
{
    char req[MAX_REQUEST_BYTES];
    ssize_t n = read_request(client_fd, req, sizeof(req));
    if (n <= 0) { close(client_fd); return; }

    char method[16] = {0};
    char path[256]  = {0};
    extract_method(req, method, sizeof(method));
    extract_path(req, path, sizeof(path));

    /* Strip query string */
    char *q = strchr(path, '?');
    if (q) *q = '\0';

    const char *body = extract_body(req);

    if (strcmp(path, "/api/health") == 0) {
        handle_health(client_fd);
    } else if (strcmp(path, "/api/status") == 0) {
        handle_status(client_fd);
    } else if (strcmp(path, "/api/message") == 0 && strcmp(method, "POST") == 0) {
        handle_message(client_fd, body);
    } else if (strcmp(path, "/api/event") == 0 && strcmp(method, "POST") == 0) {
        handle_event(client_fd, body);
    } else if (strcmp(path, "/api/hook/register") == 0 && strcmp(method, "POST") == 0) {
        handle_hook_register(client_fd, body);
    } else if (strncmp(path, "/api/hook/", 10) == 0 && strcmp(method, "DELETE") == 0) {
        handle_hook_delete(client_fd, path + 10);
    } else if (strcmp(path, "/api/hooks") == 0 && strcmp(method, "GET") == 0) {
        handle_hooks_list(client_fd);
    } else if (strcmp(path, "/api/sessions") == 0 && strcmp(method, "GET") == 0) {
        handle_sessions_list(client_fd);
    } else if (strncmp(path, "/api/session/", 13) == 0 && strcmp(method, "GET") == 0) {
        handle_session_get(client_fd, path + 13);
    } else {
        handle_not_found(client_fd);
    }

    close(client_fd);
}

/* ---- FIFO setup ---------------------------------------------------------- */

static int ensure_fifo(const char *path)
{
    if (access(path, F_OK) == 0) return 0; /* already exists */
    if (mkfifo(path, 0660) < 0 && errno != EEXIST) {
        fprintf(stderr, "claw-gateway: mkfifo(%s): %s\n", path, strerror(errno));
        return -1;
    }
    return 0;
}

/* ---- usage / main -------------------------------------------------------- */

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [OPTIONS]\n"
        "\n"
        "Options:\n"
        "  -p PORT    Listen port (default: %d)\n"
        "  -b ADDR    Bind address (default: 0.0.0.0)\n"
        "  -h         Show this help\n",
        prog, DEFAULT_PORT);
}

int main(int argc, char *argv[])
{
    int opt;
    while ((opt = getopt(argc, argv, "p:b:h")) != -1) {
        switch (opt) {
            case 'p': g_port = atoi(optarg); break;
            case 'b': g_bind_addr = optarg;  break;
            case 'h': usage(argv[0]); return 0;
            default:  usage(argv[0]); return 1;
        }
    }

    /* Set up signal handling */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_sigterm;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT,  &sa, NULL);

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = reap_children;
    sa.sa_flags   = SA_RESTART;
    sigaction(SIGCHLD, &sa, NULL);

    /* Ignore broken pipe — write errors handled via return codes */
    signal(SIGPIPE, SIG_IGN);

    /* Create agent communication FIFOs */
    /* Ensure parent directory exists */
    mkdir("/var/lib/claw", 0750);
    ensure_sessions_dir();
    ensure_fifo(AGENT_FIFO_IN);
    ensure_fifo(AGENT_FIFO_OUT);

    /* Init curl for hook delivery */
    curl_global_init(CURL_GLOBAL_DEFAULT);

    /* Create TCP listen socket */
    int srv_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (srv_fd < 0) {
        perror("socket");
        return 1;
    }

    int opt_val = 1;
    setsockopt(srv_fd, SOL_SOCKET, SO_REUSEADDR, &opt_val, sizeof(opt_val));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((uint16_t)g_port);
    if (inet_pton(AF_INET, g_bind_addr, &addr.sin_addr) <= 0) {
        fprintf(stderr, "claw-gateway: invalid bind address: %s\n", g_bind_addr);
        return 1;
    }

    if (bind(srv_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        return 1;
    }
    if (listen(srv_fd, DEFAULT_BACKLOG) < 0) {
        perror("listen");
        return 1;
    }

    fprintf(stdout,
        "claw-gateway listening on %s:%d\n"
        "  POST /api/message          — send a message to the agent\n"
        "  POST /api/event            — send a channel event\n"
        "  GET  /api/health           — liveness probe\n"
        "  GET  /api/status           — gateway status\n"
        "  POST /api/hook/register    — register an event hook\n"
        "  GET  /api/hooks            — list registered hooks\n"
        "  DELETE /api/hook/<id>      — remove a hook\n"
        "  GET  /api/sessions         — list active sessions\n"
        "  GET  /api/session/<id>     — get session details\n",
        g_bind_addr, g_port);
    fflush(stdout);

    /* Accept loop */
    while (g_running) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(srv_fd, (struct sockaddr *)&client_addr, &client_len);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            if (!g_running) break;
            perror("accept");
            continue;
        }

        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            close(client_fd);
            continue;
        }
        if (pid == 0) {
            /* Child: handle and exit */
            close(srv_fd);
            handle_connection(client_fd);
            _exit(0);
        }
        /* Parent: close client fd and loop */
        close(client_fd);
    }

    close(srv_fd);
    curl_global_cleanup();
    fprintf(stdout, "claw-gateway: shutting down.\n");
    return 0;
}
