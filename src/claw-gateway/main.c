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
 *   POST /api/message    — submit a user message; agent processes and replies
 *   POST /api/event      — post a channel event (from claw-channel)
 *   GET  /api/health     — liveness probe
 *   GET  /api/status     — current gateway status JSON
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
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "../common/claw_json.h"

/* ---- tunables ------------------------------------------------------------ */
#define DEFAULT_PORT        18789
#define DEFAULT_BACKLOG        16
#define MAX_REQUEST_BYTES   65536
#define MAX_RESPONSE_BYTES  65536
#define AGENT_FIFO_IN   "/var/lib/claw/gateway.in"    /* gateway → agent   */
#define AGENT_FIFO_OUT  "/var/lib/claw/gateway.out"   /* agent  → gateway  */
#define FIFO_TIMEOUT_S       30

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
        write(fd, hdr, (size_t)hlen);
    }
    write(fd, body, strlen(body));
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
    write(in_fd, json_msg, msglen);
    write(in_fd, "\n", 1);
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

    /* Forward to agent */
    char *reply = dispatch_to_agent(body);
    if (!reply) {
        emit_json(client_fd, 500, "{\"ok\":false,\"error\":\"Internal error dispatching message\"}");
        return;
    }

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

    emit_json(client_fd, 200, reply);
    free(reply);
}

static void handle_not_found(int client_fd)
{
    emit_json(client_fd, 404,
        "{\"ok\":false,\"error\":\"Not found\","
        "\"endpoints\":[\"/api/health\",\"/api/status\",\"/api/message\",\"/api/event\"]}");
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
    ensure_fifo(AGENT_FIFO_IN);
    ensure_fifo(AGENT_FIFO_OUT);

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
        "  POST /api/message   — send a message to the agent\n"
        "  POST /api/event     — send a channel event\n"
        "  GET  /api/health    — liveness probe\n"
        "  GET  /api/status    — gateway status\n",
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
    fprintf(stdout, "claw-gateway: shutting down.\n");
    return 0;
}
