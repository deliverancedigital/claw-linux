/*
 * claw-link — Link preview and understanding for claw-linux.
 *
 * Fetches a URL and extracts structured metadata: page title, description,
 * Open Graph tags, and canonical URL.  Used by the agent to understand the
 * content of a link before presenting it to the user.
 *
 * Corresponds to: openclaw/openclaw src/link-understanding/
 *
 * Protocol (stdin → stdout)
 * -------------------------
 *   Input:  {"url":"https://example.com"}
 *   Output: {
 *             "ok": true,
 *             "url": "https://example.com",
 *             "canonical": "https://example.com/",
 *             "title":       "Example Domain",
 *             "description": "An illustrative example.",
 *             "image":       "https://example.com/og.png",
 *             "site_name":   "Example",
 *             "type":        "website"
 *           }
 *
 * Or as a CLI tool:
 *   claw-link https://example.com
 *
 * Configuration (environment variables)
 * --------------------------------------
 *   CLAW_LINK_TIMEOUT   HTTP timeout in seconds (default: 15)
 *   CLAW_LINK_MAX_BYTES Maximum bytes to fetch (default: 512 KiB)
 *   CLAW_LINK_UA        User-agent string
 *
 * Build (requires libcurl-dev):
 *   cc -O2 -Wall -Wextra -o claw-link main.c -lcurl
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <curl/curl.h>

#include "../common/claw_json.h"

/* ---- tunables ------------------------------------------------------------ */
#define DEFAULT_TIMEOUT_S   15
#define DEFAULT_MAX_BYTES   (512 * 1024)
#define MAX_INPUT_BYTES      4096
#define MAX_URL_BYTES        2048
#define MAX_FIELD_BYTES      1024
#define DEFAULT_UA  "claw-link/1.0 (claw-linux; +https://github.com/deliverancedigital/claw-linux)"

/* ---- response buffer ----------------------------------------------------- */

typedef struct {
    char   *data;
    size_t  used;
    size_t  cap;
    size_t  limit;
} Buf;

static size_t buf_write(void *ptr, size_t size, size_t nmemb, void *userdata)
{
    Buf   *b   = (Buf *)userdata;
    size_t inc = size * nmemb;

    if (b->used + inc > b->limit) {
        /* Truncate to limit — return inc to avoid curl error */
        inc = b->limit - b->used;
        if (inc == 0) return size * nmemb; /* silently drop */
    }

    if (b->used + inc + 1 > b->cap) {
        size_t new_cap = b->cap ? b->cap * 2 : 65536;
        while (new_cap < b->used + inc + 1) new_cap *= 2;
        if (new_cap > b->limit + 1) new_cap = b->limit + 1;
        char *p = realloc(b->data, new_cap);
        if (!p) return 0;
        b->data = p;
        b->cap  = new_cap;
    }
    memcpy(b->data + b->used, ptr, inc);
    b->used += inc;
    b->data[b->used] = '\0';
    return size * nmemb;
}

/* ---- HTML meta extraction ------------------------------------------------ */

/*
 * Case-insensitive strstr.
 */
static const char *ci_strstr(const char *haystack, const char *needle)
{
    size_t nlen = strlen(needle);
    for (; *haystack; haystack++) {
        if (strncasecmp(haystack, needle, nlen) == 0)
            return haystack;
    }
    return NULL;
}

/*
 * Extract the value of an HTML attribute from a tag string.
 * e.g. attr_val("<meta content=\"hello\" />", "content", buf, sz) → "hello"
 */
static int attr_val(const char *tag, const char *attr_name,
                    char *out, size_t out_size)
{
    char needle[128];
    snprintf(needle, sizeof(needle), "%s=", attr_name);
    const char *p = ci_strstr(tag, needle);
    if (!p) return 0;
    p += strlen(needle);
    if (*p == '"' || *p == '\'') {
        char delim = *p++;
        const char *start = p;
        while (*p && *p != delim) p++;
        size_t len = (size_t)(p - start);
        if (len >= out_size) len = out_size - 1;
        memcpy(out, start, len);
        out[len] = '\0';
        return 1;
    }
    /* Unquoted attribute */
    const char *start = p;
    while (*p && *p != '>' && *p != ' ' && *p != '\t' && *p != '\n') p++;
    size_t len = (size_t)(p - start);
    if (len >= out_size) len = out_size - 1;
    memcpy(out, start, len);
    out[len] = '\0';
    return len > 0;
}

/*
 * Decode common HTML entities in-place.
 */
static void decode_entities(char *s)
{
    static const struct { const char *ent; char ch; } map[] = {
        {"&amp;",  '&'}, {"&lt;",   '<'}, {"&gt;",   '>'},
        {"&quot;", '"'}, {"&#39;",  '\''}, {"&nbsp;", ' '},
        {NULL, 0}
    };
    char *r = s, *w = s;
    while (*r) {
        if (*r == '&') {
            int found = 0;
            for (int i = 0; map[i].ent; i++) {
                size_t elen = strlen(map[i].ent);
                if (strncmp(r, map[i].ent, elen) == 0) {
                    *w++ = map[i].ch;
                    r   += elen;
                    found = 1;
                    break;
                }
            }
            if (found) continue;
        }
        *w++ = *r++;
    }
    *w = '\0';
}

typedef struct {
    char title[MAX_FIELD_BYTES];
    char description[MAX_FIELD_BYTES];
    char image[MAX_FIELD_BYTES];
    char canonical[MAX_FIELD_BYTES];
    char site_name[MAX_FIELD_BYTES];
    char og_type[128];
} Meta;

static void parse_meta(const char *html, const char *final_url, Meta *m)
{
    memset(m, 0, sizeof(*m));

    /* Extract <title>...</title> */
    const char *tp = ci_strstr(html, "<title");
    if (tp) {
        const char *ts = strchr(tp, '>');
        if (ts) {
            ts++;
            const char *te = ci_strstr(ts, "</title>");
            if (te) {
                size_t len = (size_t)(te - ts);
                if (len >= sizeof(m->title)) len = sizeof(m->title) - 1;
                memcpy(m->title, ts, len);
                m->title[len] = '\0';
                decode_entities(m->title);
                /* trim whitespace */
                char *t = m->title;
                while (isspace((unsigned char)*t)) t++;
                memmove(m->title, t, strlen(t) + 1);
                size_t tl = strlen(m->title);
                while (tl > 0 && isspace((unsigned char)m->title[tl-1]))
                    m->title[--tl] = '\0';
            }
        }
    }

    /* Scan <meta …> and <link …> tags */
    const char *p = html;
    while (p && *p) {
        const char *tag_start;

        /* Find next <meta or <link */
        const char *ms = ci_strstr(p, "<meta ");
        const char *ls = ci_strstr(p, "<link ");
        if (!ms && !ls) break;

        if (!ms) tag_start = ls;
        else if (!ls) tag_start = ms;
        else tag_start = (ms < ls) ? ms : ls;

        /* Extract full tag */
        const char *tag_end = strchr(tag_start, '>');
        if (!tag_end) break;

        size_t tag_len = (size_t)(tag_end - tag_start) + 1;
        char *tag = malloc(tag_len + 1);
        if (!tag) break;
        memcpy(tag, tag_start, tag_len);
        tag[tag_len] = '\0';

        char prop[128]  = {0};
        char name[128]  = {0};
        char cont[MAX_FIELD_BYTES] = {0};
        char rel[64]    = {0};
        char href[MAX_FIELD_BYTES] = {0};

        attr_val(tag, "property", prop, sizeof(prop));
        attr_val(tag, "name",     name, sizeof(name));
        attr_val(tag, "content",  cont, sizeof(cont));
        attr_val(tag, "rel",      rel,  sizeof(rel));
        attr_val(tag, "href",     href, sizeof(href));

        /* og:title always wins over <title>; subsequent og:title tags are ignored */
        if (strcasecmp(prop, "og:title") == 0)
            snprintf(m->title,       sizeof(m->title),       "%s", cont);
        if (strcasecmp(prop, "og:description") == 0 && !m->description[0])
            snprintf(m->description, sizeof(m->description), "%s", cont);
        if (strcasecmp(prop, "og:image") == 0 && !m->image[0])
            snprintf(m->image,       sizeof(m->image),       "%s", cont);
        if (strcasecmp(prop, "og:site_name") == 0 && !m->site_name[0])
            snprintf(m->site_name,   sizeof(m->site_name),   "%s", cont);
        if (strcasecmp(prop, "og:type") == 0 && !m->og_type[0])
            snprintf(m->og_type,     sizeof(m->og_type),     "%s", cont);
        if (strcasecmp(prop, "og:url") == 0 && !m->canonical[0])
            snprintf(m->canonical,   sizeof(m->canonical),   "%s", cont);

        if (strcasecmp(name, "description") == 0 && !m->description[0])
            snprintf(m->description, sizeof(m->description), "%s", cont);
        if (strcasecmp(name, "twitter:title") == 0 && !m->title[0])
            snprintf(m->title,       sizeof(m->title),       "%s", cont);
        if (strcasecmp(name, "twitter:description") == 0 && !m->description[0])
            snprintf(m->description, sizeof(m->description), "%s", cont);
        if (strcasecmp(name, "twitter:image") == 0 && !m->image[0])
            snprintf(m->image,       sizeof(m->image),       "%s", cont);

        if (strcasecmp(rel, "canonical") == 0 && href[0] && !m->canonical[0])
            snprintf(m->canonical,   sizeof(m->canonical),   "%s", href);

        free(tag);
        p = tag_end + 1;
    }

    /* Decode entities in extracted fields */
    decode_entities(m->title);
    decode_entities(m->description);

    /* Fallback canonical = final_url */
    if (!m->canonical[0] && final_url)
        snprintf(m->canonical, sizeof(m->canonical), "%s", final_url);
}

/* ---- fetch --------------------------------------------------------------- */

static int fetch_and_parse(const char *url, const char *input_url)
{
    int timeout = DEFAULT_TIMEOUT_S;
    size_t max_bytes = DEFAULT_MAX_BYTES;

    const char *env_to = getenv("CLAW_LINK_TIMEOUT");
    if (env_to) timeout = atoi(env_to);
    const char *env_mb = getenv("CLAW_LINK_MAX_BYTES");
    if (env_mb) max_bytes = (size_t)atol(env_mb);

    const char *ua = getenv("CLAW_LINK_UA");
    if (!ua) ua = DEFAULT_UA;

    CURL *curl = curl_easy_init();
    if (!curl) {
        puts("{\"ok\":false,\"error\":\"Failed to init curl\"}");
        return 1;
    }

    Buf body = {0};
    body.limit = max_bytes;

    curl_easy_setopt(curl, CURLOPT_URL,            url);
    curl_easy_setopt(curl, CURLOPT_USERAGENT,      ua);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS,      5L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,        (long)timeout);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,  buf_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,      &body);
    /* Prefer HTML */
    struct curl_slist *hdrs = NULL;
    hdrs = curl_slist_append(hdrs, "Accept: text/html,application/xhtml+xml;q=0.9,*/*;q=0.8");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);

    CURLcode res = curl_easy_perform(curl);

    char  *final_url = NULL;
    long   http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE,   &http_code);
    curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL,   &final_url);

    if (res != CURLE_OK) {
        char esc[256] = {0};
        json_escape(curl_easy_strerror(res), esc, sizeof(esc));
        printf("{\"ok\":false,\"error\":\"%s\",\"url\":\"%s\"}\n", esc, url);
        curl_slist_free_all(hdrs);
        curl_easy_cleanup(curl);
        free(body.data);
        return 1;
    }

    if (http_code < 200 || http_code >= 300) {
        printf("{\"ok\":false,\"error\":\"HTTP %ld\",\"url\":\"%s\"}\n", http_code, url);
        curl_slist_free_all(hdrs);
        curl_easy_cleanup(curl);
        free(body.data);
        return 1;
    }

    Meta m;
    parse_meta(body.data ? body.data : "", final_url, &m);

    char esc_url[MAX_URL_BYTES*2]    = {0};
    char esc_canonical[MAX_URL_BYTES*2] = {0};
    char esc_tit[MAX_FIELD_BYTES*2]  = {0};
    char esc_des[MAX_FIELD_BYTES*2]  = {0};
    char esc_img[MAX_FIELD_BYTES*2]  = {0};
    char esc_sn[MAX_FIELD_BYTES*2]   = {0};
    char esc_typ[256]                = {0};

    json_escape(input_url,    esc_url,       sizeof(esc_url));
    json_escape(m.canonical,  esc_canonical, sizeof(esc_canonical));
    json_escape(m.title,      esc_tit,       sizeof(esc_tit));
    json_escape(m.description,esc_des,       sizeof(esc_des));
    json_escape(m.image,      esc_img,       sizeof(esc_img));
    json_escape(m.site_name,  esc_sn,        sizeof(esc_sn));
    json_escape(m.og_type,    esc_typ,       sizeof(esc_typ));

    printf("{\"ok\":true"
           ",\"url\":\"%s\""
           ",\"canonical\":\"%s\""
           ",\"title\":\"%s\""
           ",\"description\":\"%s\""
           ",\"image\":\"%s\""
           ",\"site_name\":\"%s\""
           ",\"type\":\"%s\""
           "}\n",
           esc_url, esc_canonical, esc_tit, esc_des, esc_img, esc_sn, esc_typ);

    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);
    free(body.data);
    return 0;
}

/* ---- main ---------------------------------------------------------------- */

int main(int argc, char *argv[])
{
    char url[MAX_URL_BYTES] = {0};

    if (argc >= 2 && strcmp(argv[1], "-h") == 0) {
        fprintf(stderr,
            "Usage: claw-link <URL>\n"
            "       echo '{\"url\":\"https://...\"}' | claw-link\n"
            "\n"
            "Fetch a URL and extract link preview metadata (title, description, image).\n");
        return 0;
    }

    if (argc >= 2) {
        /* CLI mode: URL as argument */
        snprintf(url, sizeof(url), "%s", argv[1]);
    } else {
        /* JSON skill mode: read from stdin */
        char input[MAX_INPUT_BYTES] = {0};
        if (!fgets(input, sizeof(input), stdin)) {
            puts("{\"ok\":false,\"error\":\"No input\"}");
            return 1;
        }
        if (!json_get_string(input, "url", url, sizeof(url)) || url[0] == '\0') {
            puts("{\"ok\":false,\"error\":\"Missing 'url' field\"}");
            return 1;
        }
    }

    if (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0) {
        char esc[MAX_URL_BYTES * 2] = {0};
        json_escape(url, esc, sizeof(esc));
        printf("{\"ok\":false,\"error\":\"URL must start with http:// or https://\",\"url\":\"%s\"}\n", esc);
        return 1;
    }

    curl_global_init(CURL_GLOBAL_DEFAULT);
    int rc = fetch_and_parse(url, url);
    curl_global_cleanup();
    return rc;
}
