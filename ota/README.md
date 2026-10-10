# NU87을 통한 NU40 무선 펌웨어 업데이트

NU40은 Zephyr + MCUboot + MCUmgr SMP BLE를 사용합니다. NU87은 AmebaD Arduino
3.1.9의 BLE central이며 PC에서 받은 SMP 요청을 NU40으로 중계합니다.

```text
PC의 서명된 이미지 → NU87 USB UART → BLE SMP → NU40 secondary slot
NU40: 서명 검증 → 시험 부팅 → 5초 이후 OLED 처리·ACK 큐 성공 시 확정
```

최초 한 번은 NU40에 USB로 부트로더와 OTA 앱을 설치합니다. 이후 업데이트 중
NU40은 보조배터리 전원만 필요합니다. 현재 NU87이 인터넷에서 이미지를 내려받는
기능은 없으며 PC가 NU87의 USB UART로 이미지 데이터를 공급합니다.
기존 문자열·얼굴 프레임과 ACK는 별도 BLE 서비스로 계속 동작합니다.

## 플래시 배치와 서명

| 영역 | 시작 | 끝(포함하지 않음) |
| --- | --- | --- |
| 기존 MBR, 보존 | `0x00000` | `0x01000` |
| MCUboot | `0x01000` | `0x10000` |
| 실행 이미지 | `0x10000` | `0x65000` |
| 다운로드 이미지 | `0x65000` | `0xba000` |
| swap scratch | `0xba000` | `0xbe000` |
| storage | `0xbe000` | `0xc0000` |
| 공장 UF2 및 설정, 보존 | `0xc0000` | `0x100000` |

두 이미지 슬롯은 각각 340KiB입니다. `partitions.overlay`를 부트로더와 앱 모두에
적용합니다. MCUboot는 ECDSA P-256 서명을 검증하고 scratch 방식으로 교체합니다.
부트로더 스택은 upstream `prj.conf`와 같은 10KiB가 필요합니다.

현재 로컬 서명 키는 `build/ota-keys/nu40-ec-p256.pem`입니다. 키를 잃으면 설치된
부트로더가 받아들일 새 펌웨어를 서명할 수 없으므로 이 파일은 별도로 보관합니다.
개인 키와 생성 이미지·UF2는 Git에 추가하지 않습니다.

## 서명 설정 파일 준비

새 환경에서는 아래 코드를 저장소 루트에서 한 번 실행해 개인 키와 빌드 설정을
만듭니다. 기존 키가 있으면 그대로 사용합니다. 이미 OTA가 설치된 보드를 업데이트할
때는 그 보드의 최초 설치에 사용한 키를 먼저 같은 경로에 복원합니다.

```sh
python3 - <<'PYSETUP'
import os
import subprocess
import sys
from pathlib import Path

root = Path.cwd()
mcuboot = Path(os.environ.get("MCUBOOT_DIR", root.parent / "bootloader/mcuboot"))
key = root / "build/ota-keys/nu40-ec-p256.pem"
key.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
if not key.exists():
    subprocess.run([sys.executable, str(mcuboot / "scripts/imgtool.py"), "keygen",
                    "-k", str(key), "-t", "ecdsa-p256"], check=True)
key.chmod(0o600)
config = root / "build/ota-config"
config.mkdir(parents=True, exist_ok=True)
(config / "boot-key.conf").write_text(f'CONFIG_BOOT_SIGNATURE_KEY_FILE="{key}"\n')
(config / "app-key.conf").write_text(f'CONFIG_MCUBOOT_SIGNATURE_KEY_FILE="{key}"\n')
PYSETUP
```

## 빌드 및 첫 설치

기존 Zephyr/SDK 환경에서 다음 명령을 실행합니다. 키 설정 파일은 해당 경로의
`CONFIG_BOOT_SIGNATURE_KEY_FILE` 또는 `CONFIG_MCUBOOT_SIGNATURE_KEY_FILE`을 지정합니다.

```sh
west build -b nucode_nu40/nrf52840 ../bootloader/mcuboot/boot/zephyr \
  -d build/nu40-ota-boot -- \
  -DCONF_FILE=$PWD/ota/mcuboot.conf \
  -DEXTRA_CONF_FILE=$PWD/build/ota-config/boot-key.conf \
  -DEXTRA_DTC_OVERLAY_FILE=$PWD/ota/partitions.overlay

west build -b nucode_nu40/nrf52840 nu40_ble_display -d build/nu40-ota-base -- \
  -DEXTRA_CONF_FILE="ota.conf;$PWD/build/ota-config/app-key.conf" \
  -DEXTRA_DTC_OVERLAY_FILE=$PWD/ota/partitions.overlay

python3 tools/nu40_ota_package.py
python3 tools/kit_flash.py \
  --uf2 build/nu40-ota-bootstrap/bootstrap.uf2 \
  --capture-mode ble-display --capture-seconds 20
```

패키지 도구는 실제 서명·벡터·슬롯 경계를 검사하고 확정된 초기 이미지와
MCUboot를 하나의 UF2로 만듭니다. 빈 secondary/scratch/storage도 함께 초기화하므로
이 bootstrap은 최초 설치용입니다. 이후 OTA에는 `zephyr.signed.bin`을 사용합니다.
공장 UF2는 유지되어 NU40의 작은 RESET을 두 번 누르면 USB 복구 모드로 들어갑니다.

## NU87 중계 펌웨어

NU87 소스·빌드 도구·업로더는 별도 저장소의
[firmware/nu87_nu40_bridge](https://github.com/mzsz94/micro-wallaby/tree/feat/nu87-nu40-kit-ota/firmware/nu87_nu40_bridge)에
있습니다. 해당 폴더의 빌드·기록 안내를 따릅니다. 이미 `firmware=3 features=smp_relay`가
설치됐다면 NU40 KIT 통합 업데이트를 위해 NU87을 다시 기록할 필요가 없습니다.
연결된 USB bridge의 정확한 시리얼 번호를 사용하고 같은 포트를 동시에 열지 않습니다.

중계 펌웨어는 `firmware=3 features=smp_relay`로 부팅합니다. 기존 얼굴 데모가 기본이며
콘솔은 1,500,000 baud, DTR/RTS false입니다. `relay-status`로 SMP 연결을 확인합니다.
`smp <hex>`는 최대 512바이트 요청을 20바이트 BLE Write Without Response로 나누고,
같은 group/sequence/command의 알림을 재조립합니다. `pause`, `face`, `text`도 지원합니다.
현재 NU40의 ATT MTU는 23이라 알림 단편도 최대 20바이트입니다. SDK characteristic의
수신 버퍼 상한은 230바이트이고 전체 SMP 512바이트 재조립 버퍼는 스케치가 별도로
관리합니다. 설치된 중계본의 초기 512바이트 SDK 버퍼 요청은 거절되고 기본 20바이트를
유지하지만, 이 MTU의 SMP 단편 수신에는 충분합니다.

## 무선 조회와 업데이트

NU40은 보조배터리, NU87은 PC에 연결합니다. 이미지 조회는 쓰기 옵션 없이 실행합니다.
호스트 도구에는 `cbor2`, `pyserial`, `cryptography` 및 MCUboot의 imgtool 의존성이 필요합니다.

```sh
python3 tools/nu87_ota.py \
  --serial-number ACTUAL_USB_SERIAL

python3 tools/nu87_ota.py \
  --serial-number ACTUAL_USB_SERIAL \
  --image build/nu40-ota-update/zephyr/zephyr.signed.bin --write
```

새 이미지는 같은 키와 파티션 설정으로 빌드하고 `VERSION`을 올립니다.
현재 KIT 통합 버전은 `0.1.2`이며 실기에서 `0.1.1 → 0.1.2` 업데이트를 완료했습니다.

호스트는 먼저 로컬 서명을 검증하고 원격 실행 이미지가 확정됐는지 조회합니다.
UART를 열었을 때 관측되는 새 부팅 중에도 연결 확인 명령을 2초마다 재시도합니다.
SDK UART ring은 128바이트(사용 가능 127바이트)이므로 호스트는 한 번에 32바이트를
보내고 20ms 간격을 둡니다. 큰 요청 전에 `pause` 처리 응답도 기다립니다.
얼굴 전송을 일시 정지한 뒤 secondary에 업로드하고, 원격 해시와 버전이 대상과
일치하는지 확인해야 시험 부팅을 예약합니다. RESET은 한 번만 요청합니다.
재연결 후 얼굴 전송을 재개하고 새 primary의 해시와 `confirmed=true`를 다시 확인합니다.
같은 중계 세션에서 재부팅 후 SMP 확인이 시간 초과되면 UART 세션을 한 번 다시 열어
이미지 조회만 재시도합니다. 이 경로에서는 재업로드나 NU40 RESET을 추가로 보내지 않습니다.
NU40은 부팅 후 5초가 지났고 OLED 쓰기와 ACK 큐 처리가 성공했을 때만 확정합니다.
확정되지 않은 시험 이미지는 다음 MCUboot 부팅에서 원래 이미지로 되돌아갈 수 있습니다.

로그와 결과는 `build/nu87-ota/relay.log`, `result.json`에 저장합니다.
`--output`으로 별도 기록 경로를 지정할 수 있습니다. 오류가 나면 결과와 원격 이미지
상태를 확인합니다. 같은 이미지가 이미 실행·확정됐다면 재기록하지 않습니다.

## 검증

```sh
PYTHONPATH=tools python3 -m unittest \
  tools.test_nu87_ota tools.test_nu40_ota_package tools.test_smp_relay \
  tools.test_kit_flash tools.test_nu87_capture tools.test_ble_display_capture
```

호스트 검사는 서명 불일치·이미지 손상·복구 영역 경계·SMP 단편화·응답 상관관계·
업로드 정지·기존 pending 이미지·secondary 해시 불일치·RESET 응답 손실을 다룹니다.
SMP 재조립 C 코드는 ASan/UBSan으로 검사합니다.

2026-10-10 초기 설치에서 파일 전체의 flash readback 일치를 확인했습니다. 첫 부트는
작업 스택 1KiB 설정으로 앱이 시작되지 않았고, MCUboot 기본 10KiB로 수정한 뒤 동일한
앱 해시가 정상 부팅했습니다. NU40 USB 제품명은 `NU40DK BLE display OTA v3`, 실행
당시 버전은 `0.1.0`, `confirmed=1`이었습니다. 20초 캡처에서 얼굴 RX 107건의 OLED 처리와 ACK
큐 성공을 관측했습니다. 기록은 `build/ota-history/bootstrap-success-20261010-212217/`에
보관했습니다.

이어진 실기 시험에서 NU40은 보조배터리로 켜 두고 NU87만 PC에 연결했습니다.
NU87 펌웨어 3 부팅과 얼굴/ACK 211건을 확인한 다음, SMP로 NU40 `0.1.0`의 해시를
조회했습니다. 최초 전송은 192바이트 뒤 malformed 요청으로 중단됐고, 호스트 전송을
32바이트/20ms로 바꾼 뒤 167,152바이트 전체 업로드와 secondary 해시 대조가 완료됐습니다.
시험 부팅 예약과 RESET 응답도 확인했습니다.

같은 NU87 세션의 재연결 뒤 SMP 조회는 `result=-2`로 실패해 90초 확인이 시간 초과됐지만,
얼굴/ACK는 재개됐습니다. 새 UART 세션으로 재조회한 결과 primary `0.1.1`이
`active=true`, `confirmed=true`이고 해시는
`9aea9f05f8501e2c44517140fff9e318c2c02ba83b4097097954478be9bfe40e`로 서명한 대상과
일치했습니다. secondary에는 이전 `0.1.0`이 남았습니다. 사용자는 이어서
`얼굴이 다시 움직임`을 확인했습니다. 이 결과는 실제 무선 펌웨어 교체와 화면 재개를
검증하며, 장시간 안정성·전송 중 전원 차단·자동 롤백 실기는 아직 시험하지 않았습니다.

원본 실패 기록도 수정하지 않고 다음 경로에 보관했습니다.

- `build/ota-history/relay-install-20261010-214603/`: NU87 부팅·ACK·초기 이미지 조회
- `build/ota-history/wireless-update-20261010-215145/`: 최초 UART 전송 중단
- `build/ota-history/wireless-update-20261010-paced/`: 전체 전송·재부팅·확인 시간 초과 원본
- 위 경로의 `final-query/`: 새 세션에서 `0.1.1` 실행·확정 상태 조회

호스트 도구는 이 관측에 맞춰 부팅 중 연결 확인 재시도, UART 간격 조절,
확인 전용 새 세션 재시도를 포함하며 관련 호스트 검사도 추가했습니다.

이후 KIT 통합 앱 `0.1.2`의 208,802바이트를 같은 NU87으로 전송했습니다.
secondary 해시 대조·시험 부팅 예약·단일 RESET을 완료했고, 재부팅 후 첫 세션의
90초 확인은 다시 시간 초과됐습니다. 이번에는 도구의 확인 전용 새 UART 세션 경로가
자동 실행돼 `active=true`, `confirmed=true`와 대상 해시 일치를 확인했습니다.
재업로드나 추가 NU40 RESET은 없었습니다. OLED·RGB·비프·녹음 재생의 사용자 확인과
SD WAV 저장·대조, 원격 KIT 제어도 완료했습니다.
[0.1.2 실기 결과](../docs/kit-hardware-results.md)에 해시와 관측 수치를 남겼습니다.

프로토콜 기준: [Zephyr SMP BLE transport](https://docs.zephyrproject.org/latest/services/device_mgmt/smp_transport.html),
[MCUboot image and swap design](https://docs.mcuboot.com/design.html).
