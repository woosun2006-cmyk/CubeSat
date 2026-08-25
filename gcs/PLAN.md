# CubeSat GCS 구현 계획

작성일: 2026-08-25

## 목표

노트북에서 Qt 기반 GCS 프로그램 하나만 실행해 LTE/WireGuard를 통해 수신한 Pixhawk telemetry를 표시한다. 라즈베리파이에서는 GUI를 실행하지 않고 telemetry 송신 프로그램만 백그라운드로 실행한다.

## 최종 구조

```text
Pixhawk 4 Mini
  -> Raspberry Pi data.c / gcs.c
  -> USB LTE modem(usb0)
  -> WireGuard(wg0, Pi: 10.0.0.17)
  -> Laptop WireGuard peer
  -> Qt GCS application
```

## Pi 구성

- `MAVLink.c`: Pixhawk MAVLink v1/v2 수신 및 파싱
- `data.c`: IMU/GPS reader API
- `gcs.c`: data reader에서 받은 telemetry를 UDP JSON으로 송신
- `gcs.sh`: gcs 빌드/실행 스크립트
- `wg-quick@wg0`: LTE 경로의 WireGuard 터널
- 최종적으로 `gcs.service`를 부팅 자동 실행으로 등록

Pi에서는 GUI를 실행하지 않는다. GUI 종료와 무관하게 telemetry 송신은 계속되어야 한다.

## 노트북 Qt GUI 구성

### 상단 상태바

- VPN 연결 상태
- telemetry 수신 상태 및 마지막 패킷 시각
- GPS Fix 상태
- 현재 고도
- 로그 저장 상태

### 중앙 자세 영역

- 인공수평계: roll에 따른 수평선 회전, pitch 눈금 이동
- 3D 기체/큐브 모델: roll, pitch, yaw를 적용한 자세 표시
- 숫자 표시: roll, pitch, yaw(degree)

### GPS 영역

- 위도, 경도, GPS 고도
- 상대 고도
- 위성 수와 Fix 상태
- 지도 위 현재 위치

### 하단 상태/로그 영역

- 연결 끊김 및 재접속 상태
- 수신 지연
- GPS 상태 변화
- 저장 파일 경로

## 개발 순서

1. Qt 프로젝트와 UDP telemetry receiver 구성
2. JSON packet parser 및 연결 상태 표시
3. IMU 수치 패널
4. 2D 인공수평계
5. GPS 수치 패널 및 지도 영역
6. 3D 모델 렌더링
7. CSV/JSON 로그 저장 및 재접속 처리
8. Pi systemd 자동 실행 구성
9. LTE 단절/복구와 장시간 운용 시험

## 데이터 계약

현재 Pi `gcs.c`가 UDP로 보내는 JSON 필드:

- `ts_ms`
- `attitude_valid`, `roll`, `pitch`, `yaw`
- `gps_valid`, `gps_fix`, `gps_sats`
- `lat`, `lon`, `alt_m`, `rel_alt_m`
- `vx_cms`, `vy_cms`, `vz_cms`, `hdg_cdeg`

자세각은 MAVLink 라디안을 노트북 GUI에서 degree로 변환해 표시한다. GPS가 없으면 `gps_valid=false`로 표시하고 IMU 화면은 계속 동작한다.

## 실행 정책

- Pi: WireGuard와 `gcs`만 자동 실행
- 노트북: WireGuard 연결 후 Qt GCS 하나만 실행
- GUI는 Pi에 별도로 실행하지 않는다.
- UDP 포트는 기본 14550이며, VPN 내부에서만 사용한다.

## 완료 기준

- 노트북 GUI에서 IMU roll/pitch/yaw가 실시간 갱신됨
- GPS 미연결 시에도 IMU가 표시되고 GPS는 `NO FIX`로 표시됨
- LTE/WireGuard 단절 후 복구하면 자동으로 수신 재개
- Pi에 SSH로 접속하지 않아도 telemetry 송신이 자동 시작됨
