/*
 * claw-tui — Interactive terminal chat UI for claw-linux.
 *
 * Provides a readline-style terminal interface that connects to the local
 * claw-gateway HTTP API and lets a user send messages to the agent and
 * read its replies — without a web browser or GUI.
 *
 * Corresponds to: openclaw/openclaw src/tui/ (terminal UI) and
 *                 src/cli/tui-cli.ts
 *
 * Usage
 * -----
 *   claw-tui [-g GATEWAY_URL] [-s SESSION_ID]
 *
 *   Default gateway URL : http://127.0.0.1:18789
 *   Default session ID  : "default"
 *
 * Interactive commands (type at the prompt)
 * ------------------------------------------
 *   <message>    — send message to the agent and display the reply
 *   /status      — query gateway health and status
 *   /session     — show current session ID
 *   /session ID  — switch to a different session
 *   /clear       — clear the terminal screen
 *   /help        — show available commands
 *   /quit or EOF — exit
 *
 * Protocol
 * --------
 *   POST <gateway>/api/message  { "message": "...", "session": "..." }
 *   GET  <gateway>/api/health
 *   GET  <gateway>/api/status
 *
 * Environment variables
 * ----------------------
 *   CLAW_GATEWAY_URL   Override gateway URL (default http://127.0.0.1:18789)
 *   CLAW_SESSION_ID    Default session identifier (default "default")
 *
 * Build (requires libcurl-dev):
 *   cc -O2 -Wall -Wextra -o claw-tui main.c -lcurl
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <curl/curl.h>

#include "../common/claw_json.h"

/* ---- tunables ------------------------------------------------------------ */
#define DEFAULT_GATEWAY    "http://127.0.0.1:18789"
#define DEFAULT_SESSION    "default"
#define MAX_INPUT_BYTES     8192
#define MAX_URL_BYTES        512
#define MAX_SESSION_BYTES    256
#define MAX_RESPONSE_BYTES (256 * 1024)   /* 256 KiB */
#define REQUEST_TIMEOUT_S     60

/* ---- globals ------------------------------------------------------------- */
static char g_gateway[MAX_URL_BYTES]   = DEFAULT_GATEWAY;
static char g_session[MAX_SESSION_BYTES] = DEFAULT_SESSION;

/* ---- response buffer ----------------------------------------------------- */

typedef struct {
    char   *data;
    size_t  used;
    size_t  cap;
} CurlBuf;

static size_t curl_write(void *ptr, size_t size, size_t nmemb, void *userdata)
{
    CurlBuf *b = (CurlBuf *)userdata;
    size_t inc = size * nmemb;
    if (b->used + inc + 1 > b->cap) {
        size_t new_cap = b->cap * 2 + inc + 1;
        if (new_cap > MAX_RESPONSE_BYTES + 1)
            new_cap = MAX_RESPONSE_BYTES + 1;
        if (b->used + inc + 1 > new_cap)
            return 0; /* cap exceeded — abort */
        char *tmp = realloc(b->data, new_cap);
        if (!tmp) return 0;
        b->data = tmp;
        b->cap  = new_cap;
    }
    memcpy(b->data + b->used, ptr, inc);
    b->used += inc;
    b->data[b->used] = '\0';
    return inc;
}

/* ---- HTTP helpers -------------------------------------------------------- */

/*
 * Perform a GET request to `url`.  Writes response body into *out (caller
 * must free).  Returns HTTP status code, or 0 on transport error.
 */
static long http_get(const char *url, char **out)
{
    *out = NULL;
    CURL *curl = curl_easy_init();
    if (!curl) return 0;

    CurlBuf rb = { malloc(4096), 0, 4096 };
    if (!rb.data) { curl_easy_cleanup(curl); return 0; }
    rb.data[0] = '\0';

    curl_easy_setopt(curl, CURLOPT_URL,           url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,     &rb);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,       (long)REQUEST_TIMEOUT_S);
    curl_easy_setopt(curl, CURLOPT_USERAGENT,     "claw-tui/1.0");
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);

    CURLcode res = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        free(rb.data);
        fprintf(stderr, "claw-tui: HTTP GET error: %s\n", curl_easy_strerror(res));
        return 0;
    }
    *out = rb.data;
    return status;
}

/*
 * Perform a POST request to `url` with a JSON body.  Writes response body
 * into *out (caller must free).  Returns HTTP status code, or 0 on error.
 */
static long http_post_json(const char *url, const char *body, char **out)
{
    *out = NULL;
    CURL *curl = curl_easy_init();
    if (!curl) return 0;

    CurlBuf rb = { malloc(4096), 0, 4096 };
    if (!rb.data) { curl_easy_cleanup(curl); return 0; }
    rb.data[0] = '\0';

    struct curl_slist *hdrs = NULL;
    hdrs = curl_slist_append(hdrs, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL,            url);
    curl_easy_setopt(curl, CURLOPT_POST,           1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS,     body);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE,  (long)strlen(body));
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER,     hdrs);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,  curl_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,      &rb);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,        (long)REQUEST_TIMEOUT_S);
    curl_easy_setopt(curl, CURLOPT_USERAGENT,      "claw-tui/1.0");

    CURLcode res = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        free(rb.data);
        fprintf(stderr, "claw-tui: HTTP POST error: %s\n", curl_easy_strerror(res));
        return 0;
    }
    *out = rb.data;
    return status;
}

/* ---- pretty-print response ----------------------------------------------- */

/*
 * Given a JSON response body from the gateway, extract the meaningful fields
 * and print a human-readable reply to stdout.
 */
static void print_response(const char *json_body)
{
    if (!json_body || !*json_body) {
        printf("(empty response)\n");
        return;
    }

    /* Try to extract "reply" or "response" or "message" field */
    char reply[MAX_RESPONSE_BYTES];
    if (json_get_string(json_body, "reply", reply, sizeof(reply)) ||
        json_get_string(json_body, "response", reply, sizeof(reply)) ||
        json_get_string(json_body, "message", reply, sizeof(reply))) {
        printf("\n\033[1;36mAgent:\033[0m %s\n\n", reply);
        return;
    }

    /* Fallback: check for error */
    char err[1024];
    if (json_get_string(json_body, "error", err, sizeof(err))) {
        printf("\n\033[1;31mError:\033[0m %s\n\n", err);
        return;
    }

    /* Last resort: print the raw JSON */
    printf("\n\033[2m%s\033[0m\n\n", json_body);
}

/* ---- built-in commands --------------------------------------------------- */

static void cmd_help(void)
{
    printf(
        "\n"
        "  \033[1mclaw-tui\033[0m — interactive chat with the claw agent\n"
        "\n"
        "  Type a message and press Enter to send it to the agent.\n"
        "\n"
        "  Commands:\n"
        "    /status          — query gateway health and status\n"
        "    /session         — show current session ID\n"
        "    /session <ID>    — switch to a different session\n"
        "    /clear           — clear the screen\n"
        "    /help            — show this help\n"
        "    /quit            — exit (or press Ctrl-D)\n"
        "\n"
        "  Gateway: %s\n"
        "  Session: %s\n"
        "\n",
        g_gateway, g_session);
}

static void cmd_status(void)
{
    char url[MAX_URL_BYTES * 2];
    snprintf(url, sizeof(url), "%s/api/status", g_gateway);

    char *body = NULL;
    long code = http_get(url, &body);
    if (code == 0) {
        printf("\033[1;31mCannot reach gateway at %s\033[0m\n", g_gateway);
        printf("Is claw-gateway running?  Try: claw-daemon start gateway\n\n");
    } else {
        printf("\nGateway status (%s):\n%s\n\n", g_gateway, body ? body : "(empty)");
    }
    free(body);
}

static void cmd_send(const char *text)
{
    /* Build JSON request */
    char msg_esc[MAX_INPUT_BYTES * 2];
    char session_esc[MAX_SESSION_BYTES * 2];
    json_escape(text, msg_esc, sizeof(msg_esc));
    json_escape(g_session, session_esc, sizeof(session_esc));

    char payload[MAX_INPUT_BYTES * 2 + MAX_SESSION_BYTES * 2 + 64];
    snprintf(payload, sizeof(payload),
             "{\"message\":\"%s\",\"session\":\"%s\"}",
             msg_esc, session_esc);

    char url[MAX_URL_BYTES * 2];
    snprintf(url, sizeof(url), "%s/api/message", g_gateway);

    char *body = NULL;
    long code = http_post_json(url, payload, &body);

    if (code == 0) {
        printf("\033[1;31mError:\033[0m Cannot reach gateway at %s\n", g_gateway);
        printf("Is claw-gateway running?  Try: claw-daemon start gateway\n\n");
    } else if (code != 200) {
        printf("\033[1;31mGateway returned HTTP %ld:\033[0m\n%s\n\n",
               code, body ? body : "(empty)");
    } else {
        print_response(body);
    }
    free(body);
}

/* ---- main loop ----------------------------------------------------------- */

static volatile int g_done = 0;

static void on_sigint(int sig)
{
    (void)sig;
    /* Print a newline so the cursor is on a fresh line */
    ssize_t r = write(STDOUT_FILENO, "\n", 1);
    (void)r;
    g_done = 1;
}

int main(int argc, char *argv[])
{
    /* Read config from environment */
    const char *env;

    env = getenv("CLAW_GATEWAY_URL");
    if (env && *env) {
        strncpy(g_gateway, env, sizeof(g_gateway) - 1);
        g_gateway[sizeof(g_gateway) - 1] = '\0';
    }

    env = getenv("CLAW_SESSION_ID");
    if (env && *env) {
        strncpy(g_session, env, sizeof(g_session) - 1);
        g_session[sizeof(g_session) - 1] = '\0';
    }

    /* Parse command-line options */
    int opt;
    while ((opt = getopt(argc, argv, "g:s:h")) != -1) {
        switch (opt) {
            case 'g':
                strncpy(g_gateway, optarg, sizeof(g_gateway) - 1);
                g_gateway[sizeof(g_gateway) - 1] = '\0';
                break;
            case 's':
                strncpy(g_session, optarg, sizeof(g_session) - 1);
                g_session[sizeof(g_session) - 1] = '\0';
                break;
            case 'h':
                fprintf(stderr,
                    "Usage: %s [-g GATEWAY_URL] [-s SESSION_ID]\n"
                    "\n"
                    "  -g URL   Gateway base URL (default: %s)\n"
                    "  -s ID    Session identifier (default: %s)\n"
                    "  -h       Show this help\n",
                    argv[0], DEFAULT_GATEWAY, DEFAULT_SESSION);
                return 0;
            default:
                return 1;
        }
    }

    curl_global_init(CURL_GLOBAL_DEFAULT);

    /* Handle Ctrl-C gracefully */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_sigint;
    sigaction(SIGINT, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    /* Banner */
    int is_tty = isatty(STDIN_FILENO);
    if (is_tty) {
        printf("\033[1;32m");
        printf("╔════════════════════════════════════╗\n");
        printf("║       claw-tui — agent chat        ║\n");
        printf("╚════════════════════════════════════╝\033[0m\n");
        printf("Gateway : %s\n", g_gateway);
        printf("Session : %s\n\n", g_session);
        printf("Type \033[1m/help\033[0m for commands, \033[1m/quit\033[0m or Ctrl-D to exit.\n\n");
    }

    /* Main read loop */
    char line[MAX_INPUT_BYTES];

    while (!g_done) {
        if (is_tty) {
            printf("\033[1;32mclaw>\033[0m ");
            fflush(stdout);
        }

        if (!fgets(line, sizeof(line), stdin)) {
            /* EOF */
            if (is_tty) printf("\n");
            break;
        }

        /* Strip trailing newline / carriage return */
        size_t len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r'))
            line[--len] = '\0';

        /* Skip blank lines */
        if (len == 0) continue;

        /* Built-in commands */
        if (strcmp(line, "/quit") == 0 || strcmp(line, "/exit") == 0) {
            break;
        } else if (strcmp(line, "/help") == 0) {
            cmd_help();
        } else if (strcmp(line, "/status") == 0) {
            cmd_status();
        } else if (strcmp(line, "/clear") == 0) {
            printf("\033[2J\033[H");
            fflush(stdout);
        } else if (strcmp(line, "/session") == 0) {
            printf("Current session: %s\n\n", g_session);
        } else if (strncmp(line, "/session ", 9) == 0) {
            const char *new_id = line + 9;
            while (*new_id == ' ') new_id++;
            if (*new_id) {
                strncpy(g_session, new_id, sizeof(g_session) - 1);
                g_session[sizeof(g_session) - 1] = '\0';
                printf("Session switched to: %s\n\n", g_session);
            } else {
                printf("Usage: /session <ID>\n\n");
            }
        } else if (line[0] == '/') {
            printf("Unknown command: %s  (type /help for available commands)\n\n", line);
        } else {
            /* Regular message — send to agent */
            cmd_send(line);
        }
    }

    if (is_tty) printf("Goodbye.\n");
    curl_global_cleanup();
    return 0;
}
