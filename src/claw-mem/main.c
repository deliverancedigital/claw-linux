/*
 * claw-mem — Persistent memory store for the claw-linux agent.
 *
 * Provides a simple key-value store backed by a JSON file that the agent
 * can use to persist facts, notes, and context across sessions.
 *
 * Corresponds to: openclaw/openclaw src/memory/ (persistent agent memory)
 *
 * Protocol (stdin → stdout)
 * -------------------------
 *   Input and output are single-line JSON objects on stdin/stdout.
 *
 *   Set a value:
 *     Input:  {"op":"set","key":"name","value":"Alice"}
 *     Output: {"ok":true}
 *
 *   Get a value:
 *     Input:  {"op":"get","key":"name"}
 *     Output: {"ok":true,"key":"name","value":"Alice"}
 *             {"ok":false,"error":"Key not found"}
 *
 *   Delete a key:
 *     Input:  {"op":"del","key":"name"}
 *     Output: {"ok":true}
 *
 *   List all keys:
 *     Input:  {"op":"list"}
 *     Output: {"ok":true,"keys":["name","colour"]}
 *
 *   Search values containing query string:
 *     Input:  {"op":"search","query":"Ali"}
 *     Output: {"ok":true,"results":[{"key":"name","value":"Alice"}]}
 *
 * Storage
 * -------
 *   Keys and values are stored in a flat JSON object at MEMORY_FILE.
 *   File locking (flock) prevents concurrent-write corruption.
 *
 * Configuration (environment variables)
 * --------------------------------------
 *   CLAW_MEMORY_FILE   Path to memory JSON file
 *                      (default: /var/lib/claw/memory.json)
 *
 * Build:
 *   cc -O2 -Wall -Wextra -o claw-mem main.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>

#include "../common/claw_json.h"

/* ---- tunables ------------------------------------------------------------ */
#define DEFAULT_MEMORY_FILE  "/var/lib/claw/memory.json"
#define MAX_INPUT_BYTES       65536
#define MAX_KEY_BYTES           512
#define MAX_VALUE_BYTES       32768
#define MAX_MEMORY_FILE_BYTES (4 * 1024 * 1024)  /* 4 MiB */

/* ---- helpers ------------------------------------------------------------- */

static const char *memory_file(void)
{
    const char *env = getenv("CLAW_MEMORY_FILE");
    return env ? env : DEFAULT_MEMORY_FILE;
}

/*
 * Read entire memory file into a heap buffer.  Caller must free().
 * Returns NULL on error; *out_len set to file size.
 */
static char *read_memory_file(const char *path, size_t *out_len)
{
    FILE *fp = fopen(path, "r");
    if (!fp) {
        /* File doesn't exist yet — return an empty object */
        char *empty = strdup("{}");
        if (out_len) *out_len = 2;
        return empty;
    }

    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    rewind(fp);

    if (sz <= 0 || sz > MAX_MEMORY_FILE_BYTES) {
        fclose(fp);
        char *empty = strdup("{}");
        if (out_len) *out_len = 2;
        return empty;
    }

    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(fp); return NULL; }
    size_t nr = fread(buf, 1, (size_t)sz, fp);
    buf[nr] = '\0';
    fclose(fp);
    if (out_len) *out_len = nr;
    return buf;
}

/*
 * Ensure the directory for path exists.
 */
static void ensure_dir(const char *path)
{
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
}

/*
 * Write content atomically: lock the target file, write to a .tmp file then
 * rename. Locking on the stable target prevents concurrent write races.
 */
static int write_memory_file(const char *path, const char *content)
{
    char tmp_path[512];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);

    /* Open/create the target as a stable lock anchor */
    int lock_fd = open(path, O_RDWR | O_CREAT, 0644);
    if (lock_fd < 0) {
        ensure_dir(path);
        lock_fd = open(path, O_RDWR | O_CREAT, 0644);
        if (lock_fd < 0) return -1;
    }
    if (flock(lock_fd, LOCK_EX) != 0) { close(lock_fd); return -1; }

    FILE *fp = fopen(tmp_path, "w");
    if (!fp) {
        flock(lock_fd, LOCK_UN); close(lock_fd); return -1;
    }
    fputs(content, fp);
    fputc('\n', fp);
    fflush(fp);
    int tmp_fd = fileno(fp);
    if (tmp_fd >= 0) fsync(tmp_fd);
    fclose(fp);

    int rc = rename(tmp_path, path);
    flock(lock_fd, LOCK_UN);
    close(lock_fd);
    return rc;
}

/*
 * Build a new JSON object that is `obj` with `key` set to `json_value`
 * (json_value must already be a valid JSON value, e.g. a quoted string).
 * Returns a heap-allocated string.  Caller must free().
 */
static char *json_obj_set(const char *obj, const char *key, const char *json_value)
{
    /* Remove existing key if present */
    char escaped_key[MAX_KEY_BYTES + 4];
    snprintf(escaped_key, sizeof(escaped_key), "\"%s\"", key);

    /* Build new object by copying obj, removing old key, appending new */
    size_t obj_len = strlen(obj);
    size_t val_len = strlen(json_value);
    size_t new_cap = obj_len + strlen(escaped_key) + val_len + 16;
    char *out = malloc(new_cap);
    if (!out) return NULL;

    /* Locate existing key position */
    const char *found = strstr(obj, escaped_key);
    if (found) {
        const char *colon = found + strlen(escaped_key);
        while (*colon == ' ' || *colon == '\t') colon++;
        if (*colon != ':') found = NULL; /* not actually the key */
    }

    if (!found) {
        /* Key doesn't exist — insert before closing brace */
        /* Find last '}' */
        const char *end = obj + obj_len - 1;
        while (end > obj && *end != '}') end--;

        /* Count existing keys to know if we need a comma */
        /* Simple heuristic: if there's anything between { and } besides whitespace */
        const char *inner = obj + 1;
        while (*inner == ' ' || *inner == '\t' || *inner == '\n' || *inner == '\r') inner++;
        int has_entries = (*inner != '}');

        size_t prefix_len = (size_t)(end - obj);
        memcpy(out, obj, prefix_len);
        char *p = out + prefix_len;
        if (has_entries) { *p++ = ','; *p++ = '\n'; }
        else { *p++ = '\n'; }
        memcpy(p, escaped_key, strlen(escaped_key)); p += strlen(escaped_key);
        *p++ = ':';
        memcpy(p, json_value, val_len); p += val_len;
        *p++ = '\n';
        *p++ = '}';
        *p   = '\0';
    } else {
        /* Replace existing value */
        /* Find start of existing value */
        const char *colon = found + strlen(escaped_key);
        while (*colon == ' ' || *colon == '\t') colon++;
        colon++; /* skip ':' */
        while (*colon == ' ' || *colon == '\t') colon++;

        /* Walk to end of old value */
        const char *vend = colon;
        if (*vend == '"') {
            vend++;
            while (*vend && !(*vend == '"' && *(vend-1) != '\\')) vend++;
            if (*vend == '"') vend++;
        } else if (*vend == '{' || *vend == '[') {
            char open = *vend, close = (open == '{') ? '}' : ']';
            int depth = 1; vend++;
            while (*vend && depth > 0) {
                if (*vend == open)  depth++;
                if (*vend == close) depth--;
                vend++;
            }
        } else {
            while (*vend && *vend != ',' && *vend != '}' && *vend != '\n') vend++;
        }

        size_t head  = (size_t)(colon - obj);
        size_t tail_start = (size_t)(vend - obj);
        size_t tail_len   = obj_len - tail_start;

        memcpy(out, obj, head);
        memcpy(out + head, json_value, val_len);
        memcpy(out + head + val_len, obj + tail_start, tail_len);
        out[head + val_len + tail_len] = '\0';
    }

    return out;
}

/*
 * Remove key from JSON object.
 * Returns heap-allocated new object.  Caller must free().
 */
static char *json_obj_del(const char *obj, const char *key)
{
    char escaped_key[MAX_KEY_BYTES + 4];
    snprintf(escaped_key, sizeof(escaped_key), "\"%s\"", key);

    const char *found = strstr(obj, escaped_key);
    if (!found) return strdup(obj);

    const char *colon = found + strlen(escaped_key);
    while (*colon == ' ' || *colon == '\t') colon++;
    if (*colon != ':') return strdup(obj);
    colon++;
    while (*colon == ' ' || *colon == '\t') colon++;

    /* Skip value */
    const char *vend = colon;
    if (*vend == '"') {
        vend++;
        while (*vend && !(*vend == '"' && *(vend-1) != '\\')) vend++;
        if (*vend) vend++;
    } else if (*vend == '{' || *vend == '[') {
        char open = *vend, close = (open == '{') ? '}' : ']';
        int depth = 1; vend++;
        while (*vend && depth > 0) {
            if (*vend == open)  depth++;
            if (*vend == close) depth--;
            vend++;
        }
    } else {
        while (*vend && *vend != ',' && *vend != '}' && *vend != '\n') vend++;
    }

    /* Skip surrounding comma and whitespace */
    const char *pre  = found;
    const char *post = vend;

    /* If there's a leading comma (key was not first), remove it */
    while (pre > obj && (*(pre-1) == ' ' || *(pre-1) == '\t' || *(pre-1) == '\n')) pre--;
    if (pre > obj && *(pre-1) == ',') pre--;
    else {
        /* Key was first — remove trailing comma if any */
        while (*post == ' ' || *post == '\t' || *post == '\n') post++;
        if (*post == ',') post++;
    }

    size_t head_len = (size_t)(pre - obj);
    size_t tail_len = strlen(post);
    char *out = malloc(head_len + tail_len + 1);
    if (!out) return NULL;
    memcpy(out, obj, head_len);
    memcpy(out + head_len, post, tail_len);
    out[head_len + tail_len] = '\0';
    return out;
}

/* ---- operations ---------------------------------------------------------- */

static int op_set(const char *json_in)
{
    char key[MAX_KEY_BYTES]   = {0};
    char value[MAX_VALUE_BYTES] = {0};

    if (!json_get_string(json_in, "key", key, sizeof(key)) || key[0] == '\0') {
        puts("{\"ok\":false,\"error\":\"Missing or empty 'key' field\"}");
        return 1;
    }
    if (!json_get_string(json_in, "value", value, sizeof(value))) {
        puts("{\"ok\":false,\"error\":\"Missing 'value' field\"}");
        return 1;
    }

    const char *path = memory_file();
    char *mem = read_memory_file(path, NULL);
    if (!mem) { puts("{\"ok\":false,\"error\":\"Out of memory\"}"); return 1; }

    /* JSON-escape the value for storage */
    size_t esc_cap = strlen(value) * 2 + 4;
    char *esc_val  = malloc(esc_cap);
    if (!esc_val) { free(mem); puts("{\"ok\":false,\"error\":\"Out of memory\"}"); return 1; }
    esc_val[0] = '"';
    json_escape(value, esc_val + 1, esc_cap - 2);
    size_t esc_len = strlen(esc_val);
    esc_val[esc_len] = '"';
    esc_val[esc_len + 1] = '\0';

    char *updated = json_obj_set(mem, key, esc_val);
    free(esc_val);
    free(mem);

    if (!updated) { puts("{\"ok\":false,\"error\":\"Internal error\"}"); return 1; }

    int rc = 0;
    if (write_memory_file(path, updated) < 0) {
        puts("{\"ok\":false,\"error\":\"Failed to write memory file\"}");
        rc = 1;
    } else {
        puts("{\"ok\":true}");
    }
    free(updated);
    return rc;
}

static int op_get(const char *json_in)
{
    char key[MAX_KEY_BYTES] = {0};
    if (!json_get_string(json_in, "key", key, sizeof(key)) || key[0] == '\0') {
        puts("{\"ok\":false,\"error\":\"Missing or empty 'key' field\"}");
        return 1;
    }

    char *mem = read_memory_file(memory_file(), NULL);
    if (!mem) { puts("{\"ok\":false,\"error\":\"Out of memory\"}"); return 1; }

    char value[MAX_VALUE_BYTES] = {0};
    if (!json_get_string(mem, key, value, sizeof(value))) {
        free(mem);
        char esc_key[MAX_KEY_BYTES * 2 + 4] = {0};
        json_escape(key, esc_key, sizeof(esc_key));
        printf("{\"ok\":false,\"error\":\"Key not found\",\"key\":\"%s\"}\n", esc_key);
        return 1;
    }
    free(mem);

    char esc_key[MAX_KEY_BYTES * 2] = {0};
    char esc_val[MAX_VALUE_BYTES * 2] = {0};
    json_escape(key,   esc_key, sizeof(esc_key));
    json_escape(value, esc_val, sizeof(esc_val));
    printf("{\"ok\":true,\"key\":\"%s\",\"value\":\"%s\"}\n", esc_key, esc_val);
    return 0;
}

static int op_del(const char *json_in)
{
    char key[MAX_KEY_BYTES] = {0};
    if (!json_get_string(json_in, "key", key, sizeof(key)) || key[0] == '\0') {
        puts("{\"ok\":false,\"error\":\"Missing or empty 'key' field\"}");
        return 1;
    }

    const char *path = memory_file();
    char *mem = read_memory_file(path, NULL);
    if (!mem) { puts("{\"ok\":false,\"error\":\"Out of memory\"}"); return 1; }

    char *updated = json_obj_del(mem, key);
    free(mem);
    if (!updated) { puts("{\"ok\":false,\"error\":\"Internal error\"}"); return 1; }

    int rc = 0;
    if (write_memory_file(path, updated) < 0) {
        puts("{\"ok\":false,\"error\":\"Failed to write memory file\"}");
        rc = 1;
    } else {
        puts("{\"ok\":true}");
    }
    free(updated);
    return rc;
}

static int op_list(void)
{
    char *mem = read_memory_file(memory_file(), NULL);
    if (!mem) { puts("{\"ok\":false,\"error\":\"Out of memory\"}"); return 1; }

    /* Collect all keys from the JSON object */
    char keys_buf[65536] = {0};
    size_t keys_len = 0;
    int    count    = 0;

    const char *p = mem;
    while (*p) {
        /* Find next "key": inside object */
        const char *q = strchr(p, '"');
        if (!q) break;
        q++; /* skip opening quote */
        const char *key_start = q;
        while (*q && *q != '"') q++;
        if (!*q) break;
        size_t klen = (size_t)(q - key_start);
        q++; /* skip closing quote */
        while (*q == ' ' || *q == '\t') q++;
        if (*q != ':') { p = q; continue; } /* not a key-value pair */

        /* It's a key */
        char k[MAX_KEY_BYTES] = {0};
        if (klen >= sizeof(k)) klen = sizeof(k) - 1;
        memcpy(k, key_start, klen);
        k[klen] = '\0';

        char esc[MAX_KEY_BYTES * 2] = {0};
        json_escape(k, esc, sizeof(esc));

        int n = snprintf(keys_buf + keys_len, sizeof(keys_buf) - keys_len,
                         "%s\"%s\"", count > 0 ? "," : "", esc);
        if (n > 0) keys_len += (size_t)n;
        count++;
        p = q + 1;
    }
    free(mem);

    printf("{\"ok\":true,\"count\":%d,\"keys\":[%s]}\n", count, keys_buf);
    return 0;
}

static int op_search(const char *json_in)
{
    char query[MAX_VALUE_BYTES] = {0};
    if (!json_get_string(json_in, "query", query, sizeof(query)) || query[0] == '\0') {
        puts("{\"ok\":false,\"error\":\"Missing or empty 'query' field\"}");
        return 1;
    }

    char *mem = read_memory_file(memory_file(), NULL);
    if (!mem) { puts("{\"ok\":false,\"error\":\"Out of memory\"}"); return 1; }

    char results[131072] = {0};
    size_t res_len = 0;
    int    count   = 0;

    const char *p = mem;
    while (*p) {
        const char *q = strchr(p, '"');
        if (!q) break;
        q++;
        const char *key_start = q;
        while (*q && *q != '"') q++;
        if (!*q) break;
        size_t klen = (size_t)(q - key_start);
        q++;
        while (*q == ' ' || *q == '\t') q++;
        if (*q != ':') { p = q; continue; }

        char k[MAX_KEY_BYTES] = {0};
        if (klen >= sizeof(k)) klen = sizeof(k) - 1;
        memcpy(k, key_start, klen);
        k[klen] = '\0';

        char v[MAX_VALUE_BYTES] = {0};
        json_get_string(mem, k, v, sizeof(v));

        if (strstr(k, query) || strstr(v, query)) {
            char esc_k[MAX_KEY_BYTES * 2]   = {0};
            char esc_v[MAX_VALUE_BYTES * 2] = {0};
            json_escape(k, esc_k, sizeof(esc_k));
            json_escape(v, esc_v, sizeof(esc_v));
            int n = snprintf(results + res_len, sizeof(results) - res_len,
                             "%s{\"key\":\"%s\",\"value\":\"%s\"}",
                             count > 0 ? "," : "", esc_k, esc_v);
            if (n > 0) res_len += (size_t)n;
            count++;
        }
        p = q + 1;
    }
    free(mem);

    printf("{\"ok\":true,\"count\":%d,\"results\":[%s]}\n", count, results);
    return 0;
}

/* ---- main ---------------------------------------------------------------- */

int main(void)
{
    char input[MAX_INPUT_BYTES];
    if (!fgets(input, sizeof(input), stdin)) {
        puts("{\"ok\":false,\"error\":\"No input\"}");
        return 1;
    }

    char op[64] = {0};
    if (!json_get_string(input, "op", op, sizeof(op)) || op[0] == '\0') {
        puts("{\"ok\":false,\"error\":\"Missing 'op' field (set|get|del|list|search)\"}");
        return 1;
    }

    if      (strcmp(op, "set")    == 0) return op_set(input);
    else if (strcmp(op, "get")    == 0) return op_get(input);
    else if (strcmp(op, "del")    == 0) return op_del(input);
    else if (strcmp(op, "list")   == 0) return op_list();
    else if (strcmp(op, "search") == 0) return op_search(input);
    else {
        char esc[128] = {0};
        json_escape(op, esc, sizeof(esc));
        printf("{\"ok\":false,\"error\":\"Unknown op '%s' — use set|get|del|list|search\"}\n", esc);
        return 1;
    }

    return 0;
}
