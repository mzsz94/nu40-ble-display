# NU40DK-KIT 단독 진단 앱

OLED·큰 키 4개·RGB 4개·PDM 마이크·I²S 스피커·microSD 배선을 점검하는
초기 진단용 Zephyr 앱입니다. 현재 사용하는 BLE·녹음·SD 파일·OTA 통합 앱은
[nu40_ble_display](../nu40_ble_display/README.md)입니다.

단독 진단 앱의 시작 주소는 `0x1000`입니다. MCUboot를 설치한 보드에는 통합 앱의
서명 OTA 업데이트를 사용합니다. 이 진단 UF2를 기록하면 해당 영역의 MCUboot도
교체하므로 OTA 구성의 일반 업데이트 이미지로 사용할 수 없습니다.

## 빌드와 초기 배선 검사

West·Zephyr SDK 환경에서 이 저장소 루트에서 실행합니다.

```sh
west build -b nucode_nu40/nrf52840 kit_selftest -d build/kit-selftest
python3 tools/kit_flash.py --capture-seconds 20 --serial-number ACTUAL_USB_SERIAL
```

플래셔는 Windows + WSL용이며 `BARAM-NU40`, SoftDevice 없음과 이미지 경계를
확인한 후 UF2를 복사합니다. 다른 부트로더 배치의 보드에는 적용하지 않습니다.
기존 `CURRENT.UF2`는 `build/kit-flash/`에 보관합니다. 업로드 없이 로그만 수집하려면
`python3 tools/kit_selftest_capture.py --serial-number ACTUAL_USB_SERIAL`을 실행합니다.

## 진단 동작

| 큰 키 | 동작 |
| --- | --- |
| 1번 | OLED 키 상태 |
| 2번 | 마이크 블록·peak/RMS |
| 3번 | 명목 1 kHz/300 ms 비프, 진폭 4096 |
| 4번 | 상태 요약 |

USB 제품명은 `NU40DK KIT selftest v3`, 콘솔은 115200 baud입니다.
명령은 `status`, `beep`, `uf2`입니다. RGB는 대기 시 파랑, 키 입력 시 초록으로
표시합니다. 마이크는 16 kHz/16-bit/mono PCM을 수신하고 약 2초 단위로 음량을 집계합니다.
SD는 active-high 카드 감지 후 섹터 0 하나를 읽습니다. 파일 마운트·쓰기·포맷은 하지 않습니다.

로그 수집 성공은 화면 표시나 청음 확인을 대신하지 않습니다. 실제 화면·RGB·비프와
마이크 소리 반응을 별도로 확인합니다.

## 초기 실기 기록

2026-10-10 `BARAM-NU40-DEVKIT`, 공장 UF2 `60c185b`에서 firmware 2와 3의
업로드·부팅을 확인했습니다. 네 키 입력 `seen=0xf`, OLED 글자, RGB 색 변화와
firmware 3의 세 번째 키 비프를 사용자 관찰로 확인했습니다. SD 섹터 0 읽기와
마이크 PCM 블록 진행·오류 0을 관측했습니다. 당시 마이크 음질·파일 저장은 검증하지 않았습니다.
이후 설치된 0.1.2 통합 앱의 녹음·재생·파일 저장 결과는
[KIT 실기 결과](../docs/kit-hardware-results.md)에 있습니다.

핀 배치는 [설계자 KIT 예제](https://github.com/chcbaram/nu40dk-kit/tree/main/firmware/09_ws2812/nu40dk-kit-ws2812)와
[SD 예제](https://github.com/chcbaram/nu40dk-kit/tree/main/firmware/05_spi_sdcard/nu40dk-kit-spi)를 따릅니다.
