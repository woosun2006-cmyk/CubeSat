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

## Tailscale 새 사용자 설정

Tailscale 관리자 설정과 Pi의 `sudo tailscale set --ssh`는 처음 한 번만
해 둔다. 새 사용자는 다음만 하면 된다.

1. Tailscale 초대를 받고 로그인한다.
2. PC에서 연결과 주소를 확인한다.

```powershell
tailscale ping cubesat-pi
tailscale ip -4
```

3. 새 PowerShell에서 다음을 한 번 설정한다. `100.109.5.8`은 현재 Pi의
Tailscale IP다.

```powershell
setx CUBESAT_PI_HOST cubesat-pi
setx CUBESAT_TS_HOST cubesat-pi
setx CUBESAT_PI_SOURCE 100.109.5.8
```

4. Pi에 접속해서 자기 PC의 Tailscale IP로 CSV를 만들고 적용한다.

```bash
ssh -F NUL -o IdentitiesOnly=yes -o IdentityFile=NUL jih@cubesat-pi
cd ~/cubesat/gcs/connection
cp HJ_tailscale.csv NAME_tailscale.csv
nano NAME_tailscale.csv
python3 change.py NAME_tailscale.csv
```

CSV에서는 `gcs_host`만 자기 PC의 `tailscale ip -4` 결과로 바꾸고, 나머지
포트는 그대로 둔다. CSV는 Pi에 만들어야 한다.

5. 이전 사용자의 GCS를 종료한 뒤 PC에서 기존처럼 실행한다.

```powershell
.\cubesat-all.bat --lora-port COMx
```

한 번에 한 명만 사용할 수 있다. SSH 키와 WireGuard peer를 새로 만들 필요는
없다. 기존 WireGuard 설정은 fallback용으로 남겨둔다.

## PC 원클릭 설정

새 PC에서는 PC의 `gcs_proto` 폴더에 있는 `setup-cubesat.bat`을 실행한다.
auth key를 입력하면 Tailscale 등록, PC IP CSV 생성·적용, GCS 실행까지
자동으로 처리된다. 따라서 새 사용자가 Pi에서 CSV를 직접 만들 필요는 없다.

이 기능을 쓰려면 Tailscale 관리자 화면의 `Keys`에서 만든 auth key가 필요하다.
auth key는 비밀번호와 같으므로 파일에 저장하거나 공개하지 않는다.
