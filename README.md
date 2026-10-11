# NU40DK-KIT BLE · 오디오 · SD · OTA

Military Wallaby 작업 앱은 [`firmware/nu40_voice`](firmware/nu40_voice/README.md)입니다.
현재 소스는 `0.2.2`이며 실측 IMU 얼굴·수평계, BTN2 녹음·BTN3 전송과 저장된 BLE
구독의 재연결 처리를 포함합니다. 이 구성의 HostPC·NU87 코드는
[micro-wallaby 작업 브랜치](https://github.com/mzsz94/micro-wallaby/tree/feat/military-wallaby-oled-graphics)에
있으며 두 보드 모두 Zephyr를 사용합니다. [그래픽·실기 안내](docs/military-oled.md)를 참고하세요.

NU40DK-KIT의 모든 내장 입출력을 사용하는 Zephyr 앱입니다. OLED 문자열·움직이는 얼굴,
버튼 4개, RGB 4개, 마이크 음량·2초 녹음, 스피커 비프·재생, microSD WAV 저장·대조를
통합합니다. NU87을 통한 BLE 펌웨어 업데이트와 원격 KIT 제어를 지원합니다.

- [키 사용법·기능·빌드](nu40_ble_display/README.md)
- [OTA 최초 설치·서명 키·업데이트](ota/README.md)
- [단독 KIT 진단 앱](kit_selftest/README.md)
- [NU87 중계 앱](https://github.com/mzsz94/micro-wallaby/tree/feat/nu87-nu40-kit-ota/firmware/nu87_nu40_bridge)

기존 전체 KIT 예제에서는 NU40은 Zephyr, NU87 중계 앱은 AmebaD Arduino 3.1.9를 사용합니다.
기존 IMU 센서 앱과는 별도 기능이며 이 키트 자체에는 IMU가 포함되지 않습니다.

## 개발 환경

검증 기준은 Zephyr `e70694102ad6f910485155a824c5483daf605a9f`와 SDK 1.0.1입니다.
기존 West 워크스페이스에서 이 저장소를 Zephyr·bootloader의 형제 폴더로 두거나,
다음처럼 새 워크스페이스를 만듭니다.

```sh
mkdir nu40-workspace
cd nu40-workspace
git clone --branch feat/nu40-kit-ota https://github.com/mzsz94/nu40-ble-display.git
west init -l nu40-ble-display
west update
west zephyr-export
cd nu40-ble-display
python3 -m pip install -r tools/requirements.txt
python3 -m pip install -r ../bootloader/mcuboot/scripts/requirements.txt
```

Zephyr의 Python 의존성과 SDK도 설치된 환경이 필요합니다. 다른 위치의 MCUboot를
사용하면 `MCUBOOT_DIR=/absolute/path/to/bootloader/mcuboot`를 지정할 수 있습니다.
부트로더 배치가 다른 NU40에서는 공장 부트 정보부터 확인합니다. 이 프로젝트의
검증 보드는 `BARAM-NU40`, SoftDevice 없음이며 공장 UF2 복구 영역을 보존합니다.

## 검증과 라이선스

앱·프로토콜·호스트 도구는 Apache-2.0입니다. 글꼴의 별도 BSD 고지는
[LICENSE.font](nu40_ble_display/LICENSE.font)에 보존합니다.
호스트 검사 171개와 Zephyr build-only 시나리오 1개가 통과했습니다.
NU40 0.1.2의 [실기 검증 결과](docs/kit-hardware-results.md)와
[OTA 안내](ota/README.md)에 검증 범위와 실패 기록을 구분합니다.
개인 키, SDK, 생성 펌웨어 바이너리와 원본 연속 로그는 저장소에 포함하지 않습니다.

Military Wallaby의 실제 IMU와 연동하는 얼굴·수평계 OLED 화면은
[그래픽 안내](docs/military-oled.md)를 참고하세요.
