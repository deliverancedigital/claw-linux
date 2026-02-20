/*
 * claw_json.h — Minimal JSON read/write utilities for claw-linux C binaries.
 *
 * All claw skill binaries communicate over stdin/stdout using a simple flat
 * JSON object protocol:
 *   • Input  : one JSON object on stdin  (e.g. {"command":"ls","timeout":30})
 *   • Output : one JSON object on stdout (e.g. {"ok":true,"exit_code":0,...})
 *
 * This header provides just enough JSON support for that protocol — it is
 * intentionally not a general-purpose JSON library.
 *
 * API summary
 * -----------
 *   json_get_string(json, key, out, out_size)  → 1 on success, 0 if missing
 *   json_get_long(json, key, default_val)       → parsed long or default
 *   json_escape(in, out, out_size)              → fills out with JSON-escaped in
 */
#ifndef CLAW_JSON_H
#define CLAW_JSON_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Portable unused-function suppressor */
#if defined(__GNUC__) || defined(__clang__)
#  define CLAW_MAYBE_UNUSED __attribute__((unused))
#else
#  define CLAW_MAYBE_UNUSED
#endif

/* --------------------------------------------------------------------------
 * json_get_string — extract a top-level string value from a flat JSON object.
 *
 * Handles basic JSON string escape sequences: \n \r \t \" \\ \uXXXX (BMP).
 * Returns 1 on success (out is NUL-terminated), 0 if the key is absent or the
 * value is not a JSON string.
 * -------------------------------------------------------------------------- */
static CLAW_MAYBE_UNUSED int json_get_string(const char *json, const char *key,
                            char *out, size_t out_size)
{
    if (!json || !key || !out || out_size == 0) return 0;

    /* Build search needle: "key" */
    char needle[256];
    int n = snprintf(needle, sizeof(needle), "\"%s\"", key);
    if (n < 0 || (size_t)n >= sizeof(needle)) return 0;

    const char *p = strstr(json, needle);
    if (!p) return 0;
    p += strlen(needle);

    /* Skip whitespace then colon */
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p != ':') return 0;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p != '"') return 0;
    p++; /* skip opening quote */

    size_t i = 0;
    while (*p && i < out_size - 1) {
        if (*p == '"') break; /* closing quote */
        if (*p == '\\') {
            p++;
            switch (*p) {
                case 'n':  out[i++] = '\n'; break;
                case 'r':  out[i++] = '\r'; break;
                case 't':  out[i++] = '\t'; break;
                case '"':  out[i++] = '"';  break;
                case '\\': out[i++] = '\\'; break;
                case '/':  out[i++] = '/';  break;
                case 'u': {
                    /* \uXXXX — encode BMP codepoint as UTF-8 */
                    if (p[1] && p[2] && p[3] && p[4]) {
                        char hex[5] = { p[1], p[2], p[3], p[4], '\0' };
                        unsigned long cp = strtoul(hex, NULL, 16);
                        p += 4;
                        if (cp < 0x80 && i < out_size - 1) {
                            out[i++] = (char)cp;
                        } else if (cp < 0x800 && i + 1 < out_size - 1) {
                            out[i++] = (char)(0xC0 | (cp >> 6));
                            out[i++] = (char)(0x80 | (cp & 0x3F));
                        } else if (i + 2 < out_size - 1) {
                            out[i++] = (char)(0xE0 | (cp >> 12));
                            out[i++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                            out[i++] = (char)(0x80 | (cp & 0x3F));
                        }
                    }
                    break;
                }
                default: out[i++] = *p; break;
            }
        } else {
            out[i++] = *p;
        }
        p++;
    }
    out[i] = '\0';
    return (*p == '"') ? 1 : 0;
}

/* --------------------------------------------------------------------------
 * json_get_long — extract a top-level integer/boolean value.
 *
 * Recognises JSON numbers and the literals true (→ 1) / false (→ 0).
 * Returns default_val if the key is absent or the value cannot be parsed.
 * -------------------------------------------------------------------------- */
static CLAW_MAYBE_UNUSED long json_get_long(const char *json, const char *key, long default_val)
{
    if (!json || !key) return default_val;

    char needle[256];
    int n = snprintf(needle, sizeof(needle), "\"%s\"", key);
    if (n < 0 || (size_t)n >= sizeof(needle)) return default_val;

    const char *p = strstr(json, needle);
    if (!p) return default_val;
    p += strlen(needle);

    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p != ':') return default_val;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;

    if (strncmp(p, "true",  4) == 0) return 1;
    if (strncmp(p, "false", 5) == 0) return 0;
    if ((*p >= '0' && *p <= '9') || *p == '-')
        return strtol(p, NULL, 10);

    return default_val;
}

/* --------------------------------------------------------------------------
 * json_escape — write a JSON-safe escaped version of `in` into `out`.
 *
 * out must be at least 2 * strlen(in) + 1 bytes to be safe.
 * out is always NUL-terminated.
 * -------------------------------------------------------------------------- */
static CLAW_MAYBE_UNUSED void json_escape(const char *in, char *out, size_t out_size)
{
    if (!in || !out || out_size == 0) {
        if (out && out_size > 0) out[0] = '\0';
        return;
    }
    size_t j = 0;
    for (size_t i = 0; in[i] != '\0' && j + 8 < out_size; i++) {
        unsigned char c = (unsigned char)in[i];
        switch (c) {
            case '"':  out[j++] = '\\'; out[j++] = '"';  break;
            case '\\': out[j++] = '\\'; out[j++] = '\\'; break;
            case '\n': out[j++] = '\\'; out[j++] = 'n';  break;
            case '\r': out[j++] = '\\'; out[j++] = 'r';  break;
            case '\t': out[j++] = '\\'; out[j++] = 't';  break;
            default:
                if (c < 0x20) {
                    int written = snprintf(out + j, out_size - j,
                                          "\\u%04x", (unsigned)c);
                    if (written > 0) j += (size_t)written;
                } else {
                    out[j++] = (char)c;
                }
                break;
        }
    }
    out[j] = '\0';
}

#endif /* CLAW_JSON_H */
