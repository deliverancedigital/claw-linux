/*
 * claw-pair — Device pairing for claw-linux.
 *
 * Implements a simple challenge-response pairing protocol that lets remote
 * devices securely register with the local claw-linux agent.  A paired device
 * receives a token it can use to authenticate subsequent API requests.
 *
 * Corresponds to: openclaw/openclaw src/pairing/ (device pairing)
 *
 * Pairing flow
 * ------------
 *   1. Server side (this machine): claw-pair advertise
 *        — generates a short pairing code (6 alphanumeric chars)
 *        — prints a QR-like ASCII art or the raw code
 *        — listens on PAIR_PORT for an incoming JSON connection
 *        — verifies the code echoed by the client
 *        — issues a random 256-bit hex token and writes it to the paired
 *          device registry (/var/lib/claw/paired.json)
 *        — responds to client with the token
 *
 *   2. Client side (remote device): claw-pair connect <host> <code>
 *        — connects to the server
 *        — sends the pairing code
 *        — receives the token
 *        — prints the token for the user to save
 *
 *   3. Management:  claw-pair list   — list all paired devices
 *                   claw-pair revoke <name|token> — remove a paired device
 *                   claw-pair status              — show pairing server status
 *
 * Registry format (/var/lib/claw/paired.json)
 * -------------------------------------------
 *   {
 *     "devices": [
 *       {"name":"phone","token":"abc…","paired_at":1700000000,"address":"1.2.3.4"}
 *     ]
 *   }
 *
 * Configuration (environment variables)
 * --------------------------------------
 *   CLAW_PAIR_PORT   Pairing listen port (default: 18791)
 *   CLAW_PAIR_BIND   Bind address (default: 0.0.0.0)
 *
 * Build:
 *   cc -O2 -Wall -Wextra -o claw-pair main.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "../common/claw_json.h"

/* ---- tunables ------------------------------------------------------------ */
#define DEFAULT_PORT       18791
#define DEFAULT_BIND       "0.0.0.0"
#define CODE_LEN           6
#define TOKEN_LEN          64    /* 32 bytes = 64 hex chars */
#define PAIR_TIMEOUT_S    120    /* code expires after 2 minutes */
#define REGISTRY_FILE    "/var/lib/claw/paired.json"
#define MAX_REGISTRY_BYTES (64 * 1024)
#define MAX_REQUEST_BYTES   4096
#define MAX_NAME_BYTES        64

/* ---- random helpers ------------------------------------------------------ */

static void random_bytes(unsigned char *buf, size_t n)
{
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        size_t got = 0;
        while (got < n) {
            ssize_t r = read(fd, buf + got, n - got);
            if (r > 0) got += (size_t)r;
        }
        close(fd);
    } else {
        /* Fallback (weaker) */
        srand((unsigned)time(NULL) ^ (unsigned)getpid());
        for (size_t i = 0; i < n; i++) buf[i] = (unsigned char)(rand() & 0xFF);
    }
}

static void gen_code(char *out)
{
    static const char ALPHA[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
    unsigned char raw[CODE_LEN];
    random_bytes(raw, sizeof(raw));
    for (int i = 0; i < CODE_LEN; i++)
        out[i] = ALPHA[raw[i] % (sizeof(ALPHA) - 1)];
    out[CODE_LEN] = '\0';
}

static void gen_token(char *out)
{
    unsigned char raw[TOKEN_LEN / 2];
    random_bytes(raw, sizeof(raw));
    for (size_t i = 0; i < sizeof(raw); i++)
        snprintf(out + i * 2, 3, "%02x", raw[i]);
    out[TOKEN_LEN] = '\0';
}

/* ---- registry ------------------------------------------------------------ */

static void ensure_dir(const char *path)
{
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') { *p = '\0'; mkdir(tmp, 0755); *p = '/'; }
    }
}

static char *read_registry(void)
{
    FILE *fp = fopen(REGISTRY_FILE, "r");
    if (!fp) return strdup("{\"devices\":[]}");
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp); rewind(fp);
    if (sz <= 0 || sz > MAX_REGISTRY_BYTES) { fclose(fp); return strdup("{\"devices\":[]}"); }
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(fp); return NULL; }
    size_t nr = fread(buf, 1, (size_t)sz, fp);
    buf[nr] = '\0';
    fclose(fp);
    return buf;
}

static int write_registry(const char *content)
{
    ensure_dir(REGISTRY_FILE);
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s.tmp", REGISTRY_FILE);
    FILE *fp = fopen(tmp, "w");
    if (!fp) return -1;
    fputs(content, fp); fputc('\n', fp);
    fclose(fp);
    return rename(tmp, REGISTRY_FILE);
}

/*
 * Append a device entry to the registry.
 */
static int registry_add(const char *name, const char *token,
                        const char *address, long paired_at)
{
    char *reg = read_registry();
    if (!reg) return -1;

    /* Find the closing bracket of the devices array */
    char *arr_end = strrchr(reg, ']');
    if (!arr_end) { free(reg); return -1; }

    /* Check if array has existing entries */
    char *arr_start = strstr(reg, "[");
    int has_entries = 0;
    if (arr_start) {
        const char *p = arr_start + 1;
        while (*p == ' ' || *p == '\n' || *p == '\t') p++;
        has_entries = (*p != ']');
    }

    char entry[1024];
    char esc_name[MAX_NAME_BYTES * 2]   = {0};
    char esc_tok[TOKEN_LEN + 4]         = {0};
    char esc_addr[64]                   = {0};
    json_escape(name,    esc_name, sizeof(esc_name));
    json_escape(token,   esc_tok,  sizeof(esc_tok));
    json_escape(address, esc_addr, sizeof(esc_addr));

    snprintf(entry, sizeof(entry),
        "%s{\"name\":\"%s\",\"token\":\"%s\",\"address\":\"%s\",\"paired_at\":%ld}",
        has_entries ? "," : "", esc_name, esc_tok, esc_addr, paired_at);

    size_t prefix  = (size_t)(arr_end - reg);
    size_t elen    = strlen(entry);
    size_t tail    = strlen(arr_end);
    char  *newreg  = malloc(prefix + elen + tail + 2);
    if (!newreg) { free(reg); return -1; }
    memcpy(newreg, reg, prefix);
    memcpy(newreg + prefix, entry, elen);
    memcpy(newreg + prefix + elen, arr_end, tail);
    newreg[prefix + elen + tail] = '\0';
    free(reg);

    int rc = write_registry(newreg);
    free(newreg);
    return rc;
}

/* ---- advertise (server side) --------------------------------------------- */

static void handle_pair_request(int cfd, const char *code, const char *peer_ip)
{
    char req[MAX_REQUEST_BYTES] = {0};
    ssize_t nr = read(cfd, req, sizeof(req) - 1);
    if (nr <= 0) { close(cfd); return; }
    req[nr] = '\0';

    /* Skip HTTP headers if present, find JSON body */
    const char *body = strstr(req, "\r\n\r\n");
    if (body) body += 4; else body = req;

    char recv_code[16]        = {0};
    char device_name[MAX_NAME_BYTES] = {0};

    json_get_string(body, "code", recv_code, sizeof(recv_code));
    json_get_string(body, "name", device_name, sizeof(device_name));

    if (!device_name[0]) snprintf(device_name, sizeof(device_name), "device-%s", peer_ip);

    const char *resp_body;
    char token[TOKEN_LEN + 1] = {0};

    if (strcmp(recv_code, code) != 0) {
        resp_body = "{\"ok\":false,\"error\":\"Invalid pairing code\"}";
    } else {
        gen_token(token);
        registry_add(device_name, token, peer_ip, (long)time(NULL));
        static char ok_resp[TOKEN_LEN + 128];
        snprintf(ok_resp, sizeof(ok_resp),
            "{\"ok\":true,\"token\":\"%s\",\"name\":\"%s\"}", token, device_name);
        resp_body = ok_resp;
    }

    char http_resp[2048];
    int hlen = snprintf(http_resp, sizeof(http_resp),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n"
        "%s",
        strlen(resp_body), resp_body);
    if (hlen > 0) {
        ssize_t nw = write(cfd, http_resp, (size_t)hlen);
        (void)nw;
    }
    close(cfd);

    if (token[0])
        printf("\n✓ Device '%s' paired successfully.\n  Token: %s\n", device_name, token);
    else
        printf("\n✗ Pairing failed — wrong code from %s\n", peer_ip);
    fflush(stdout);
}

static void cmd_advertise(void)
{
    const char *bind_addr = getenv("CLAW_PAIR_BIND") ? getenv("CLAW_PAIR_BIND") : DEFAULT_BIND;
    int port = DEFAULT_PORT;
    const char *env_port = getenv("CLAW_PAIR_PORT");
    if (env_port) port = atoi(env_port);

    char code[CODE_LEN + 1];
    gen_code(code);

    printf("═══════════════════════════════════════\n");
    printf("  claw-pair pairing code: %s\n", code);
    printf("  Port: %d   Expires in: %ds\n", port, PAIR_TIMEOUT_S);
    printf("═══════════════════════════════════════\n");
    printf("On the remote device, run:\n");
    printf("  claw-pair connect <this-host> %s\n\n", code);
    fflush(stdout);

    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) { perror("socket"); exit(1); }
    int optval = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((uint16_t)port);
    inet_pton(AF_INET, bind_addr, &addr.sin_addr);

    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) < 0) { perror("bind"); exit(1); }
    if (listen(srv, 4) < 0) { perror("listen"); exit(1); }

    /* Set accept timeout */
    struct timeval tv = { PAIR_TIMEOUT_S, 0 };
    setsockopt(srv, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);
    printf("Waiting for pairing connection on port %d…\n", port);
    fflush(stdout);

    int cfd = accept(srv, (struct sockaddr *)&client_addr, &client_len);
    close(srv);

    if (cfd < 0) {
        fprintf(stderr, "claw-pair: timeout or error waiting for connection: %s\n",
                strerror(errno));
        exit(1);
    }

    char peer_ip[INET_ADDRSTRLEN] = {0};
    inet_ntop(AF_INET, &client_addr.sin_addr, peer_ip, sizeof(peer_ip));
    printf("Connection from %s\n", peer_ip);
    fflush(stdout);

    handle_pair_request(cfd, code, peer_ip);
}

/* ---- connect (client side) ----------------------------------------------- */

static void cmd_connect(const char *host, const char *code)
{
    int port = DEFAULT_PORT;
    const char *env_port = getenv("CLAW_PAIR_PORT");
    if (env_port) port = atoi(env_port);

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); exit(1); }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((uint16_t)port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) <= 0) {
        fprintf(stderr, "claw-pair: invalid host address '%s'\n", host);
        exit(1);
    }

    printf("Connecting to %s:%d…\n", host, port);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "claw-pair: connect: %s\n", strerror(errno));
        exit(1);
    }

    char body[256];
    snprintf(body, sizeof(body), "{\"code\":\"%s\",\"name\":\"claw-device\"}", code);

    char req[512];
    int rlen = snprintf(req, sizeof(req),
        "POST /pair HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n"
        "%s",
        host, port, strlen(body), body);

    ssize_t nw = write(fd, req, (size_t)rlen);
    (void)nw;

    char resp[4096] = {0};
    ssize_t nr = read(fd, resp, sizeof(resp) - 1);
    close(fd);

    if (nr <= 0) { fprintf(stderr, "claw-pair: no response from server\n"); exit(1); }
    resp[nr] = '\0';

    const char *json = strstr(resp, "\r\n\r\n");
    if (json) json += 4; else json = resp;

    char token[TOKEN_LEN + 1] = {0};
    char err[256] = {0};
    long ok = json_get_long(json, "ok", 0);
    json_get_string(json, "token", token, sizeof(token));
    json_get_string(json, "error", err,   sizeof(err));

    if (!ok || !token[0]) {
        fprintf(stderr, "claw-pair: pairing failed: %s\n",
                err[0] ? err : "unknown error");
        exit(1);
    }

    printf("\n✓ Paired successfully!\n");
    printf("  Token: %s\n", token);
    printf("\nSave this token — use it in CLAW_PAIR_TOKEN to authenticate.\n");
}

/* ---- list ---------------------------------------------------------------- */

static void cmd_list(void)
{
    char *reg = read_registry();
    if (!reg) { fprintf(stderr, "claw-pair: cannot read registry\n"); return; }

    /* Count and print devices */
    int count = 0;
    const char *p = reg;
    printf("%-20s  %-20s  %-16s  %s\n", "NAME", "TOKEN (first 16…)", "ADDRESS", "PAIRED AT");
    printf("%-20s  %-20s  %-16s  %s\n", "----", "----------------", "-------", "---------");

    while ((p = strstr(p, "\"name\":")) != NULL) {
        char name[MAX_NAME_BYTES] = {0};
        char token[TOKEN_LEN + 1] = {0};
        char address[64]          = {0};

        /* Find the enclosing object start */
        const char *obj = p;
        while (obj > reg && *obj != '{') obj--;

        json_get_string(obj, "name",    name,    sizeof(name));
        json_get_string(obj, "token",   token,   sizeof(token));
        json_get_string(obj, "address", address, sizeof(address));
        long paired_at = json_get_long(obj, "paired_at", 0);

        /* Format paired_at as a date string */
        char date[32] = {0};
        if (paired_at > 0) {
            time_t t = (time_t)paired_at;
            struct tm *tm = gmtime(&t);
            strftime(date, sizeof(date), "%Y-%m-%d %H:%M", tm);
        }

        /* Truncate token for display */
        char tok_short[20] = {0};
        if (strlen(token) > 16)
            snprintf(tok_short, sizeof(tok_short), "%-.16s…", token);
        else
            snprintf(tok_short, sizeof(tok_short), "%s", token);

        printf("%-20s  %-20s  %-16s  %s\n", name, tok_short, address, date);
        count++;
        p++;
    }
    free(reg);
    printf("\n%d paired device(s). Registry: %s\n", count, REGISTRY_FILE);
}

/* ---- revoke -------------------------------------------------------------- */

static void cmd_revoke(const char *name_or_token)
{
    char *reg = read_registry();
    if (!reg) { fprintf(stderr, "claw-pair: cannot read registry\n"); exit(1); }

    /* Find the device with matching name or token and remove the {} block */
    const char *p = reg;
    int removed = 0;

    while ((p = strstr(p, "\"name\":")) != NULL) {
        const char *obj_start = p;
        while (obj_start > reg && *obj_start != '{') obj_start--;

        char name[MAX_NAME_BYTES]  = {0};
        char token[TOKEN_LEN + 1]  = {0};
        json_get_string(obj_start, "name",  name,  sizeof(name));
        json_get_string(obj_start, "token", token, sizeof(token));

        if (strcmp(name, name_or_token) == 0 || strcmp(token, name_or_token) == 0) {
            /* Find the matching closing brace */
            const char *obj_end = obj_start + 1;
            int depth = 1;
            while (*obj_end && depth > 0) {
                if (*obj_end == '{') depth++;
                if (*obj_end == '}') depth--;
                obj_end++;
            }

            /* Remove from reg: trim surrounding comma */
            size_t pre_len  = (size_t)(obj_start - reg);
            size_t post_off = (size_t)(obj_end - reg);

            /* Strip trailing comma or leading comma */
            size_t trim_pre = pre_len;
            while (trim_pre > 0 &&
                   (reg[trim_pre-1] == ',' || reg[trim_pre-1] == '\n' || reg[trim_pre-1] == ' '))
                trim_pre--;

            size_t tail = strlen(reg + post_off);
            char *newreg = malloc(trim_pre + tail + 1);
            if (!newreg) { free(reg); exit(1); }
            memcpy(newreg, reg, trim_pre);
            memcpy(newreg + trim_pre, reg + post_off, tail);
            newreg[trim_pre + tail] = '\0';
            free(reg);
            reg = newreg;
            removed = 1;
            break;
        }
        p++;
    }

    if (!removed) {
        fprintf(stderr, "claw-pair: device '%s' not found\n", name_or_token);
        free(reg);
        exit(1);
    }

    write_registry(reg);
    free(reg);
    printf("Device '%s' revoked.\n", name_or_token);
}

/* ---- usage --------------------------------------------------------------- */

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s <command> [args]\n"
        "\n"
        "Commands:\n"
        "  advertise               — generate pairing code and wait for a device\n"
        "  connect <host> <code>   — connect to a pairing server with a code\n"
        "  list                    — list all paired devices\n"
        "  revoke <name|token>     — remove a paired device\n"
        "\n"
        "Environment:\n"
        "  CLAW_PAIR_PORT          Pairing port (default: %d)\n"
        "  CLAW_PAIR_BIND          Bind address (default: %s)\n",
        prog, DEFAULT_PORT, DEFAULT_BIND);
}

/* ---- main ---------------------------------------------------------------- */

int main(int argc, char *argv[])
{
    signal(SIGPIPE, SIG_IGN);

    if (argc < 2) { usage(argv[0]); return 1; }

    const char *cmd = argv[1];

    if (strcmp(cmd, "advertise") == 0) {
        cmd_advertise();
    } else if (strcmp(cmd, "connect") == 0) {
        if (argc < 4) {
            fprintf(stderr, "claw-pair connect: usage: claw-pair connect <host> <code>\n");
            return 1;
        }
        cmd_connect(argv[2], argv[3]);
    } else if (strcmp(cmd, "list") == 0) {
        cmd_list();
    } else if (strcmp(cmd, "revoke") == 0) {
        if (argc < 3) {
            fprintf(stderr, "claw-pair revoke: missing device name or token\n");
            return 1;
        }
        cmd_revoke(argv[2]);
    } else if (strcmp(cmd, "-h") == 0 || strcmp(cmd, "--help") == 0) {
        usage(argv[0]); return 0;
    } else {
        fprintf(stderr, "claw-pair: unknown command '%s'\n", cmd);
        usage(argv[0]); return 1;
    }
    return 0;
}
