# 공용 설정 읽기. connection/connecting_port.yaml 이 단일 출처다.
#
# 이 파일을 source 하기 전에 PROJECT_DIR 이 정해져 있어야 한다.
# 값 우선순위: 환경변수 > YAML > 스크립트 기본값
#
# YAML은 한 단계 "key: value" 만 쓰므로 sed 로 충분하다. 파이에 PyYAML 같은
# 패키지를 더 깔지 않으려는 것이다.

CONF_FILE=${CONF_FILE:-$PROJECT_DIR/connection/connecting_port.yaml}

conf() {
    _v=""
    if [ -f "$CONF_FILE" ]; then
        _v=$(sed -n "s/^[[:space:]]*$1[[:space:]]*:[[:space:]]*//p" "$CONF_FILE" \
             | head -n 1 \
             | sed 's/[[:space:]]*#.*$//' \
             | sed 's/[[:space:]]*$//')
    fi
    if [ -n "$_v" ]; then
        printf '%s\n' "$_v"
    else
        printf '%s\n' "$2"
    fi
}

# 인자가 주사율로 쓸 수 있는 수인지 본다. 0 초과 50 이하만 통과시킨다.
# 링크는 50 Hz 까지 무손실로 실측되었고 그 위는 권하지 않는다.
is_hz() {
    case "$1" in
        ''|*[!0-9.]*|*.*.*) return 1 ;;
    esac
    awk -v v="$1" 'BEGIN { exit !(v > 0 && v <= 50) }'
}
