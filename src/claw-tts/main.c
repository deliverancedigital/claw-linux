/*
 * claw-tts — Text-to-speech skill for claw-linux.
 *
 * Wraps espeak-ng (available via `apk add espeak-ng`) to synthesise speech
 * from text.  On XFCE desktop deployments audio is delivered via ALSA/PulseAudio;
 * on headless deployments the WAV output can be written to a file.
 *
 * Corresponds to: openclaw/openclaw src/tts/ (text-to-speech)
 *
 * Protocol (stdin → stdout, JSON skill mode)
 * ------------------------------------------
 *   Input:
 *     {"text":"Hello world","voice":"en","speed":175,"pitch":50,"output":"speak"}
 *
 *   Fields:
 *     text    — the text to synthesise (required)
 *     voice   — espeak-ng voice name, e.g. "en", "en-us", "fr" (default: "en")
 *     speed   — words per minute, 80–450 (default: 175)
 *     pitch   — pitch 0–99 (default: 50)
 *     output  — "speak" (play audio, default) or "file" (write to WAV_FILE)
 *     file    — output WAV file path (used when output=file)
 *
 *   Output (speak):
 *     {"ok":true,"voice":"en","words":3}
 *
 *   Output (file):
 *     {"ok":true,"file":"/tmp/claw-tts.wav"}
 *
 * CLI mode
 * --------
 *   claw-tts "Hello world"
 *   claw-tts -v en-us -s 160 "Hello world"
 *   claw-tts -f /tmp/out.wav "Hello world"
 *
 * Options
 * -------
 *   -v VOICE   Voice name (default: en)
 *   -s SPEED   Words per minute (default: 175)
 *   -p PITCH   Pitch 0–99 (default: 50)
 *   -f FILE    Write WAV to FILE instead of playing
 *   -h         Show this help
 *
 * Dependencies
 * ------------
 *   apk add espeak-ng
 *
 * Build:
 *   cc -O2 -Wall -Wextra -o claw-tts main.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <ctype.h>

#include "../common/claw_json.h"

/* ---- tunables ------------------------------------------------------------ */
#define DEFAULT_VOICE   "en"
#define DEFAULT_SPEED   175
#define DEFAULT_PITCH    50
#define MAX_TEXT_BYTES  65536
#define MAX_INPUT_BYTES 65792
#define ESPEAK_BIN      "espeak-ng"

/* ---- helpers ------------------------------------------------------------- */

/* Count words in a string (whitespace-delimited). */
static int count_words(const char *s)
{
    int n = 0, in_word = 0;
    while (*s) {
        if (isspace((unsigned char)*s)) { in_word = 0; }
        else if (!in_word)              { in_word = 1; n++; }
        s++;
    }
    return n;
}

/*
 * Run espeak-ng with the given parameters.
 * If out_file is NULL, plays audio directly.
 * Returns exit code (0 = success).
 */
static int run_espeak(const char *text, const char *voice, int speed, int pitch,
                      const char *out_file)
{
    char speed_str[16], pitch_str[16];
    snprintf(speed_str, sizeof(speed_str), "%d", speed);
    snprintf(pitch_str, sizeof(pitch_str), "%d", pitch);

    /* Build argv */
    const char *argv[16];
    int i = 0;
    argv[i++] = ESPEAK_BIN;
    argv[i++] = "-v"; argv[i++] = voice;
    argv[i++] = "-s"; argv[i++] = speed_str;
    argv[i++] = "-p"; argv[i++] = pitch_str;
    if (out_file) {
        argv[i++] = "-w"; argv[i++] = out_file;
    }
    argv[i++] = text;
    argv[i]   = NULL;

    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        /* Silence stderr from espeak-ng in skill mode */
        if (isatty(STDOUT_FILENO) == 0) {
            int dev_null = open("/dev/null", O_WRONLY);
            if (dev_null >= 0) {
                dup2(dev_null, STDERR_FILENO);
                close(dev_null);
            }
        }
        execvp(ESPEAK_BIN, (char *const *)argv);
        /* If espeak-ng not found, fall back to espeak */
        execvp("espeak", (char *const *)argv);
        _exit(127);
    }

    int status = 0;
    for (;;) {
        pid_t w = waitpid(pid, &status, 0);
        if (w < 0) {
            if (errno == EINTR) continue; /* retry on signal interrupt */
            return -1;                    /* waitpid error */
        }
        break;
    }
    if (WIFEXITED(status))   return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return 1;
}

/* ---- skill mode ---------------------------------------------------------- */

static int skill_mode(void)
{
    char input[MAX_INPUT_BYTES] = {0};
    if (!fgets(input, sizeof(input), stdin)) {
        puts("{\"ok\":false,\"error\":\"No input\"}");
        return 1;
    }

    char text[MAX_TEXT_BYTES]  = {0};
    char voice[64]  = DEFAULT_VOICE;
    char out_file[512] = {0};
    char output_mode[32] = "speak";

    if (!json_get_string(input, "text", text, sizeof(text)) || text[0] == '\0') {
        puts("{\"ok\":false,\"error\":\"Missing 'text' field\"}");
        return 1;
    }

    json_get_string(input, "voice",  voice,       sizeof(voice));
    json_get_string(input, "output", output_mode, sizeof(output_mode));
    json_get_string(input, "file",   out_file,    sizeof(out_file));

    long speed = json_get_long(input, "speed", DEFAULT_SPEED);
    long pitch = json_get_long(input, "pitch", DEFAULT_PITCH);

    if (speed < 80)  speed = 80;
    if (speed > 450) speed = 450;
    if (pitch < 0)   pitch = 0;
    if (pitch > 99)  pitch = 99;

    const char *wav = NULL;
    if (strcmp(output_mode, "file") == 0) {
        if (!out_file[0]) snprintf(out_file, sizeof(out_file), "/tmp/claw-tts.wav");
        wav = out_file;
    }

    int rc = run_espeak(text, voice, (int)speed, (int)pitch, wav);

    if (rc == 127) {
        puts("{\"ok\":false,\"error\":\"espeak-ng not found — install with: apk add espeak-ng\"}");
        return 1;
    }
    if (rc != 0) {
        printf("{\"ok\":false,\"error\":\"espeak-ng exited with code %d\"}\n", rc);
        return 1;
    }

    int words = count_words(text);

    if (wav) {
        char esc_file[1024] = {0};
        json_escape(out_file, esc_file, sizeof(esc_file));
        printf("{\"ok\":true,\"voice\":\"%s\",\"words\":%d,\"file\":\"%s\"}\n",
               voice, words, esc_file);
    } else {
        printf("{\"ok\":true,\"voice\":\"%s\",\"words\":%d}\n", voice, words);
    }
    return 0;
}

/* ---- usage --------------------------------------------------------------- */

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [OPTIONS] \"text to speak\"\n"
        "       echo '{\"text\":\"hello\"}' | %s\n"
        "\n"
        "Options:\n"
        "  -v VOICE   Voice name (default: %s)\n"
        "  -s SPEED   Words per minute, 80–450 (default: %d)\n"
        "  -p PITCH   Pitch 0–99 (default: %d)\n"
        "  -f FILE    Write WAV to FILE instead of playing audio\n"
        "  -h         Show this help\n"
        "\n"
        "Requires espeak-ng:  apk add espeak-ng\n",
        prog, prog, DEFAULT_VOICE, DEFAULT_SPEED, DEFAULT_PITCH);
}

/* ---- main ---------------------------------------------------------------- */

int main(int argc, char *argv[])
{
    /* If stdin is not a tty and no args given, run as JSON skill */
    if (argc == 1 && !isatty(STDIN_FILENO))
        return skill_mode();

    char voice[64]  = DEFAULT_VOICE;
    int  speed      = DEFAULT_SPEED;
    int  pitch      = DEFAULT_PITCH;
    char out_file[512] = {0};

    int opt;
    while ((opt = getopt(argc, argv, "v:s:p:f:h")) != -1) {
        switch (opt) {
            case 'v': snprintf(voice, sizeof(voice), "%s", optarg); break;
            case 's': speed = atoi(optarg); break;
            case 'p': pitch = atoi(optarg); break;
            case 'f': snprintf(out_file, sizeof(out_file), "%s", optarg); break;
            case 'h': usage(argv[0]); return 0;
            default:  usage(argv[0]); return 1;
        }
    }

    if (optind >= argc) {
        fprintf(stderr, "claw-tts: no text provided\n");
        usage(argv[0]);
        return 1;
    }

    /* Concatenate remaining args as the text */
    char text[MAX_TEXT_BYTES] = {0};
    size_t pos = 0;
    for (int i = optind; i < argc; i++) {
        if (pos > 0 && pos < sizeof(text) - 1) text[pos++] = ' ';
        size_t alen = strlen(argv[i]);
        if (pos + alen >= sizeof(text)) alen = sizeof(text) - pos - 1;
        memcpy(text + pos, argv[i], alen);
        pos += alen;
    }
    text[pos] = '\0';

    if (speed < 80)  speed = 80;
    if (speed > 450) speed = 450;
    if (pitch < 0)   pitch = 0;
    if (pitch > 99)  pitch = 99;

    const char *wav = out_file[0] ? out_file : NULL;
    int rc = run_espeak(text, voice, speed, pitch, wav);

    if (rc == 127) {
        fprintf(stderr, "claw-tts: espeak-ng not found — install with: apk add espeak-ng\n");
        return 1;
    }
    return rc;
}
