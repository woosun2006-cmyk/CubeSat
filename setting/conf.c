#define _POSIX_C_SOURCE 200809L

#include "conf.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 다른 위치에 두면 CUBESAT_ROOT 로 덮어쓴다 (camera/record/record_video.c 와 동일 규칙). */
#define PROJECT_ROOT_DEFAULT "/home/jih/cubesat"

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

/* 매번 열고 닫는다 -- 부팅 시 십여 개 값을 한 번씩 읽는 정도라 캐시할 값이
 * 없고, 실행 중 port.yaml 이 바뀌면 재시작 없이도 다음 조회에 반영된다. */
static const char *find_raw(const char *name)
{
    static char value_buf[128];
    const char *project_dir;
    char path[PATH_MAX];
    char line[256];
    FILE *file;

    project_dir = getenv("CUBESAT_ROOT");
    if (project_dir == NULL || project_dir[0] == '\0') {
        project_dir = PROJECT_ROOT_DEFAULT;
    }
    if (snprintf(path, sizeof(path), "%s/setting/port.yaml", project_dir) >= (int)sizeof(path)) {
        return NULL;
    }

    file = fopen(path, "r");
    if (file == NULL) {
        return NULL;
    }

    while (fgets(line, sizeof(line), file) != NULL) {
        char *comment = strchr(line, '#');
        char *text;
        char *colon;
        char *key;
        char *value;

        if (comment != NULL) {
            *comment = '\0';
        }
        text = trim(line);
        if (*text == '\0') {
            continue;
        }

        colon = strchr(text, ':');
        if (colon == NULL) {
            continue;
        }
        *colon = '\0';
        key = trim(text);
        value = trim(colon + 1);

        if (strcmp(key, name) == 0 && *value != '\0') {
            snprintf(value_buf, sizeof(value_buf), "%s", value);
            fclose(file);
            return value_buf;
        }
    }

    fclose(file);
    return NULL;
}

const char *conf_get(const char *name, const char *fallback)
{
    const char *raw = find_raw(name);
    return raw != NULL ? raw : fallback;
}

long conf_int(const char *name, long fallback)
{
    const char *raw = find_raw(name);
    char *end;
    long parsed;

    if (raw == NULL) {
        return fallback;
    }
    errno = 0;
    parsed = strtol(raw, &end, 10);
    if (errno != 0 || end == raw) {
        return fallback;
    }
    return parsed;
}

double conf_num(const char *name, double fallback)
{
    const char *raw = find_raw(name);
    char *end;
    double parsed;

    if (raw == NULL) {
        return fallback;
    }
    errno = 0;
    parsed = strtod(raw, &end);
    if (errno != 0 || end == raw) {
        return fallback;
    }
    return parsed;
}
