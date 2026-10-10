# NU40 BLE OLED Display

NU40DK-KIT의 OLED에 BLE로 받은 문자열을 표시하는 Zephyr 앱입니다.
NU40은 peripheral로 광고하고, NU87 같은 central이 전송한 메시지를 조립해 출력한 뒤
OLED 처리 결과를 notification으로 응답합니다.

```text
NU87 / BLE central ── GATT write ──> NU40DK-KIT ──> OLED
                  <── ACK notify ──
```

수신 앱과 공통 프로토콜을 제공합니다. NU87 송신 펌웨어는 이 저장소에 포함하지 않습니다.

## 하드웨어

| 항목 | 설정 |
| --- | --- |
| 보드 | NUCODE NU40 / nRF52840 + NU40DK-KIT |
| OLED | SSD1306 호환, 0.91인치, 128×32, I²C 주소 `0x3c` |
| I²C | SDA P0.22, SCL P0.21, 400 kHz |
| 화면 | 6×8 글꼴, 21열 × 4행 |
| 입력 문자 | printable ASCII와 LF, 메시지 최대 84바이트 |
| BLE 장치 이름 | `NU40-KIT-DISPLAY` |
| USB 콘솔 | CDC ACM, 115200 baud, `status` / `uf2` 명령 |

연결 전에는 `WAITING FOR NU87`를 표시합니다. 연결이 끊기면 대기 화면으로 돌아가
다시 광고합니다. 화면 처리는 앱 스레드에서 실행하며 BLE 콜백에서는 수신·큐 처리만 합니다.
현재 한글 글리프, 페어링, 암호화는 지원하지 않습니다.

## 빌드

검증 기준은 Zephyr `e70694102ad6f910485155a824c5483daf605a9f`와 Zephyr SDK 1.0.1입니다.
Zephyr 개발 환경을 준비한 뒤 기존 West 워크스페이스 안에서 실행합니다.

```bash
git clone https://github.com/mzsz94/nu40-ble-display.git
west build -b nucode_nu40/nrf52840 nu40-ble-display/nu40_ble_display \
  -d build/nu40-ble-display
```

새 워크스페이스는 이 저장소의 `west.yml`로 초기화할 수 있습니다.

```bash
mkdir nu40-workspace
cd nu40-workspace
git clone https://github.com/mzsz94/nu40-ble-display.git
west init -l nu40-ble-display
west update
west zephyr-export
# Zephyr의 호스트 의존성과 SDK 설치를 마친 뒤:
west build -b nucode_nu40/nrf52840 nu40-ble-display/nu40_ble_display \
  -d build/nu40-ble-display
```

생성 파일은 `build/nu40-ble-display/zephyr/zephyr.uf2`입니다.
검증한 BARAM-NU40 부트로더는 `Board-ID: BARAM-NU40`, `SoftDevice: not found`이며
앱 시작 주소는 `0x1000`입니다. 다른 부트로더 구성은 별도 확인이 필요합니다.
NU40 모듈의 작은 RESET을 빠르게 두 번 눌러 부트 드라이브를 연 뒤 UF2를 복사합니다.
실행 후 OLED의 대기 화면과 콘솔의 다음 로그를 확인합니다.

```text
LINK|BOOT role=display firmware=1 oled_init=0 protocol=1
LINK|BLE enable_result=0
LINK|BLE advertising_result=0 name=NU40-KIT-DISPLAY
```

## BLE 프로토콜

| 용도 | UUID | 동작 |
| --- | --- | --- |
| Service | `90e2c000-3c0d-4ef3-9d78-574cd1a64087` | 서비스 검색 |
| ACK | `90e2c001-3c0d-4ef3-9d78-574cd1a64087` | Notify, CCCD 구독 |
| TEXT | `90e2c002-3c0d-4ef3-9d78-574cd1a64087` | Write with response |

기본 ATT MTU 23을 사용합니다. TEXT는 최대 20바이트이며 다음 형식으로 전송합니다.

```text
[version=1, type=1, seq_low, seq_high, chunk_index, total_length, text...]
```

`chunk_index`는 0부터 시작합니다. 문자열을 14바이트씩 순서대로 나누며 마지막 조각만
짧을 수 있습니다. `total_length`는 전체 문자열의 바이트 길이이고 NUL 종료 문자는 보내지
않습니다. 모든 조각은 동일한 16비트 little-endian 시퀀스를 사용합니다.
첫 조각부터 2초 안에 메시지를 완성해야 합니다. 순서·길이가 잘못된 조각은 거절합니다.
화면을 넘는 다섯 번째 행이나 지원하지 않는 문자는 출력하지 않습니다.

ACK 형식은 다음과 같습니다.

```text
[version=1, type=2, seq_low, seq_high, status]
```

상태는 `0=OK`, `1=BAD_TEXT`, `2=OLED_ERROR`, `3=BUSY`입니다.
central은 ACK notification을 먼저 구독하고, TEXT 조각들을 write-with-response로 전송한 뒤
동일 시퀀스의 ACK를 확인합니다. ACK 0은 OLED 드라이버 처리 성공을 뜻합니다.
실제 화면에 글자가 보이는지는 별도로 확인해야 합니다. 재전송 시 동일 메시지가 다시
표시될 수 있으며 중복 제거는 구현하지 않았습니다.

프레임 생성·조립·ACK 파싱은 [shared/ble_display_protocol.h](shared/ble_display_protocol.h)를
사용할 수 있습니다. 화면 예시는 다음과 같습니다.

```text
NU87 -> NU40
Count: 12
Uptime: 26s
BLE link ready
```

## 검증

2026-10-10 NU40 실기에서 OLED 초기화, BLE 초기화와 광고 시작을 확인했으며
`WAITING FOR NU87` 글자도 확인했습니다. NU87과의 메시지 교환 및 수신한 데이터의
OLED 갱신은 아직 실기 검증 중입니다.

프로토콜의 잘못된 조각, 순서·시퀀스, 길이, 시간 제한·시계 wrap과 화면 배치를 검사하는
호스트 테스트 13개가 통과했습니다. Python과 호스트 C 컴파일러로 실행합니다.

```bash
python3 -m unittest discover -s tools -p 'test_*.py' -v
```

Zephyr 빌드 시나리오는 `nu40_ble_display/testcase.yaml`에 있습니다.
빌드 산출물과 UF2 바이너리는 저장소에 포함하지 않습니다.

## 라이선스

앱과 공통 프로토콜은 [Apache-2.0](LICENSE)입니다.
6×8 글꼴은 Adafruit GFX의 classic ASCII font를 사용하며,
별도 [BSD 라이선스](nu40_ble_display/LICENSE.font)와 원본 저작권 고지를 유지합니다.
