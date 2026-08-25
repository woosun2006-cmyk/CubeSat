# connection

이 폴더가 연결 설정의 단일 출처다. `gcs` 폴더의 스크립트는 모두
`connecting_port.yaml` 을 읽는다.

```text
connecting_port.yaml   현재 활성 설정. 직접 고치지 말 것
MJ.csv                 MJ 노트북 설정
change.py              CSV -> YAML 적용기
```

## 다른 노트북으로 바꾸기

1. 자기 이름으로 CSV를 만든다. `MJ.csv` 를 복사해서 고치면 된다.

   ```csv
   key,value
   name,KIM
   description,KIM 노트북
   gcs_host,10.0.0.21
   telemetry_port,14550
   relay_port,14551
   ack_port,14552
   ```

   `gcs_host` 는 그 노트북의 WireGuard 주소다. Windows에서 `ipconfig` 의
   WireGuard 어댑터에 나오는 `10.0.0.x` 값.

2. 적용한다.

   ```bash
   python3 change.py KIM.csv
   sudo systemctl restart gcs
   ```

3. 확인한다.

   ```bash
   python3 change.py --show
   ../link.sh
   ```

CSV에 적지 않은 항목은 그대로 남는다. 기체 쪽 설정(`pixhawk_device`,
`pixhawk_baud`, `send_hz`)은 사람이 바뀌어도 건드릴 필요가 없다.
