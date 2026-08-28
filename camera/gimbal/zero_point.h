#ifndef ZERO_POINT_H
#define ZERO_POINT_H

#include <stdio.h>
#include <stdlib.h>

#define ZERO_POINT_DEFAULT 15

/* 예전에는 "zero_point.yaml" 을 cwd 기준 상대경로로 열었다. 그래서 짐벌
   폴더 밖에서 실행하면 파일을 못 찾고 조용히 기본값 15 로 떨어졌다 —
   scripts/ 에서 부르는 record.sh 가 실제로 그랬고, 보정값 pitch 18 이
   무시됐다. 배치가 고정이므로 루트를 박고 절대경로를 만든다.
   다른 계정이나 다른 위치라면 CUBESAT_ROOT 로 덮어쓴다. */
#define CUBESAT_ROOT_DEFAULT "/home/jih/cubesat"

static inline const char *zero_point_path(void) {
    static char path[512];
    const char *root = getenv("CUBESAT_ROOT");
    if (root == NULL || root[0] == '\0') root = CUBESAT_ROOT_DEFAULT;
    snprintf(path, sizeof(path), "%s/camera/gimbal/zero_point.yaml", root);
    return path;
}

// zero_point.yaml 에서 pitch/roll 영점 duty 를 읽는다. 파일이 없거나
// 키를 못 찾으면 ZERO_POINT_DEFAULT(정중앙)를 그대로 둔다.
static inline void load_zero_point(int *pitch_zero, int *roll_zero) {
    *pitch_zero = ZERO_POINT_DEFAULT;
    *roll_zero = ZERO_POINT_DEFAULT;

    FILE *f = fopen(zero_point_path(), "r");
    if (!f) return;

    char line[128];
    int val;
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, " pitch_zero_duty: %d", &val) == 1) *pitch_zero = val;
        else if (sscanf(line, " roll_zero_duty: %d", &val) == 1) *roll_zero = val;
    }
    fclose(f);
}

static inline int save_zero_point(int pitch_zero, int roll_zero) {
    FILE *f = fopen(zero_point_path(), "w");
    if (!f) return -1;

    fprintf(f,
        "# zero_point.yaml — 서보모터 영점(기준점) 설정\n"
        "#\n"
        "# gimbal 서보 두 개의 \"영점\" duty 값. 짐벌 프로그램은 시작할 때 먼저\n"
        "# 이 위치로 이동한 뒤, 자세값(pitch/roll)에 따라 이 값을 중심으로 움직인다.\n"
        "# zero_calib 으로 다시 잡을 수 있다.\n"
        "#\n"
        "#   ./zero_calib          WASD 로 조정 후 space 로 저장\n"
        "#\n"
        "# duty 범위는 PWM_RANGE=200 기준 10~20 (1.0ms~2.0ms), 15가 정중앙(1.5ms).\n"
        "\n"
        "pitch_zero_duty: %d   # PITCH_PIN(1, GPIO18) 영점\n"
        "roll_zero_duty: %d    # ROLL_PIN(26, GPIO12) 영점\n",
        pitch_zero, roll_zero);

    fclose(f);
    return 0;
}

#endif
