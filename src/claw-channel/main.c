/*
 * claw-channel — Webhook channel adapter for claw-linux.
 *
 * Receives inbound webhook payloads from messaging platforms (Telegram,
 * Discord, Slack, generic HTTP) and normalises them into a common JSON
 * envelope before forwarding them to the claw-gateway for agent processing.
 *
 * Protocol
 * --------
 *   Listens on a configurable TCP port (default 18790) for HTTP POST
 *   requests.  Each request body is parsed to extract the message text,
 *   sender, and channel type.  The normalised envelope is then POSTed to
 *   the gateway's /api/event endpoint.
 *
 * Supported channel formats
 * -------------------------
 *   telegram  — Telegram Bot API update object
 *   discord   — Discord Interactions / gateway webhook payload
 *   slack     — Slack Events API payload
 *   line      — LINE Messaging API webhook
 *   whatsapp  — WhatsApp Business API webhook
 *   webhook   — Generic flat JSON: { "message": "...", "sender": "..." }
 *
 * Normalised envelope (sent to gateway)
 * --------------------------------------
 *   {
 *     "channel": "<telegram|discord|slack|line|whatsapp|webhook>",
 *     "sender":  "<username or ID>",
 *     "message": "<plain text of the message>",
 *     "raw":     "<original payload, JSON-escaped>"
 *   }
 *
 * Configuration (environment variables)
 * --------------------------------------
 *   CLAW_CHANNEL_PORT          Listen port (default 18790)
 *   CLAW_CHANNEL_BIND          Bind address (default 127.0.0.1)
 *   CLAW_GATEWAY_URL           Gateway event endpoint
 *                              (default http://127.0.0.1:18789/api/event)
 *   CLAW_CHANNEL_SECRET        Optional shared secret for webhook validation
 *                              (compared against X-Claw-Secret header)
 *   CLAW_TELEGRAM_TOKEN        Telegram bot token (used to validate
 *                              X-Telegram-Bot-Api-Secret-Token header)
 *   CLAW_WHATSAPP_TOKEN        WhatsApp verify token (used during one-time
 *                              webhook setup GET challenge-response)
 *
 * Build (requires libcurl-dev):
 *   cc -O2 -Wall -Wextra -o claw-channel main.c -lcurl
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <curl/curl.h>

#include "../common/claw_json.h"

/* ---- tunables ------------------------------------------------------------ */
#define DEFAULT_PORT        18790
#define DEFAULT_BIND        "127.0.0.1"
#define DEFAULT_GATEWAY_URL "http://127.0.0.1:18789/api/event"
#define MAX_REQUEST_BYTES   131072   /* 128 KiB */
#define MAX_FIELD_BYTES      8192
#define MAX_URL_BYTES        2048
#define BACKLOG                16

/* ---- globals ------------------------------------------------------------- */
static volatile int g_running      = 1;
static int          g_port         = DEFAULT_PORT;
static const char  *g_bind_addr    = DEFAULT_BIND;
static char         g_gateway_url[MAX_URL_BYTES] = DEFAULT_GATEWAY_URL;
static char         g_secret[256]  = {0};
static char         g_tg_token[256] = {0};
static char         g_wa_token[256] = {0}; /* WhatsApp hub.verify_token */

/* ---- signal handlers ----------------------------------------------------- */
static void on_sigterm(int sig) { (void)sig; g_running = 0; }

static void reap_children(int sig)
{
    (void)sig;
    while (waitpid(-1, NULL, WNOHANG) > 0)
        ;
}

/* ---- HTTP helpers --------------------------------------------------------- */

static void http_respond(int fd, int status, const char *body)
{
    char hdr[512];
    int hlen = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n",
        status,
        status == 200 ? "OK" : status == 400 ? "Bad Request" :
        status == 401 ? "Unauthorized" : "Internal Server Error",
        strlen(body));
    if (hlen > 0) {
        ssize_t r = write(fd, hdr, (size_t)hlen);
        (void)r;
    }
    ssize_t r = write(fd, body, strlen(body));
    (void)r;
}

static ssize_t read_request(int fd, char *buf, size_t bufsz)
{
    size_t total = 0;
    while (total < bufsz - 1) {
        ssize_t n = read(fd, buf + total, bufsz - 1 - total);
        if (n < 0) { if (errno == EINTR) continue; return -1; }
        if (n == 0) break;
        total += (size_t)n;
        buf[total] = '\0';
        const char *hend = strstr(buf, "\r\n\r\n");
        if (hend) {
            const char *cl = strstr(buf, "Content-Length:");
            if (!cl) cl = strstr(buf, "content-length:");
            if (cl) {
                long clen = strtol(cl + 15, NULL, 10);
                size_t bstart = (size_t)(hend - buf) + 4;
                if (total >= bstart + (size_t)clen) break;
            } else {
                break;
            }
        }
    }
    buf[total] = '\0';
    return (ssize_t)total;
}

static const char *get_body(const char *req)
{
    const char *p = strstr(req, "\r\n\r\n");
    return p ? p + 4 : NULL;
}

static int get_method(const char *req, char *out, size_t sz)
{
    const char *sp = strchr(req, ' ');
    if (!sp) return 0;
    size_t len = (size_t)(sp - req);
    if (len >= sz) len = sz - 1;
    memcpy(out, req, len); out[len] = '\0';
    return 1;
}

static int get_path(const char *req, char *out, size_t sz)
{
    const char *sp1 = strchr(req, ' ');
    if (!sp1) return 0;
    sp1++;
    const char *sp2 = strchr(sp1, ' ');
    if (!sp2) return 0;
    size_t len = (size_t)(sp2 - sp1);
    if (len >= sz) len = sz - 1;
    memcpy(out, sp1, len); out[len] = '\0';
    char *q = strchr(out, '?'); if (q) *q = '\0';
    return 1;
}

/*
 * Get the value of an HTTP header from a raw request.
 * Returns 1 on success.
 */
static int get_header(const char *req, const char *hdr_name,
                      char *out, size_t out_sz)
{
    char needle[258]; /* name + ": " + NUL */
    int nlen = snprintf(needle, sizeof(needle), "%s:", hdr_name);
    if (nlen < 0) return 0;
    const char *p = strstr(req, needle);
    if (!p) {
        /* try lowercase */
        char lc[256];
        size_t i;
        for (i = 0; hdr_name[i] && i < sizeof(lc)-1; i++)
            lc[i] = (hdr_name[i] >= 'A' && hdr_name[i] <= 'Z')
                     ? (char)(hdr_name[i] + 32) : hdr_name[i];
        lc[i] = '\0';
        /* Rebuild needle from lc; lc is always shorter than needle */
        size_t lc_needle_len = strlen(lc) + 2;
        if (lc_needle_len < sizeof(needle)) {
            memcpy(needle, lc, strlen(lc));
            needle[strlen(lc)]   = ':';
            needle[strlen(lc)+1] = '\0';
        }
        p = strstr(req, needle);
    }
    if (!p) return 0;
    p += strlen(needle);
    while (*p == ' ' || *p == '\t') p++;
    size_t i = 0;
    while (*p && *p != '\r' && *p != '\n' && i < out_sz - 1)
        out[i++] = *p++;
    out[i] = '\0';
    return 1;
}

/*
 * Extract a URL query parameter value from the raw HTTP request line.
 * Returns 1 on success.
 */
static int get_query_param(const char *req, const char *param,
                           char *out, size_t out_sz)
{
    const char *sp1 = strchr(req, ' ');
    if (!sp1) return 0;
    sp1++;
    const char *sp2 = strchr(sp1, ' ');
    if (!sp2) return 0;

    const char *q = memchr(sp1, '?', (size_t)(sp2 - sp1));
    if (!q) return 0;
    q++;

    size_t plen = strlen(param);
    while (q < sp2) {
        if (strncmp(q, param, plen) == 0 && *(q + plen) == '=') {
            const char *v = q + plen + 1;
            size_t i = 0;
            while (v < sp2 && *v != '&' && *v != ' ' && i < out_sz - 1)
                out[i++] = *v++;
            out[i] = '\0';
            return 1;
        }
        /* Advance past this parameter */
        while (q < sp2 && *q != '&') q++;
        if (q < sp2) q++;
    }
    return 0;
}

/* ---- CURL forward to gateway --------------------------------------------- */

typedef struct { char *data; size_t used; size_t cap; } CurlBuf;

static size_t curl_write(void *ptr, size_t sz, size_t nm, void *ud)
{
    CurlBuf *b = (CurlBuf *)ud;
    size_t inc = sz * nm;
    if (b->used + inc + 1 > b->cap) {
        b->cap = b->cap * 2 + inc + 1;
        char *tmp = realloc(b->data, b->cap);
        if (!tmp) return 0;
        b->data = tmp;
    }
    memcpy(b->data + b->used, ptr, inc);
    b->used += inc;
    b->data[b->used] = '\0';
    return inc;
}

static int forward_to_gateway(const char *json_envelope)
{
    CURL *curl = curl_easy_init();
    if (!curl) return 0;

    CurlBuf rb = { malloc(1024), 0, 1024 };
    if (!rb.data) { curl_easy_cleanup(curl); return 0; }
    rb.data[0] = '\0';

    struct curl_slist *hdrs = NULL;
    hdrs = curl_slist_append(hdrs, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL,            g_gateway_url);
    curl_easy_setopt(curl, CURLOPT_POST,           1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS,     json_envelope);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE,  (long)strlen(json_envelope));
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER,     hdrs);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,  curl_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,      &rb);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,        10L);

    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);
    free(rb.data);

    return (res == CURLE_OK) ? 1 : 0;
}

/* ---- Channel normalizers ------------------------------------------------- */

/*
 * Normalize a Telegram Bot API update payload.
 * Extracts message.text and message.from.username.
 */
static int normalize_telegram(const char *body, char *out, size_t out_sz)
{
    char text[MAX_FIELD_BYTES]   = {0};
    char sender[256]             = {0};
    char text_esc[MAX_FIELD_BYTES * 2] = {0};
    char sender_esc[512]         = {0};
    char body_esc[MAX_FIELD_BYTES * 2] = {0};

    /* Telegram: {"update_id":..., "message":{"text":"...", "from":{"username":"..."}}} */
    json_get_string(body, "text",     text,   sizeof(text));
    json_get_string(body, "username", sender, sizeof(sender));

    /* If empty try top-level message field (already flattened) */
    if (!text[0]) {
        /* Body may be the full update; look for "text" anywhere */
        const char *tp = strstr(body, "\"text\"");
        if (tp) {
            char tmp[MAX_FIELD_BYTES] = {0};
            json_get_string(tp - 1, "text", tmp, sizeof(tmp));
            if (tmp[0]) {
                memcpy(text, tmp, sizeof(text) - 1);
                text[sizeof(text) - 1] = '\0';
            }
        }
    }

    json_escape(text,   text_esc,   sizeof(text_esc));
    json_escape(sender, sender_esc, sizeof(sender_esc));
    json_escape(body,   body_esc,   sizeof(body_esc));

    return snprintf(out, out_sz,
        "{\"channel\":\"telegram\","
        "\"sender\":\"%s\","
        "\"message\":\"%s\","
        "\"raw\":\"%s\"}",
        sender_esc, text_esc, body_esc) > 0;
}

/*
 * Normalize a Discord webhook / interaction payload.
 */
static int normalize_discord(const char *body, char *out, size_t out_sz)
{
    char content[MAX_FIELD_BYTES] = {0};
    char username[256]            = {0};
    char content_esc[MAX_FIELD_BYTES * 2] = {0};
    char username_esc[512]        = {0};
    char body_esc[MAX_FIELD_BYTES * 2]    = {0};

    json_get_string(body, "content",  content,  sizeof(content));
    json_get_string(body, "username", username, sizeof(username));

    json_escape(content,  content_esc,  sizeof(content_esc));
    json_escape(username, username_esc, sizeof(username_esc));
    json_escape(body,     body_esc,     sizeof(body_esc));

    return snprintf(out, out_sz,
        "{\"channel\":\"discord\","
        "\"sender\":\"%s\","
        "\"message\":\"%s\","
        "\"raw\":\"%s\"}",
        username_esc, content_esc, body_esc) > 0;
}

/*
 * Normalize a Slack Events API payload.
 */
static int normalize_slack(const char *body, char *out, size_t out_sz)
{
    char text[MAX_FIELD_BYTES] = {0};
    char user[256]             = {0};
    char text_esc[MAX_FIELD_BYTES * 2] = {0};
    char user_esc[512]         = {0};
    char body_esc[MAX_FIELD_BYTES * 2] = {0};

    json_get_string(body, "text", text, sizeof(text));
    json_get_string(body, "user", user, sizeof(user));

    json_escape(text,     text_esc, sizeof(text_esc));
    json_escape(user,     user_esc, sizeof(user_esc));
    json_escape(body,     body_esc, sizeof(body_esc));

    return snprintf(out, out_sz,
        "{\"channel\":\"slack\","
        "\"sender\":\"%s\","
        "\"message\":\"%s\","
        "\"raw\":\"%s\"}",
        user_esc, text_esc, body_esc) > 0;
}

/*
 * Normalize a LINE Messaging API webhook payload.
 *
 * LINE sends:
 *   {
 *     "destination": "<userId>",
 *     "events": [{
 *       "type": "message",
 *       "message": { "type": "text", "text": "..." },
 *       "source":  { "userId": "...", "type": "user" }
 *     }]
 *   }
 */
static int normalize_line(const char *body, char *out, size_t out_sz)
{
    char text[MAX_FIELD_BYTES]   = {0};
    char sender[256]             = {0};
    char text_esc[MAX_FIELD_BYTES * 2] = {0};
    char sender_esc[512]         = {0};
    char body_esc[MAX_FIELD_BYTES * 2] = {0};

    /* Extract text from events[0].message.text */
    const char *tp = strstr(body, "\"text\"");
    if (tp) {
        /* Skip to value after the colon */
        const char *vp = tp + 6;
        while (*vp == ' ' || *vp == ':' || *vp == '\t') vp++;
        if (*vp == '"') {
            vp++;
            size_t ti = 0;
            while (*vp && *vp != '"' && ti < sizeof(text) - 1)
                text[ti++] = *vp++;
            text[ti] = '\0';
        }
    }

    /* Extract userId from source.userId */
    json_get_string(body, "userId", sender, sizeof(sender));
    if (!sender[0]) {
        const char *sp = strstr(body, "\"userId\"");
        if (sp) {
            sp += 8;
            while (*sp == ' ' || *sp == ':' || *sp == '\t') sp++;
            if (*sp == '"') {
                sp++;
                size_t si = 0;
                while (*sp && *sp != '"' && si < sizeof(sender) - 1)
                    sender[si++] = *sp++;
                sender[si] = '\0';
            }
        }
    }
    if (!sender[0]) snprintf(sender, sizeof(sender), "line-user");

    json_escape(text,   text_esc,   sizeof(text_esc));
    json_escape(sender, sender_esc, sizeof(sender_esc));
    json_escape(body,   body_esc,   sizeof(body_esc));

    return snprintf(out, out_sz,
        "{\"channel\":\"line\","
        "\"sender\":\"%s\","
        "\"message\":\"%s\","
        "\"raw\":\"%s\"}",
        sender_esc, text_esc, body_esc) > 0;
}

/*
 * Normalize a WhatsApp Business API webhook payload.
 *
 * WhatsApp sends:
 *   {
 *     "object": "whatsapp_business_account",
 *     "entry": [{
 *       "changes": [{
 *         "value": {
 *           "messages": [{ "from": "...", "text": { "body": "..." } }]
 *         }
 *       }]
 *     }]
 *   }
 */
static int normalize_whatsapp(const char *body, char *out, size_t out_sz)
{
    char text[MAX_FIELD_BYTES]   = {0};
    char sender[256]             = {0};
    char text_esc[MAX_FIELD_BYTES * 2] = {0};
    char sender_esc[512]         = {0};
    char body_esc[MAX_FIELD_BYTES * 2] = {0};

    /* Extract from: messages[0].from */
    const char *fp = strstr(body, "\"from\"");
    if (fp) {
        fp += 6;
        while (*fp == ' ' || *fp == ':' || *fp == '\t') fp++;
        if (*fp == '"') {
            fp++;
            size_t si = 0;
            while (*fp && *fp != '"' && si < sizeof(sender) - 1)
                sender[si++] = *fp++;
            sender[si] = '\0';
        }
    }
    if (!sender[0]) snprintf(sender, sizeof(sender), "whatsapp-user");

    /* Extract body: messages[0].text.body */
    const char *bp = strstr(body, "\"body\"");
    if (bp) {
        bp += 6;
        while (*bp == ' ' || *bp == ':' || *bp == '\t') bp++;
        if (*bp == '"') {
            bp++;
            size_t ti = 0;
            while (*bp && *bp != '"' && ti < sizeof(text) - 1)
                text[ti++] = *bp++;
            text[ti] = '\0';
        }
    }

    json_escape(text,   text_esc,   sizeof(text_esc));
    json_escape(sender, sender_esc, sizeof(sender_esc));
    json_escape(body,   body_esc,   sizeof(body_esc));

    return snprintf(out, out_sz,
        "{\"channel\":\"whatsapp\","
        "\"sender\":\"%s\","
        "\"message\":\"%s\","
        "\"raw\":\"%s\"}",
        sender_esc, text_esc, body_esc) > 0;
}

/*
 * Generic webhook: expects { "message": "...", "sender": "..." }
 */
static int normalize_webhook(const char *body, char *out, size_t out_sz)
{
    char message[MAX_FIELD_BYTES] = {0};
    char sender[256]              = "webhook";
    char msg_esc[MAX_FIELD_BYTES * 2] = {0};
    char sender_esc[512]          = {0};
    char body_esc[MAX_FIELD_BYTES * 2]= {0};

    json_get_string(body, "message", message, sizeof(message));
    json_get_string(body, "sender",  sender,  sizeof(sender));

    json_escape(message, msg_esc,    sizeof(msg_esc));
    json_escape(sender,  sender_esc, sizeof(sender_esc));
    json_escape(body,    body_esc,   sizeof(body_esc));

    return snprintf(out, out_sz,
        "{\"channel\":\"webhook\","
        "\"sender\":\"%s\","
        "\"message\":\"%s\","
        "\"raw\":\"%s\"}",
        sender_esc, msg_esc, body_esc) > 0;
}

/* ---- connection handler -------------------------------------------------- */

static void handle_connection(int cfd, const char *req, size_t req_len)
{
    (void)req_len;

    char method[16] = {0};
    char path[256]  = {0};
    get_method(req, method, sizeof(method));
    get_path(req, path, sizeof(path));

    /* Health check */
    if (strcmp(path, "/health") == 0) {
        http_respond(cfd, 200,
            "{\"ok\":true,\"service\":\"claw-channel\",\"status\":\"healthy\"}");
        return;
    }

    /* WhatsApp GET challenge (hub.verify_token check for webhook setup) */
    if (strcmp(method, "GET") == 0 &&
        strcmp(path, "/channel/whatsapp") == 0) {
        char mode[64]      = {0};
        char token[256]    = {0};
        char challenge[256]= {0};
        get_query_param(req, "hub.mode",         mode,      sizeof(mode));
        get_query_param(req, "hub.verify_token",  token,     sizeof(token));
        get_query_param(req, "hub.challenge",     challenge, sizeof(challenge));
        if (strcmp(mode, "subscribe") == 0 &&
            g_wa_token[0] && strcmp(token, g_wa_token) == 0 && challenge[0]) {
            /* Respond with the challenge as plain text */
            char hdr[256];
            int hlen = snprintf(hdr, sizeof(hdr),
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: text/plain\r\n"
                "Content-Length: %zu\r\n"
                "Connection: close\r\n\r\n",
                strlen(challenge));
            if (hlen > 0) { ssize_t r = write(cfd, hdr, (size_t)hlen); (void)r; }
            ssize_t r = write(cfd, challenge, strlen(challenge)); (void)r;
        } else {
            http_respond(cfd, 403, "{\"ok\":false,\"error\":\"Verification failed\"}");
        }
        return;
    }

    /* Only accept POST to known paths */
    if (strcmp(method, "POST") != 0) {
        http_respond(cfd, 400, "{\"ok\":false,\"error\":\"Method must be POST\"}");
        return;
    }

    /* Validate shared secret if configured */
    if (g_secret[0]) {
        char incoming[256] = {0};
        get_header(req, "X-Claw-Secret", incoming, sizeof(incoming));
        if (strcmp(incoming, g_secret) != 0) {
            http_respond(cfd, 401, "{\"ok\":false,\"error\":\"Invalid secret\"}");
            return;
        }
    }

    const char *body = get_body(req);
    if (!body || !*body) {
        http_respond(cfd, 400, "{\"ok\":false,\"error\":\"Empty body\"}");
        return;
    }

    /* Build normalised envelope */
    char envelope[MAX_REQUEST_BYTES];
    int ok = 0;

    if (strcmp(path, "/channel/telegram") == 0) {
        ok = normalize_telegram(body, envelope, sizeof(envelope));
    } else if (strcmp(path, "/channel/discord") == 0) {
        ok = normalize_discord(body, envelope, sizeof(envelope));
    } else if (strcmp(path, "/channel/slack") == 0) {
        ok = normalize_slack(body, envelope, sizeof(envelope));
    } else if (strcmp(path, "/channel/line") == 0) {
        ok = normalize_line(body, envelope, sizeof(envelope));
    } else if (strcmp(path, "/channel/whatsapp") == 0) {
        ok = normalize_whatsapp(body, envelope, sizeof(envelope));
    } else if (strcmp(path, "/channel/webhook") == 0 ||
               strcmp(path, "/channel") == 0) {
        ok = normalize_webhook(body, envelope, sizeof(envelope));
    } else {
        http_respond(cfd, 404,
            "{\"ok\":false,\"error\":\"Unknown channel path\","
            "\"paths\":[\"/channel/telegram\",\"/channel/discord\","
            "\"/channel/slack\",\"/channel/line\",\"/channel/whatsapp\","
            "\"/channel/webhook\"]}");
        return;
    }

    if (!ok) {
        http_respond(cfd, 500, "{\"ok\":false,\"error\":\"Failed to normalize payload\"}");
        return;
    }

    /* Forward to gateway */
    if (forward_to_gateway(envelope)) {
        http_respond(cfd, 200, "{\"ok\":true,\"forwarded\":true}");
    } else {
        http_respond(cfd, 500,
            "{\"ok\":false,\"error\":\"Failed to forward to gateway\"}");
    }
}

/* ---- main ---------------------------------------------------------------- */

int main(int argc, char *argv[])
{
    /* Read config from environment */
    {
        const char *p;
        p = getenv("CLAW_CHANNEL_PORT");   if (p) g_port = atoi(p);
        p = getenv("CLAW_CHANNEL_BIND");   if (p) g_bind_addr = p;
        p = getenv("CLAW_GATEWAY_URL");
        if (p) strncpy(g_gateway_url, p, sizeof(g_gateway_url) - 1);
        p = getenv("CLAW_CHANNEL_SECRET"); if (p) strncpy(g_secret, p, sizeof(g_secret) - 1);
        p = getenv("CLAW_TELEGRAM_TOKEN"); if (p) strncpy(g_tg_token, p, sizeof(g_tg_token) - 1);
        p = getenv("CLAW_WHATSAPP_TOKEN"); if (p) strncpy(g_wa_token, p, sizeof(g_wa_token) - 1);
    }

    int opt;
    while ((opt = getopt(argc, argv, "p:b:g:s:h")) != -1) {
        switch (opt) {
            case 'p': g_port = atoi(optarg); break;
            case 'b': g_bind_addr = optarg;  break;
            case 'g': strncpy(g_gateway_url, optarg, sizeof(g_gateway_url)-1); break;
            case 's': strncpy(g_secret, optarg, sizeof(g_secret)-1); break;
            case 'h':
                fprintf(stderr,
                    "Usage: %s [-p PORT] [-b ADDR] [-g GATEWAY_URL] [-s SECRET]\n",
                    argv[0]);
                return 0;
            default:
                return 1;
        }
    }

    curl_global_init(CURL_GLOBAL_DEFAULT);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_sigterm;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT,  &sa, NULL);

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = reap_children;
    sa.sa_flags   = SA_RESTART;
    sigaction(SIGCHLD, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) { perror("socket"); return 1; }

    int one = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port   = htons((uint16_t)g_port);
    inet_pton(AF_INET, g_bind_addr, &a.sin_addr);

    if (bind(srv, (struct sockaddr *)&a, sizeof(a)) < 0) { perror("bind"); return 1; }
    if (listen(srv, BACKLOG) < 0) { perror("listen"); return 1; }

    fprintf(stdout,
        "claw-channel listening on %s:%d\n"
        "  POST /channel/telegram  — Telegram Bot API updates\n"
        "  POST /channel/discord   — Discord webhook events\n"
        "  POST /channel/slack     — Slack Events API\n"
        "  POST /channel/line      — LINE Messaging API\n"
        "  POST /channel/whatsapp  — WhatsApp Business API\n"
        "  POST /channel/webhook   — Generic webhook\n"
        "  GET  /health            — liveness probe\n"
        "  Forwarding to: %s\n",
        g_bind_addr, g_port, g_gateway_url);
    fflush(stdout);

    while (g_running) {
        struct sockaddr_in ca;
        socklen_t cl = sizeof(ca);
        int cfd = accept(srv, (struct sockaddr *)&ca, &cl);
        if (cfd < 0) {
            if (errno == EINTR) continue;
            if (!g_running) break;
            perror("accept");
            continue;
        }

        pid_t pid = fork();
        if (pid < 0) { perror("fork"); close(cfd); continue; }
        if (pid == 0) {
            close(srv);
            char req[MAX_REQUEST_BYTES];
            ssize_t n = read_request(cfd, req, sizeof(req));
            if (n > 0) handle_connection(cfd, req, (size_t)n);
            close(cfd);
            _exit(0);
        }
        close(cfd);
    }

    close(srv);
    curl_global_cleanup();
    return 0;
}
