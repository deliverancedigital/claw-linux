/*
 * claw-fetch — Web fetch skill binary for claw-linux.
 *
 * Fetches a URL using libcurl and returns the HTTP response as JSON.
 *
 * Protocol (stdin → stdout, one JSON object each):
 *
 *   Input:
 *     {
 *       "url":       "https://example.com",
 *       "method":    "GET",           // optional, default GET
 *       "timeout":   15,              // seconds, optional, default 15
 *       "max_bytes": 1048576          // response cap, optional, default 1 MiB
 *     }
 *
 *   Output (success):
 *     { "ok": true, "status_code": 200, "body": "<response body>" }
 *
 *   Output (error):
 *     { "ok": false, "error": "<reason>" }
 *
 * Build (requires libcurl-dev):
 *   cc -O2 -Wall -Wextra -o claw-fetch main.c -lcurl
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <curl/curl.h>

#include "../common/claw_json.h"

/* ---- tunables ------------------------------------------------------------ */
#define MAX_INPUT_BYTES      65536
#define MAX_URL_BYTES         2048
#define MAX_METHOD_BYTES        16
#define DEFAULT_TIMEOUT           15    /* seconds */
#define MAX_TIMEOUT              300    /* 5 minutes hard cap */
#define DEFAULT_MAX_BYTES   (1024 * 1024)   /* 1 MiB */
#define ABS_MAX_BYTES      (32 * 1024 * 1024) /* 32 MiB hard cap */

#define USER_AGENT "claw-linux/1.0 (OpenClaw-compatible autonomous agent; +https://github.com/deliverancedigital/claw-linux)"

/* ---- response buffer ----------------------------------------------------- */

typedef struct {
    char   *data;
    size_t  used;
    size_t  cap;
    size_t  limit;   /* max bytes to store */
} ResponseBuf;

static size_t write_callback(void *ptr, size_t size, size_t nmemb, void *userdata)
{
    size_t incoming = size * nmemb;
    ResponseBuf *rb = (ResponseBuf *)userdata;

    /* Enforce the per-request cap */
    if (rb->used >= rb->limit)
        return 0; /* signal curl to abort */

    size_t accept = incoming;
    if (rb->used + accept > rb->limit)
        accept = rb->limit - rb->used;

    /* Grow buffer */
    if (rb->used + accept + 1 > rb->cap) {
        size_t new_cap = rb->cap * 2 + accept + 1;
        char *tmp = realloc(rb->data, new_cap);
        if (!tmp) return 0;
        rb->data = tmp;
        rb->cap  = new_cap;
    }

    memcpy(rb->data + rb->used, ptr, accept);
    rb->used += accept;
    rb->data[rb->used] = '\0';
    return incoming; /* always return the full incoming size so curl stays happy */
}

/* ---- helpers ------------------------------------------------------------- */

static void emit_error(const char *msg)
{
    char esc[4096];
    json_escape(msg, esc, sizeof(esc));
    printf("{\"ok\":false,\"error\":\"%s\"}\n", esc);
    fflush(stdout);
}

/* ---- main ---------------------------------------------------------------- */

int main(void)
{
    /* 1. Read JSON request from stdin */
    char input[MAX_INPUT_BYTES];
    size_t total = 0;
    ssize_t n;
    while (total < sizeof(input) - 1) {
        n = read(STDIN_FILENO, input + total, sizeof(input) - 1 - total);
        if (n <= 0) break;
        total += (size_t)n;
    }
    input[total] = '\0';

    /* 2. Parse fields */
    char url[MAX_URL_BYTES]       = {0};
    char method[MAX_METHOD_BYTES] = "GET";

    if (!json_get_string(input, "url", url, sizeof(url))) {
        emit_error("Missing or invalid 'url' field");
        return 0;
    }
    /* Optional: method */
    json_get_string(input, "method", method, sizeof(method));

    /* Convert method to uppercase */
    for (int i = 0; method[i]; i++)
        if (method[i] >= 'a' && method[i] <= 'z')
            method[i] = (char)(method[i] - 32);

    long timeout  = json_get_long(input, "timeout",   DEFAULT_TIMEOUT);
    long max_bytes = json_get_long(input, "max_bytes", DEFAULT_MAX_BYTES);

    if (timeout   <= 0 || timeout   > MAX_TIMEOUT)   timeout   = DEFAULT_TIMEOUT;
    if (max_bytes <= 0 || max_bytes > ABS_MAX_BYTES)  max_bytes = DEFAULT_MAX_BYTES;

    /* 3. Basic URL validation — must start with http:// or https:// */
    if (strncmp(url, "http://",  7) != 0 &&
        strncmp(url, "https://", 8) != 0) {
        emit_error("URL must start with http:// or https://");
        return 0;
    }

    /* 4. Initialise libcurl */
    CURL *curl = curl_easy_init();
    if (!curl) {
        emit_error("curl_easy_init() failed");
        return 1;
    }

    /* 5. Prepare response buffer */
    ResponseBuf rb = {0};
    rb.cap   = 4096;
    rb.limit = (size_t)max_bytes;
    rb.data  = malloc(rb.cap);
    if (!rb.data) {
        curl_easy_cleanup(curl);
        emit_error("Memory allocation failed");
        return 1;
    }
    rb.data[0] = '\0';

    /* 6. Configure curl */
    curl_easy_setopt(curl, CURLOPT_URL,            url);
    curl_easy_setopt(curl, CURLOPT_USERAGENT,      USER_AGENT);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,        timeout);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS,      5L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,  write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,      &rb);

    /* TLS: verify peer certificate */
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);

    /* Method */
    if (strcmp(method, "HEAD") == 0) {
        curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
    } else if (strcmp(method, "POST") == 0) {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, "");
    } else if (strcmp(method, "GET") != 0) {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);
    }

    /* 7. Execute */
    CURLcode res = curl_easy_perform(curl);

    long status_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status_code);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK && res != CURLE_WRITE_ERROR /* max_bytes truncation */) {
        char errmsg[256];
        snprintf(errmsg, sizeof(errmsg), "curl error: %s", curl_easy_strerror(res));
        emit_error(errmsg);
        free(rb.data);
        return 0;
    }

    /* 8. JSON-escape the body and emit result */
    size_t esc_size = rb.used * 2 + 16;
    if (esc_size < 64) esc_size = 64;
    char *body_esc = malloc(esc_size);
    if (!body_esc) {
        free(rb.data);
        emit_error("Memory allocation failed");
        return 1;
    }

    json_escape(rb.data, body_esc, esc_size);
    printf("{\"ok\":true,\"status_code\":%ld,\"body\":\"%s\"}\n",
           status_code, body_esc);
    fflush(stdout);

    free(rb.data);
    free(body_esc);
    return 0;
}
