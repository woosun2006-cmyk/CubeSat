## 각 스크립트 사용법

### gcs.sh
`./gcs.sh 50`
50Hz로 gcs프로그램을 통신, htop 처럼 GCS에 보내는 픽스호크 값을 터미널에서 gui로 띄워 줌.

### record.sh
`./record.sh 10min`
10분동안 아래 방향의 카메라 촬영 기록 및 짐벌 작동
`./record.sh 10sec`
10초동안 아래방향의 카메라 촬영 기록 및 짐벌 작동
** 짐벌 작동은 촬영 시간 만큼 작동함 **

### cubesat.sh
위의 두 스크립트 합친 결과물(전체 미션 수행 스크립트)
`./cubesat.sh --50Hz --10min`
50Hz로 gcs 통신 및 10분동안 카메라 촬영 기록 및 짐벌 작동
** 50Hz, 10min  순서 바뀌어도 무관 **

## 로그 저장 경로
cubesat/log/gcs : gcs.sh 에서 보내는 값들에 대한 로그 저장
cubesat/log/video : record.sh 로 녹화한 영상 저장
