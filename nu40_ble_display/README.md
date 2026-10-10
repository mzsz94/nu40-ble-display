# NU40DK-KIT 통합 BLE 앱

NU40DK-KIT의 OLED, 키 4개, RGB 4개, PDM 마이크, I²S 스피커,
microSD를 하나의 Zephyr 앱에서 사용합니다. NU87이 보내는 문자열·얼굴 프레임과
서명된 BLE OTA 업데이트도 유지합니다. NU40은 보조배터리만 연결해 동작할 수 있습니다.
NU87은 AmebaD Arduino 3.1.9 기반 central이며 PC의 USB UART 명령을 BLE로 중계합니다.

## 사용할 수 있는 기능

| 부품 | 통합 동작 |
| --- | --- |
| OLED | NU87 문자열·움직이는 얼굴, 마이크 peak/RMS, KIT 상태 화면 |
| 키 4개 | 화면 전환, 녹음, 비프, 녹음 재생, RGB 모드 전환 |
| RGB 4개 | BLE 연결·키 입력·녹음 표시, 마이크 음량 반응, 끄기 |
| PDM 마이크 | 16 kHz/16-bit/mono 음량 측정과 2초 PCM 녹음 |
| MAX98357A + 스피커 | 1 kHz/300 ms 비프, 현재 부팅에서 녹음한 2초 재생 |
| microSD | FAT 마운트, 새 WAV 저장, 전체 파일 읽기 대조 |
| BLE | 기존 문자열·얼굴 수신과 ACK, KIT 상태 조회·제어 |
| OTA | MCUboot 서명 검증·시험 부팅·확정, NU87을 통한 무선 업데이트 |

마이크와 오디오/SD 처리는 별도 스레드에서 실행합니다. 화면을 마이크·상태 페이지로
바꿔도 NU87 프레임은 최신 상태로 보관하고 ACK를 보냅니다. 이때 ACK는 선택된 화면의
OLED 처리가 성공했다는 뜻이며, 수신한 얼굴이 현재 보인다는 뜻은 아닙니다.
첫 페이지로 돌아가면 가장 최근 NU87 프레임을 표시합니다. 연결이 없으면 로컬 얼굴이 움직입니다.

OLED는 SSD1306 호환 0.91인치 128×32, I²C `0x3c`입니다. 글꼴은 6×8 ASCII,
21열 × 4행입니다. 한글 글리프·음성 인식·인터넷 이미지 다운로드는 포함하지 않습니다.
핀 구성은 실기 확인한 `kit_selftest/app.overlay`와 같습니다.

## 키 사용법

코드의 key 0~3은 기존 진단 앱과 같은 물리 키입니다. 일반적으로 큰 키 1~4에 대응합니다.

| 큰 키 | 동작 |
| --- | --- |
| 1번 | 화면 순환: NU87 화면 → 마이크 음량 → KIT 상태 |
| 2번 | 2초 녹음 시작. RGB 빨강으로 표시, 카드가 있으면 WAV도 저장 |
| 3번 | 비프음 |
| 4번 짧게 | 현재 부팅에서 마지막으로 녹음한 내용 재생 |
| 4번 0.8초 이상 | RGB 모드 순환: 상태 색 → 마이크 음량 반응 → 끄기 |

오디오/SD 작업은 한 번에 하나만 실행합니다. 작업 중 다른 오디오 요청은 busy로 거절합니다.
녹음 전 4번을 누르면 재생할 데이터가 없다는 결과를 반환합니다.
RGB 최대 밝기는 채널당 8/255이며, 재생은 원본 PCM의 1배 이득을 사용합니다.

## SD 파일

기존 FAT 카드에서 `/KITSD:/NU40KIT/R00000.WAV`부터 사용하지 않은 이름을 선택합니다.
기존 파일을 덮어쓰거나 지우지 않으며, 포맷 기능은 빌드에서 비활성화합니다.
파일은 표준 PCM WAV(16 kHz, mono, signed 16-bit little endian)이고 2초 파일은
64,044바이트입니다. 저장 후 전체 WAV 헤더와 PCM을 RAM 녹음과 대조합니다.
재생 시에도 저장 파일을 다시 대조한 다음 같은 PCM을 스피커로 보냅니다.

카드가 없거나 마운트·저장이 실패해도 RAM 녹음은 남습니다. 파일이 저장됐다면
그 파일의 대조에 실패한 재생 요청은 오류로 종료합니다. 현재 재생 버퍼는 재부팅 시
초기화되며, 기존 WAV를 목록에서 골라 재생하는 기능은 없습니다.
카드는 전원을 켜기 전에 삽입합니다. 카드 없이 시작한 경우 삽입 후 `sd` 명령으로
다시 마운트할 수 있습니다. 사용 중 카드 제거·교체는 지원하지 않습니다.

## NU87을 통한 제어

기존 NU87 `firmware=3 features=smp_relay`를 그대로 사용합니다.
NU40은 전원만, NU87은 PC USB에 연결합니다.
이 저장소 루트에서 다음 명령을 실행합니다.

```sh
python3 tools/kit_control.py status
python3 tools/kit_control.py page --value 1
python3 tools/kit_control.py rgb --value 1
python3 tools/kit_control.py beep
python3 tools/kit_control.py record
python3 tools/kit_control.py play
python3 tools/kit_control.py page --value 0
```

다른 NU87이면 `--serial-number ACTUAL_USB_SERIAL`을 지정합니다.
같은 포트를 다른 터미널과 동시에 열지 않습니다. 제어 도구는 NU87의 얼굴 전송을
잠시 멈추고 SMP 요청을 처리한 후 얼굴 전송을 재개합니다.
`status --watch 20`은 20초 동안 키·마이크 상태를 수집합니다.
로그·결과는 `build/kit-control/`에 저장하며 `--output`으로 별도 경로를 지정할 수 있습니다.
명령은 큐 수락과 실제 완료를 구분하고, 녹음·재생·비프 완료 카운터를 대조합니다.
SMP 상태 조회가 `-2`로 시간 초과되면 읽기만 최대 3회 시도합니다.
이미 실행됐을 수 있는 녹음·재생 등의 쓰기 명령은 자동 반복하지 않습니다.

사용자 SMP 그룹은 64입니다. 명령 0 read는 schema 1 상태 맵,
명령 1 write는 `{action, value}`를 받습니다. action은 beep=1, record=2,
play=3, sd=4, page=5, rgb=6입니다. page/rgb 값은 0~2이고 나머지는 0입니다.
원격 상태는 드라이버·PCM·파일 처리 증거이며 실제 화면·소리·음질은 직접 확인합니다.

NU40 USB 콘솔(115200 baud)은 `status`, `beep`, `record`, `play`, `sd`, `mic`,
`face`, `rgb`, `uf2`를 지원합니다. USB 제품명은 기존 OTA v3를 유지합니다.

## 빌드와 업데이트

OTA 최초 설치·키 관리·복구 배치는 [OTA 안내](../ota/README.md)를 따릅니다.
현재 앱 버전은 `VERSION`의 0.1.2입니다. 기존 OTA 슬롯 크기는 각각 340KiB입니다.

```sh
python3 -m west build \
  -b nucode_nu40/nrf52840 nu40_ble_display -d build/nu40-kit-ota -- \
  -DEXTRA_CONF_FILE="ota.conf;$PWD/build/ota-config/app-key.conf" \
  -DEXTRA_DTC_OVERLAY_FILE=$PWD/ota/partitions.overlay

python3 tools/nu87_ota.py \
  --image build/nu40-kit-ota/zephyr/zephyr.signed.bin --write
```

첫 bootstrap을 다시 설치하지 않고 서명된 앱만 무선으로 보냅니다.
NU40 USB 연결이나 NU87 펌웨어 재기록은 필요하지 않습니다.

## 검증

```sh
PYTHONPATH=tools python3 -m unittest discover -s tools -p 'test_*.py'

python3 ../zephyr/scripts/twister \
  -T nu40_ble_display -p nucode_nu40/nrf52840 --build-only --outdir build/twister-kit
```

2026-10-10: 호스트 검사 171개 통과. 버튼 독립 디바운스·긴 누름·uptime wrap,
WAV 손상/크기 경계, 실제 C 코드 ASan/UBSan, 원격 작업 완료 판정 등을 검사했습니다.
NU40 OTA 빌드 FLASH 208,652 bytes / RAM 133,944 bytes, 서명 이미지 208,802 bytes입니다.
Twister는 기본 통합 앱의 build-only 구성 1개를 경고 없이 빌드했습니다.
NU40이 보조배터리에 연결된 상태에서 NU87을 통해 0.1.2로 업데이트했고,
실행 이미지의 해시·active·confirmed를 확인했습니다. 사용자는 OLED·RGB·비프·
녹음 재생을 모두 확인했으며 원격 제어와 SD WAV 저장·읽기 대조도 성공했습니다.
[실기 결과와 남은 검증 범위](../docs/kit-hardware-results.md)를 참고하세요.

글꼴은 Adafruit_GFX classic ASCII font에서 파생했으며 BSD 라이선스는
`LICENSE.font`에 보존했습니다. 기존 무선 얼굴 및 OTA 실기 기록은 각각
`build/nu87-history/`와 `build/ota-history/`에 남아 있습니다.
