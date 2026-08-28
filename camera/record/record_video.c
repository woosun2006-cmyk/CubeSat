#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* 배치가 고정되어 있어 경로를 세지 않고 그대로 쓴다. 다른 계정이나 다른
   위치에 두면 CUBESAT_ROOT 로 덮어쓴다. 경로가 틀리면 mkdir 에서 바로
   실패하므로, 조용히 엉뚱한 곳에 쌓이는 일은 없다. */
#define PROJECT_ROOT_DEFAULT "/home/jih/cubesat"

typedef struct {
    int camera_id;
    int fps;
    int quality;
    char awb_mode[32];
} record_settings_t;

static pid_t child_pid = -1;

static char *trim(char *text)
{
    char *end;

    while (isspace((unsigned char)*text)) {
        text++;
    }
    end = text + strlen(text);
    while (end > text && isspace((unsigned char)end[-1])) {
        *--end = '\0';
    }
    return text;
}

static void remove_comment(char *text)
{
    char *comment = strchr(text, '#');
    if (comment != NULL) {
        *comment = '\0';
    }
}

static int setting_int(const char *value, int fallback)
{
    char *end;
    long parsed;

    errno = 0;
    parsed = strtol(value, &end, 10);
    if (errno != 0 || end == value) {
        return fallback;
    }
    return (int)parsed;
}

static void load_settings(const char *path, record_settings_t *settings)
{
    FILE *file;
    char line[256];
    int in_record = 0;

    settings->camera_id = 0;
    settings->fps = 24;
    settings->quality = 70;
    snprintf(settings->awb_mode, sizeof(settings->awb_mode), "auto");

    file = fopen(path, "r");
    if (file == NULL) {
        fprintf(stderr, "record_video: config not found: %s\n", path);
        fprintf(stderr, "record_video: using defaults (camera=0 fps=24 quality=70 awb=auto)\n");
        return;
    }

    while (fgets(line, sizeof(line), file) != NULL) {
        char *text = trim(line);
        char *colon;
        char *key;
        char *value;

        remove_comment(text);
        text = trim(text);
        if (*text == '\0') {
            continue;
        }

        if (strcmp(text, "record:") == 0) {
            in_record = 1;
            continue;
        }
        if (in_record && line[0] != ' ' && line[0] != '\t') {
            in_record = 0;
        }
        if (!in_record) {
            continue;
        }

        colon = strchr(text, ':');
        if (colon == NULL) {
            continue;
        }
        *colon = '\0';
        key = trim(text);
        value = trim(colon + 1);

        if (strcmp(key, "camera_id") == 0) {
            settings->camera_id = setting_int(value, settings->camera_id);
        } else if (strcmp(key, "fps") == 0) {
            settings->fps = setting_int(value, settings->fps);
        } else if (strcmp(key, "quality") == 0) {
            settings->quality = setting_int(value, settings->quality);
        } else if (strcmp(key, "awb_mode") == 0 && *value != '\0') {
            snprintf(settings->awb_mode, sizeof(settings->awb_mode), "%s", value);
        }
    }
    fclose(file);
}

static void stop_child(int signum)
{
    (void)signum;
    if (child_pid > 0) {
        kill(child_pid, SIGTERM);
    }
}

static int make_directory(const char *path)
{
    struct stat info;

    if (mkdir(path, 0755) == 0) {
        return 0;
    }
    if (errno == EEXIST && stat(path, &info) == 0 && S_ISDIR(info.st_mode)) {
        return 0;
    }
    perror("record_video: mkdir");
    return -1;
}

int main(int argc, char **argv)
{
    const char *project_dir;
    char config_path[PATH_MAX];
    char video_dir[PATH_MAX];
    char output_path[PATH_MAX];
    char camera_id[32];
    char fps[32];
    char quality[32];
    char timeout_ms[64];
    char filename[64];
    char *end;
    double seconds = 10.0;
    long double timeout_value;
    record_settings_t settings;
    struct sigaction action;
    struct sigaction old_int;
    struct sigaction old_term;
    struct tm local_time;
    time_t now;
    pid_t waited;
    int status;

    if (argc > 2) {
        fprintf(stderr, "usage: %s [seconds]\n", argv[0]);
        return 2;
    }
    if (argc == 2) {
        errno = 0;
        seconds = strtod(argv[1], &end);
        if (errno != 0 || end == argv[1] || *trim(end) != '\0' ||
            !isfinite(seconds) || seconds <= 0.0) {
            fprintf(stderr, "record_video: seconds must be a positive number\n");
            return 2;
        }
    }

    project_dir = getenv("CUBESAT_ROOT");
    if (project_dir == NULL || project_dir[0] == '\0') {
        project_dir = PROJECT_ROOT_DEFAULT;
    }
    if (snprintf(config_path, sizeof(config_path), "%s/setting/cam_sets.yaml",
                 project_dir) >= (int)sizeof(config_path) ||
        snprintf(video_dir, sizeof(video_dir), "%s/log/camera",
                 project_dir) >= (int)sizeof(video_dir)) {
        fprintf(stderr, "record_video: path is too long\n");
        return 1;
    }
    load_settings(config_path, &settings);
    if (make_directory(video_dir) != 0) {
        return 1;
    }

    now = time(NULL);
    if (localtime_r(&now, &local_time) == NULL ||
        strftime(filename, sizeof(filename), "video_%m%d_%H%M%S.mp4",
                 &local_time) == 0 ||
        snprintf(output_path, sizeof(output_path), "%s/%s", video_dir,
                 filename) >= (int)sizeof(output_path)) {
        fprintf(stderr, "record_video: cannot create output filename\n");
        return 1;
    }

    timeout_value = (long double)seconds * 1000.0L;
    if (timeout_value > (long double)LLONG_MAX) {
        fprintf(stderr, "record_video: recording duration is too large\n");
        return 2;
    }

    snprintf(camera_id, sizeof(camera_id), "%d", settings.camera_id);
    snprintf(fps, sizeof(fps), "%d", settings.fps);
    snprintf(quality, sizeof(quality), "%d", settings.quality);
    snprintf(timeout_ms, sizeof(timeout_ms), "%.0Lf", timeout_value);

    printf("recording %.0fs at 640x640 -> %s\n", seconds, output_path);
    fflush(stdout);

    memset(&action, 0, sizeof(action));
    action.sa_handler = stop_child;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, &old_int);
    sigaction(SIGTERM, &action, &old_term);

    child_pid = fork();
    if (child_pid < 0) {
        perror("record_video: fork");
        sigaction(SIGINT, &old_int, NULL);
        sigaction(SIGTERM, &old_term, NULL);
        return 1;
    }
    if (child_pid == 0) {
        char *const command[] = {
            "rpicam-vid",
            "--camera", camera_id,
            "--width", "640",
            "--height", "640",
            "--framerate", fps,
            "--awb", settings.awb_mode,
            "--codec", "libav",
            "--libav-format", "mp4",
            "--quality", quality,
            "--nopreview",
            "--timeout", timeout_ms,
            "-o", output_path,
            NULL
        };
        execvp(command[0], command);
        perror("record_video: exec rpicam-vid");
        _exit(127);
    }

    do {
        waited = waitpid(child_pid, &status, 0);
    } while (waited < 0 && errno == EINTR);

    child_pid = -1;
    sigaction(SIGINT, &old_int, NULL);
    sigaction(SIGTERM, &old_term, NULL);

    if (waited < 0) {
        perror("record_video: waitpid");
        return 1;
    }
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        printf("saved: %s\n", output_path);
        return 0;
    }
    if (WIFSIGNALED(status)) {
        fprintf(stderr, "record_video: rpicam-vid stopped by signal %d\n",
                WTERMSIG(status));
        return 128 + WTERMSIG(status);
    }
    fprintf(stderr, "record_video: rpicam-vid exited with status %d\n",
            WEXITSTATUS(status));
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}
