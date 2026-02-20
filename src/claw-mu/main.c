/*
 * claw-mu — Media / image understanding for claw-linux.
 *
 * Sends an image or media file to a vision-capable LLM (Ollama or OpenAI)
 * and returns a natural-language description or answer to a question about
 * the content.  Base64-encodes the image and submits it to the vision API.
 *
 * Corresponds to: openclaw/openclaw src/media-understanding/
 *
 * Protocol (stdin → stdout, JSON skill)
 * --------------------------------------
 *   Input:
 *     {
 *       "image":   "/path/to/image.jpg",  ← local file path (required)
 *       "prompt":  "Describe this image.", ← question/prompt (default below)
 *       "provider":"ollama",               ← "ollama" (default) or "openai"
 *       "model":   "llava"                 ← model name
 *     }
 *
 *   Output:
 *     {"ok":true,"description":"A scenic mountain landscape…","model":"llava"}
 *
 * CLI mode
 * --------
 *   claw-mu /path/to/image.jpg
 *   claw-mu -p "What colour is the car?" /path/to/image.jpg
 *   claw-mu -m gpt-4o --provider openai /path/to/image.jpg
 *
 * Configuration (environment variables)
 * --------------------------------------
 *   OPENCLAW_MODEL_PROVIDER   "ollama" (default) or "openai"
 *   OPENCLAW_OLLAMA_HOST      Ollama API host (default: http://localhost:11434)
 *   OPENCLAW_MODEL_NAME       Model name (default: llava for Ollama, gpt-4o for OpenAI)
 *   OPENCLAW_OPENAI_API_KEY   OpenAI API key (required for provider=openai)
 *
 * Build (requires libcurl-dev):
 *   cc -O2 -Wall -Wextra -o claw-mu main.c -lcurl
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sys/stat.h>
#include <curl/curl.h>

#include "../common/claw_json.h"

/* ---- tunables ------------------------------------------------------------ */
#define MAX_INPUT_BYTES      4096
#define MAX_PATH_BYTES        512
#define MAX_PROMPT_BYTES     1024
#define MAX_IMAGE_BYTES   (20 * 1024 * 1024)  /* 20 MiB */
#define MAX_RESPONSE_BYTES (4 * 1024 * 1024)  /* 4 MiB */
#define DEFAULT_PROMPT  "Describe this image in detail."
#define DEFAULT_OLLAMA_HOST "http://localhost:11434"
#define DEFAULT_OLLAMA_MODEL "llava"
#define DEFAULT_OPENAI_MODEL "gpt-4o"
#define REQUEST_TIMEOUT_S 120

/* ---- base64 -------------------------------------------------------------- */

static const char B64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static char *base64_encode(const unsigned char *data, size_t len, size_t *out_len)
{
    size_t encoded_len = ((len + 2) / 3) * 4 + 1;
    char  *out         = malloc(encoded_len);
    if (!out) return NULL;

    size_t i = 0, j = 0;
    for (i = 0; i + 2 < len; i += 3) {
        out[j++] = B64[ data[i]          >> 2];
        out[j++] = B64[(data[i]   & 0x03) << 4 | data[i+1] >> 4];
        out[j++] = B64[(data[i+1] & 0x0F) << 2 | data[i+2] >> 6];
        out[j++] = B64[ data[i+2] & 0x3F];
    }
    if (i < len) {
        out[j++] = B64[data[i] >> 2];
        if (i + 1 < len) {
            out[j++] = B64[(data[i] & 0x03) << 4 | data[i+1] >> 4];
            out[j++] = B64[(data[i+1] & 0x0F) << 2];
        } else {
            out[j++] = B64[(data[i] & 0x03) << 4];
            out[j++] = '=';
        }
        out[j++] = '=';
    }
    out[j] = '\0';
    if (out_len) *out_len = j;
    return out;
}

/* ---- read file ----------------------------------------------------------- */

static unsigned char *read_image_file(const char *path, size_t *out_len)
{
    struct stat st;
    if (stat(path, &st) < 0) return NULL;
    if ((size_t)st.st_size > MAX_IMAGE_BYTES) return NULL;

    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;

    unsigned char *buf = malloc((size_t)st.st_size + 1);
    if (!buf) { fclose(fp); return NULL; }

    size_t nr = fread(buf, 1, (size_t)st.st_size, fp);
    fclose(fp);
    if (nr == 0) { free(buf); return NULL; }

    if (out_len) *out_len = nr;
    return buf;
}

/* ---- HTTP response buffer ------------------------------------------------ */

typedef struct {
    char   *data;
    size_t  used;
    size_t  cap;
} Buf;

static size_t buf_write(void *ptr, size_t size, size_t nmemb, void *ud)
{
    Buf   *b   = (Buf *)ud;
    size_t inc = size * nmemb;
    if (b->used + inc + 1 > b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 65536;
        while (nc < b->used + inc + 1) nc *= 2;
        if (nc > MAX_RESPONSE_BYTES + 1) nc = MAX_RESPONSE_BYTES + 1;
        char *p = realloc(b->data, nc);
        if (!p) return 0;
        b->data = p; b->cap = nc;
    }
    if (b->used + inc > MAX_RESPONSE_BYTES) inc = MAX_RESPONSE_BYTES - b->used;
    memcpy(b->data + b->used, ptr, inc);
    b->used += inc;
    b->data[b->used] = '\0';
    return size * nmemb;
}

/* ---- Ollama vision API --------------------------------------------------- */

static int query_ollama(const char *model, const char *prompt,
                        const char *b64_image, const char *host,
                        char *out, size_t out_size)
{
    char url[512];
    snprintf(url, sizeof(url), "%s/api/generate", host);

    /* Build JSON body */
    size_t body_cap = strlen(b64_image) + strlen(prompt) + 256;
    char *body = malloc(body_cap);
    if (!body) return -1;

    char esc_prompt[MAX_PROMPT_BYTES * 2] = {0};
    json_escape(prompt, esc_prompt, sizeof(esc_prompt));

    snprintf(body, body_cap,
        "{\"model\":\"%s\","
        "\"prompt\":\"%s\","
        "\"images\":[\"%s\"],"
        "\"stream\":false}",
        model, esc_prompt, b64_image);

    CURL *curl = curl_easy_init();
    if (!curl) { free(body); return -1; }

    Buf resp = {0};
    struct curl_slist *hdrs = NULL;
    hdrs = curl_slist_append(hdrs, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL,           url);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS,    body);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER,    hdrs);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,       REQUEST_TIMEOUT_S);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, buf_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,     &resp);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);

    CURLcode rc = curl_easy_perform(curl);
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);
    free(body);

    if (rc != CURLE_OK) {
        free(resp.data);
        snprintf(out, out_size, "{\"ok\":false,\"error\":\"%s\"}", curl_easy_strerror(rc));
        return 1;
    }

    /* Extract "response" field from Ollama reply */
    char description[MAX_RESPONSE_BYTES / 2] = {0};
    if (resp.data)
        json_get_string(resp.data, "response", description, sizeof(description));
    free(resp.data);

    if (!description[0]) {
        snprintf(out, out_size, "{\"ok\":false,\"error\":\"Empty response from Ollama\"}");
        return 1;
    }

    char esc_desc[MAX_RESPONSE_BYTES] = {0};
    char esc_model[64] = {0};
    json_escape(description, esc_desc,  sizeof(esc_desc));
    json_escape(model,       esc_model, sizeof(esc_model));
    snprintf(out, out_size,
        "{\"ok\":true,\"description\":\"%s\",\"model\":\"%s\"}",
        esc_desc, esc_model);
    return 0;
}

/* ---- OpenAI vision API --------------------------------------------------- */

static int query_openai(const char *model, const char *prompt,
                        const char *b64_image, const char *api_key,
                        char *out, size_t out_size)
{
    /* Detect image type from first bytes — default to jpeg */
    const char *mime = "image/jpeg";
    /* (b64_image encoding doesn't expose raw bytes, so we default) */

    size_t body_cap = strlen(b64_image) + strlen(prompt) + 1024;
    char *body = malloc(body_cap);
    if (!body) return -1;

    char esc_prompt[MAX_PROMPT_BYTES * 2] = {0};
    json_escape(prompt, esc_prompt, sizeof(esc_prompt));

    snprintf(body, body_cap,
        "{\"model\":\"%s\","
        "\"messages\":[{\"role\":\"user\",\"content\":["
          "{\"type\":\"text\",\"text\":\"%s\"},"
          "{\"type\":\"image_url\",\"image_url\":"
            "{\"url\":\"data:%s;base64,%s\"}}"
        "]}],"
        "\"max_tokens\":1024}",
        model, esc_prompt, mime, b64_image);

    char auth_hdr[512];
    snprintf(auth_hdr, sizeof(auth_hdr), "Authorization: Bearer %s", api_key);

    CURL *curl = curl_easy_init();
    if (!curl) { free(body); return -1; }

    Buf resp = {0};
    struct curl_slist *hdrs = NULL;
    hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
    hdrs = curl_slist_append(hdrs, auth_hdr);

    curl_easy_setopt(curl, CURLOPT_URL,           "https://api.openai.com/v1/chat/completions");
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS,    body);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER,    hdrs);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,       REQUEST_TIMEOUT_S);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, buf_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,     &resp);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);

    CURLcode rc = curl_easy_perform(curl);
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);
    free(body);

    if (rc != CURLE_OK) {
        free(resp.data);
        snprintf(out, out_size, "{\"ok\":false,\"error\":\"%s\"}", curl_easy_strerror(rc));
        return 1;
    }

    /* Extract choices[0].message.content */
    char description[MAX_RESPONSE_BYTES / 2] = {0};
    if (resp.data) {
        /* Navigate: {"choices":[{"message":{"content":"..."}}]} */
        const char *cp = strstr(resp.data, "\"content\":");
        if (cp) {
            cp += 10;
            while (*cp == ' ') cp++;
            if (*cp == '"') {
                cp++;
                size_t di = 0;
                while (*cp && di < sizeof(description) - 1) {
                    if (*cp == '\\' && *(cp+1)) {
                        cp++;
                        switch (*cp) {
                            case 'n': description[di++] = '\n'; break;
                            case 't': description[di++] = '\t'; break;
                            case '"': description[di++] = '"';  break;
                            default:  description[di++] = *cp;  break;
                        }
                    } else if (*cp == '"') {
                        break;
                    } else {
                        description[di++] = *cp;
                    }
                    cp++;
                }
                description[di] = '\0';
            }
        }
    }
    free(resp.data);

    if (!description[0]) {
        snprintf(out, out_size, "{\"ok\":false,\"error\":\"Empty or unexpected response from OpenAI\"}");
        return 1;
    }

    char esc_desc[MAX_RESPONSE_BYTES] = {0};
    char esc_model[64] = {0};
    json_escape(description, esc_desc,  sizeof(esc_desc));
    json_escape(model,       esc_model, sizeof(esc_model));
    snprintf(out, out_size,
        "{\"ok\":true,\"description\":\"%s\",\"model\":\"%s\"}",
        esc_desc, esc_model);
    return 0;
}

/* ---- core logic ---------------------------------------------------------- */

static int understand(const char *image_path, const char *prompt,
                      const char *provider, const char *model_arg,
                      char *out, size_t out_size)
{
    /* Read and base64-encode the image */
    size_t img_len = 0;
    unsigned char *img_data = read_image_file(image_path, &img_len);
    if (!img_data) {
        snprintf(out, out_size,
            "{\"ok\":false,\"error\":\"Cannot read image file: %s\"}",
            strerror(errno));
        return 1;
    }

    size_t b64_len = 0;
    char *b64 = base64_encode(img_data, img_len, &b64_len);
    free(img_data);
    if (!b64) {
        snprintf(out, out_size, "{\"ok\":false,\"error\":\"Base64 encoding failed\"}");
        return 1;
    }

    int rc;
    if (strcmp(provider, "openai") == 0) {
        const char *api_key = getenv("OPENCLAW_OPENAI_API_KEY");
        if (!api_key || !api_key[0]) {
            snprintf(out, out_size,
                "{\"ok\":false,\"error\":\"OPENCLAW_OPENAI_API_KEY not set\"}");
            free(b64); return 1;
        }
        const char *model = model_arg && model_arg[0] ? model_arg : DEFAULT_OPENAI_MODEL;
        rc = query_openai(model, prompt, b64, api_key, out, out_size);
    } else {
        /* Default: Ollama */
        const char *host = getenv("OPENCLAW_OLLAMA_HOST");
        if (!host || !host[0]) host = DEFAULT_OLLAMA_HOST;
        const char *model = model_arg && model_arg[0] ? model_arg : DEFAULT_OLLAMA_MODEL;
        const char *env_model = getenv("OPENCLAW_MODEL_NAME");
        if (env_model && env_model[0] && !model_arg) model = env_model;
        rc = query_ollama(model, prompt, b64, host, out, out_size);
    }

    free(b64);
    return rc;
}

/* ---- usage --------------------------------------------------------------- */

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [OPTIONS] <image-file>\n"
        "       echo '{\"image\":\"/path.jpg\",\"prompt\":\"…\"}' | %s\n"
        "\n"
        "Options:\n"
        "  -p PROMPT    Question or description prompt\n"
        "  -m MODEL     Model name (default: llava / gpt-4o)\n"
        "  --provider P Provider: ollama (default) or openai\n"
        "  -h           Show this help\n"
        "\n"
        "Environment:\n"
        "  OPENCLAW_MODEL_PROVIDER   ollama | openai\n"
        "  OPENCLAW_OLLAMA_HOST      Ollama host (default: http://localhost:11434)\n"
        "  OPENCLAW_MODEL_NAME       Model name override\n"
        "  OPENCLAW_OPENAI_API_KEY   OpenAI API key\n",
        prog, prog);
}

/* ---- main ---------------------------------------------------------------- */

int main(int argc, char *argv[])
{
    signal(SIGPIPE, SIG_IGN);

    /* JSON skill mode */
    if (argc == 1 && !isatty(STDIN_FILENO)) {
        char input[MAX_INPUT_BYTES] = {0};
        if (!fgets(input, sizeof(input), stdin)) {
            puts("{\"ok\":false,\"error\":\"No input\"}");
            return 1;
        }
        char image_path[MAX_PATH_BYTES]  = {0};
        char prompt[MAX_PROMPT_BYTES]    = DEFAULT_PROMPT;
        char provider[32]                = {0};
        char model[64]                   = {0};

        if (!json_get_string(input, "image", image_path, sizeof(image_path)) ||
                image_path[0] == '\0') {
            puts("{\"ok\":false,\"error\":\"Missing 'image' field\"}");
            return 1;
        }
        json_get_string(input, "prompt",   prompt,   sizeof(prompt));
        json_get_string(input, "provider", provider, sizeof(provider));
        json_get_string(input, "model",    model,    sizeof(model));

        if (!provider[0]) {
            const char *env = getenv("OPENCLAW_MODEL_PROVIDER");
            snprintf(provider, sizeof(provider), "%s", env ? env : "ollama");
        }

        char out[MAX_RESPONSE_BYTES] = {0};
        curl_global_init(CURL_GLOBAL_DEFAULT);
        int rc = understand(image_path, prompt, provider, model, out, sizeof(out));
        curl_global_cleanup();
        puts(out);
        return rc;
    }

    /* CLI mode */
    char prompt[MAX_PROMPT_BYTES]   = DEFAULT_PROMPT;
    char model[64]                  = {0};
    char provider[32]               = {0};
    const char *image_path          = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-p") == 0 && i+1 < argc) {
            snprintf(prompt, sizeof(prompt), "%s", argv[++i]);
        } else if (strcmp(argv[i], "-m") == 0 && i+1 < argc) {
            snprintf(model, sizeof(model), "%s", argv[++i]);
        } else if (strcmp(argv[i], "--provider") == 0 && i+1 < argc) {
            snprintf(provider, sizeof(provider), "%s", argv[++i]);
        } else if (strcmp(argv[i], "-h") == 0) {
            usage(argv[0]); return 0;
        } else if (argv[i][0] != '-') {
            image_path = argv[i];
        }
    }

    if (!image_path) {
        fprintf(stderr, "claw-mu: no image file provided\n");
        usage(argv[0]);
        return 1;
    }

    if (!provider[0]) {
        const char *env = getenv("OPENCLAW_MODEL_PROVIDER");
        snprintf(provider, sizeof(provider), "%s", env ? env : "ollama");
    }

    char out[MAX_RESPONSE_BYTES] = {0};
    curl_global_init(CURL_GLOBAL_DEFAULT);
    int rc = understand(image_path, prompt, provider, model, out, sizeof(out));
    curl_global_cleanup();
    puts(out);
    return rc;
}
