/*
 * claw-media — Media playback and information skill for claw-linux.
 *
 * Plays audio and video files via mpv (or mplayer as a fallback), and
 * retrieves media metadata via ffprobe.  On XFCE desktop deployments mpv
 * opens a graphical window; on headless deployments it plays audio only or
 * runs in no-video mode.
 *
 * Corresponds to: openclaw/openclaw src/media/ (media handling)
 *
 * Usage (CLI)
 * -----------
 *   claw-media play  <file|URL>   — play a media file or stream
 *   claw-media info  <file|URL>   — print media metadata as JSON
 *   claw-media stop               — stop the current playback (sends SIGTERM)
 *   claw-media -h                 — show this help
 *
 * Usage (JSON skill, stdin → stdout)
 * -----------------------------------
 *   Play:
 *     Input:  {"op":"play","file":"/path/to/video.mp4","audio_only":false}
 *     Output: {"ok":true,"op":"play","file":"...","pid":12345}
 *
 *   Info:
 *     Input:  {"op":"info","file":"/path/to/video.mp4"}
 *     Output: {"ok":true,"op":"info","file":"...","duration":120.5,
 *              "width":1920,"height":1080,"codec":"h264","audio_codec":"aac",
 *              "size":4194304}
 *
 *   Stop:
 *     Input:  {"op":"stop"}
 *     Output: {"ok":true,"op":"stop"}
 *
 * Dependencies
 * ------------
 *   Playback:  apk add mpv          (preferred) or apk add mplayer
 *   Metadata:  apk add ffmpeg       (provides ffprobe)
 *
 * Build:
 *   cc -O2 -Wall -Wextra -o claw-media main.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "../common/claw_json.h"

/* ---- tunables ------------------------------------------------------------ */
#define MAX_INPUT_BYTES   4096
#define MAX_PATH_BYTES    2048
#define PID_FILE          "/var/run/claw/media.pid"
#define INFO_BUF_SIZE     65536

/* ---- helpers ------------------------------------------------------------- */

static int cmd_exists(const char *cmd)
{
    const char *dirs[] = {
        "/usr/bin", "/usr/local/bin", "/bin", NULL
    };
    char path[512];
    for (int i = 0; dirs[i]; i++) {
        snprintf(path, sizeof(path), "%s/%s", dirs[i], cmd);
        if (access(path, X_OK) == 0) return 1;
    }
    return 0;
}

static int has_display(void)
{
    return (getenv("DISPLAY") != NULL || getenv("WAYLAND_DISPLAY") != NULL);
}

/* ---- PID file ------------------------------------------------------------ */

static void write_pid(pid_t pid)
{
    mkdir("/var/run/claw", 0755);
    FILE *f = fopen(PID_FILE, "w");
    if (f) { fprintf(f, "%d\n", (int)pid); fclose(f); }
}

static pid_t read_pid(void)
{
    FILE *f = fopen(PID_FILE, "r");
    if (!f) return -1;
    int pid = -1;
    int r = fscanf(f, "%d", &pid);
    (void)r;
    fclose(f);
    return (pid_t)pid;
}

/* ---- play ---------------------------------------------------------------- */

static int do_play(const char *file, int audio_only)
{
    const char *player = NULL;
    if (cmd_exists("mpv"))     player = "mpv";
    else if (cmd_exists("mplayer")) player = "mplayer";

    if (!player) {
        fprintf(stderr, "claw-media: no player found — install mpv: apk add mpv\n");
        return 1;
    }

    /* Build argv */
    const char *argv[16];
    int i = 0;
    argv[i++] = player;

    if (strcmp(player, "mpv") == 0) {
        if (audio_only || !has_display()) {
            argv[i++] = "--no-video";
        }
        argv[i++] = "--really-quiet";
    } else {
        /* mplayer */
        if (audio_only || !has_display()) {
            argv[i++] = "-novideo";
        }
        argv[i++] = "-really-quiet";
    }

    argv[i++] = file;
    argv[i]   = NULL;

    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        execvp(player, (char *const *)argv);
        _exit(127);
    }

    write_pid(pid);
    return (int)pid;
}

/* ---- info (via ffprobe) -------------------------------------------------- */

static int do_info(const char *file, char *out, size_t out_size)
{
    if (!cmd_exists("ffprobe")) {
        snprintf(out, out_size,
            "{\"ok\":false,\"error\":\"ffprobe not found — install with: apk add ffmpeg\"}");
        return 1;
    }

    /* Run ffprobe in JSON format */
    const char *argv[] = {
        "ffprobe",
        "-v", "quiet",
        "-print_format", "json",
        "-show_format",
        "-show_streams",
        file,
        NULL
    };

    int pipefd[2];
    if (pipe(pipefd) < 0) return -1;

    pid_t pid = fork();
    if (pid < 0) { close(pipefd[0]); close(pipefd[1]); return -1; }

    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        int dev_null = open("/dev/null", O_WRONLY);
        if (dev_null >= 0) { dup2(dev_null, STDERR_FILENO); close(dev_null); }
        close(pipefd[1]);
        execvp("ffprobe", (char *const *)argv);
        _exit(127);
    }

    close(pipefd[1]);

    /* Read ffprobe output */
    char ffbuf[INFO_BUF_SIZE] = {0};
    size_t total = 0;
    ssize_t nr;
    while ((nr = read(pipefd[0], ffbuf + total, sizeof(ffbuf) - 1 - total)) > 0)
        total += (size_t)nr;
    close(pipefd[0]);
    ffbuf[total] = '\0';

    int status = 0;
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0 || total == 0) {
        char esc_file[MAX_PATH_BYTES * 2] = {0};
        json_escape(file, esc_file, sizeof(esc_file));
        snprintf(out, out_size,
            "{\"ok\":false,\"error\":\"ffprobe failed\",\"file\":\"%s\"}", esc_file);
        return 1;
    }

    /* Extract key fields from ffprobe JSON */
    char duration_str[32] = "0";
    char width_str[16]    = "0";
    char height_str[16]   = "0";
    char codec[64]        = "";
    char audio_codec[64]  = "";
    char size_str[32]     = "0";

    /* Parse duration from format.duration */
    const char *dp = strstr(ffbuf, "\"duration\":");
    if (dp) {
        dp += 11;
        while (*dp == ' ' || *dp == '"') dp++;
        size_t dl = 0;
        while (*dp && *dp != '"' && *dp != ',' && *dp != '\n' && dl < sizeof(duration_str)-1)
            duration_str[dl++] = *dp++;
        duration_str[dl] = '\0';
    }

    /* Parse size from format.size */
    const char *sp = strstr(ffbuf, "\"size\":");
    if (sp) {
        sp += 7;
        while (*sp == ' ' || *sp == '"') sp++;
        size_t sl = 0;
        while (*sp && *sp != '"' && *sp != ',' && *sp != '\n' && sl < sizeof(size_str)-1)
            size_str[sl++] = *sp++;
        size_str[sl] = '\0';
    }

    /* Walk streams for video/audio codec and dimensions */
    const char *stream = ffbuf;
    while ((stream = strstr(stream, "\"codec_type\":")) != NULL) {
        stream += 13;
        char ctype[32] = {0};
        const char *q = stream;
        while (*q == ' ' || *q == '"') q++;
        size_t ci = 0;
        while (*q && *q != '"' && ci < sizeof(ctype)-1) ctype[ci++] = *q++;

        /* Find codec_name in the same stream object (search backwards to "{") */
        const char *block_start = stream - 13;
        while (block_start > ffbuf && *block_start != '{') block_start--;

        char cn_name[64] = {0};
        const char *cn = strstr(block_start, "\"codec_name\":");
        if (cn && cn < stream + 512) {
            cn += 13;
            while (*cn == ' ' || *cn == '"') cn++;
            size_t k = 0;
            while (*cn && *cn != '"' && k < sizeof(cn_name)-1) cn_name[k++] = *cn++;
        }

        if (strcmp(ctype, "video") == 0 && !codec[0]) {
            snprintf(codec, sizeof(codec), "%s", cn_name);
            /* width/height */
            const char *wp = strstr(block_start, "\"width\":");
            if (wp) {
                wp += 8;
                while (*wp == ' ') wp++;
                size_t wl = 0;
                while (*wp && *wp != ',' && *wp != '\n' && wl < sizeof(width_str)-1)
                    width_str[wl++] = *wp++;
                width_str[wl] = '\0';
            }
            const char *hp = strstr(block_start, "\"height\":");
            if (hp) {
                hp += 9;
                while (*hp == ' ') hp++;
                size_t hl = 0;
                while (*hp && *hp != ',' && *hp != '\n' && hl < sizeof(height_str)-1)
                    height_str[hl++] = *hp++;
                height_str[hl] = '\0';
            }
        } else if (strcmp(ctype, "audio") == 0 && !audio_codec[0]) {
            snprintf(audio_codec, sizeof(audio_codec), "%s", cn_name);
        }

        stream++;
    }

    char esc_file[MAX_PATH_BYTES * 2] = {0};
    char esc_codec[128]       = {0};
    char esc_acodec[128]      = {0};
    json_escape(file,        esc_file,   sizeof(esc_file));
    json_escape(codec,       esc_codec,  sizeof(esc_codec));
    json_escape(audio_codec, esc_acodec, sizeof(esc_acodec));

    snprintf(out, out_size,
        "{\"ok\":true,\"op\":\"info\""
        ",\"file\":\"%s\""
        ",\"duration\":%s"
        ",\"width\":%s"
        ",\"height\":%s"
        ",\"codec\":\"%s\""
        ",\"audio_codec\":\"%s\""
        ",\"size\":%s"
        "}",
        esc_file, duration_str, width_str, height_str,
        esc_codec, esc_acodec, size_str);

    return 0;
}

/* ---- stop ---------------------------------------------------------------- */

static int do_stop(void)
{
    pid_t pid = read_pid();
    if (pid <= 0) {
        fprintf(stderr, "claw-media: no media PID file found at %s\n", PID_FILE);
        return 1;
    }
    if (kill(pid, SIGTERM) < 0) {
        fprintf(stderr, "claw-media: kill(%d): %s\n", (int)pid, strerror(errno));
        return 1;
    }
    unlink(PID_FILE);
    return 0;
}

/* ---- usage --------------------------------------------------------------- */

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s <command> [args]\n"
        "       echo '{\"op\":\"info\",\"file\":\"video.mp4\"}' | %s\n"
        "\n"
        "Commands:\n"
        "  play  <file|URL>   Play a media file or network stream\n"
        "  info  <file|URL>   Print media metadata as JSON\n"
        "  stop               Stop the current playback\n"
        "\n"
        "Dependencies:\n"
        "  Playback: apk add mpv\n"
        "  Metadata: apk add ffmpeg\n",
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

        char op[32]            = {0};
        char file[MAX_PATH_BYTES] = {0};
        long audio_only = 0;

        if (!json_get_string(input, "op", op, sizeof(op))) {
            puts("{\"ok\":false,\"error\":\"Missing 'op' field (play|info|stop)\"}");
            return 1;
        }
        json_get_string(input, "file", file, sizeof(file));
        audio_only = json_get_long(input, "audio_only", 0);

        if (strcmp(op, "play") == 0) {
            if (!file[0]) { puts("{\"ok\":false,\"error\":\"Missing 'file' field\"}"); return 1; }
            int pid = do_play(file, (int)audio_only);
            if (pid < 0) { puts("{\"ok\":false,\"error\":\"Failed to launch player\"}"); return 1; }
            char esc_file[MAX_PATH_BYTES * 2] = {0};
            json_escape(file, esc_file, sizeof(esc_file));
            printf("{\"ok\":true,\"op\":\"play\",\"file\":\"%s\",\"pid\":%d}\n",
                   esc_file, pid);
        } else if (strcmp(op, "info") == 0) {
            if (!file[0]) { puts("{\"ok\":false,\"error\":\"Missing 'file' field\"}"); return 1; }
            char info_buf[INFO_BUF_SIZE] = {0};
            do_info(file, info_buf, sizeof(info_buf));
            puts(info_buf);
        } else if (strcmp(op, "stop") == 0) {
            int rc = do_stop();
            puts(rc == 0 ? "{\"ok\":true,\"op\":\"stop\"}"
                         : "{\"ok\":false,\"op\":\"stop\",\"error\":\"No active playback\"}");
            return rc;
        } else {
            char esc[64] = {0};
            json_escape(op, esc, sizeof(esc));
            printf("{\"ok\":false,\"error\":\"Unknown op '%s' — use play|info|stop\"}\n", esc);
            return 1;
        }
        return 0;
    }

    /* CLI mode */
    if (argc < 2) { usage(argv[0]); return 1; }

    const char *cmd = argv[1];

    if (strcmp(cmd, "-h") == 0 || strcmp(cmd, "--help") == 0) {
        usage(argv[0]); return 0;
    }

    if (strcmp(cmd, "play") == 0) {
        if (argc < 3) { fprintf(stderr, "claw-media play: missing file/URL\n"); return 1; }
        int audio_only = 0;
        const char *file = argv[2];
        if (argc >= 4 && strcmp(argv[2], "--audio-only") == 0) {
            audio_only = 1; file = argv[3];
        }
        int pid = do_play(file, audio_only);
        if (pid <= 0) return 1;
        /* Wait for completion in CLI mode */
        int status = 0;
        waitpid((pid_t)pid, &status, 0);
        unlink(PID_FILE);
        return WIFEXITED(status) ? WEXITSTATUS(status) : 1;

    } else if (strcmp(cmd, "info") == 0) {
        if (argc < 3) { fprintf(stderr, "claw-media info: missing file/URL\n"); return 1; }
        char info_buf[INFO_BUF_SIZE] = {0};
        int rc = do_info(argv[2], info_buf, sizeof(info_buf));
        puts(info_buf);
        return rc;

    } else if (strcmp(cmd, "stop") == 0) {
        return do_stop();

    } else {
        fprintf(stderr, "claw-media: unknown command '%s'\n", cmd);
        usage(argv[0]);
        return 1;
    }
}
