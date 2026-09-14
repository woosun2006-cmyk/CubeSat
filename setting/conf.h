#ifndef CUBESAT_CONF_H
#define CUBESAT_CONF_H

/* setting/port.yaml 한 곳에서 통신 설정을 읽는다. conf.py 와 같은 규칙:
 * 한 단짜리 "key: value" 만 보고, 없으면 부르는 쪽의 기본값으로 돌아간다. */

const char *conf_get(const char *name, const char *fallback);
long conf_int(const char *name, long fallback);
double conf_num(const char *name, double fallback);

#endif
