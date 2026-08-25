## Architecture
모뎀으로 vpn거쳐서 노트북으로 데이터 보내기.
혹은 노트북에서 라즈베리파이에 접속해서 데이터를 받아올 수 있도록 시스템 구축

##Programs

### MAVLink.c
purpose : 라즈베리 파이(raspberri pi 4 B+)와 픽스호크(pixhawk 4 mini)의 연결을 담당

### data.c
purpose : 픽스호크에서 기체 데이터(GPS, IMU 센서 값)을 읽어옴.

### LTE.c
purpose : LTE모뎀과 라즈베리파이의 연결 및 통신을 원활히 하고, ip를 고정 하는 역할을 함.

### VPN.c
purpose : wireguard를 통해서 원거리에서도 통신을 할 수 있게끔 함.

### gcs.c
purpose : gcs program을 최종적으로 빌딩.
-위의 모든 프로그램을 종합 하여, 노트북으로 데이터 패키지를 보냄.
