# UxPlay macOS 작업 인수인계

브랜치: `macos-airplay-ux` (fork: `origin`)
대상: macOS 15.7 / Intel / GStreamer 1.28.5 (Homebrew) / 논-Retina 2560×1440

## 빌드와 실행

```sh
make -j$(sysctl -n hw.ncpu)
script -q /dev/null ./uxplay -hls -d 1 \
  -vs "uxvideosink force-aspect-ratio=true start-fullscreen=true hide-until-stream=true max-lateness=-1 qos=false"
```

- 로그를 파이프로 넘기면 `script`가 `tcgetattr/ioctl` 오류로 죽는다. 리다이렉트 없이 실행하고 출력 파일을 따로 읽을 것.
- 테스트 전 `pkill -9 -f uxplay`로 이전 인스턴스를 반드시 정리한다. 인스턴스가 둘 이상이면 로그와 동작이 뒤섞여 판단이 전부 어긋난다.

## 이 브랜치가 더한 것

### 벤더링한 videosink (`renderers/uxvideosink/`)

GStreamer의 osxvideosink를 패치해 `uxvideosink`로 정적 링크한다. 별도 플러그인 설치가 필요 없다.

- 회전 시 SIGSEGV — `showFrame:`에서 원본은 매핑된 프레임으로, 대상은 실제 텍스처 크기로 각각 제한
- 회전 복귀 시 티어링 — 크기가 바뀔 때만 버퍼를 다시 잡는다
- 세로 모드 FPS 붕괴 — QoS 프레임 드롭. `max-lateness=-1 qos=false`로 실행
- 의사 전체화면(pseudo-fullscreen). 네이티브 전체화면은 회전·teardown과 경쟁 상태가 나서 쓰지 않는다
- 화면 위 재생 패널: 볼륨, ◀◀ ▮▮ ▶▶, 전체화면, 스크러버와 경과/전체 시간. 패널 빈 곳을 끌어 옮길 수 있다
- 조작은 GstNavigation 키 경로로 `uxplay-*` 문자열을 되돌려 보내 `video_window_key_pressed()`가 받는다

### 메뉴바 상태 아이템 (`renderers/macos_statusbar.{h,m}`)

클라이언트 이름, 상태, 곡 정보, 볼륨 슬라이더, 진행/시크 슬라이더, Disconnect, Quit.

### 프로토콜·세션

- `/playback-info` 폴링을 생존 신호로 인정 (`9efb394`). HLS 클라이언트는 `/feedback`을 보내지 않아 15초마다 세션이 끊겼다
- `POST /action`·`/setProperty`의 `assert(airplay_video)` 제거 (`d31d404`). 클라이언트 요청 하나로 서버 전체가 abort 됐다
- 싱크를 `GstVideoOverlay` 인터페이스가 아니라 **속성 이름으로** 찾는다 (`9ef54f8`). playbin 안에서는 playsink가 먼저 잡혀 모든 속성 설정이 조용히 버려졌다
- HLS 세션을 상태바에 연결하고, 클라이언트가 떠나면 HLS 파이프라인을 정리한다 (`a1258f8`)

## 미해결: 유튜브 앱의 진행 바가 갱신되지 않음

**증상.** 화면 스크러버로 시크하면 유튜브 앱의 진행 바가 따라오지 않는 경우가 많다. 특히 뒤로 가기. 실제 AirPlay 수신기(Apple TV)에서는 정상이므로 앱 문제가 아니다.

**측정된 사실** (추측 아님, 로그 집계):

- `/playback-info` 응답은 정확하다. 한 세션 203회 중 202회가 올바른 위치·`rate 1.0`, 본문 없는 응답 1회
- 리버스 채널 전송 실패 0회
- 앱은 재생 중이라고 믿는 동안 초당 약 1회 폴링한다
- 리버스 채널로 `state=paused`를 보내면 **앱이 폴링을 멈춘다** — 다음 이벤트까지 0~1회. `playing`을 보내면 재개(12/3/2/2/3/78/64회)

**시도했다가 되돌린 것.** 아래는 전부 현재 트리에 없다. 다시 넣기 전에 이 기록을 읽을 것.

| 시도 | 결과 |
|---|---|
| 시크 전후로 `paused`→`playing` 이벤트 전송 | 전진은 개선, 뒤로 가기는 악화. 드래그 내내 앱이 폴링을 멈춘다 |
| 이벤트 완전 제거 | 전진도 갱신 안 됨 |
| 드래그 끝에서만 전환 전송 | 미검증 상태에서 되돌림 |
| `readyToPlay` 등 4개를 `plist_new_bool`로 (규격상 boolean, 코드에 `should these be int or bool?` 주석) | "많이 줄었다"는 확인 있음. 되돌아간 상태 — **다시 시도할 가치가 가장 높음** |
| `loadedTimeRanges`를 0부터 시작 | 근거 없는 추측 |
| `stallCount` 추가 | 근거 없는 추측 |
| 250ms 타이머로 파이프라인 위치를 직접 싱크에 밀어 넣기 | 화면 패널이 앱 폴링에 얹혀 멈추는 문제는 실재. 함께 되돌아감 |
| 시크 정착 전까지 목표 위치를 보고 | 미검증 |

**다음에 할 일.** 한 번에 하나만 바꾸고 확인한다. 우선순위:

1. boolean 타입 수정만 단독으로 되살려 확인
2. 화면 패널 위치를 파이프라인에서 직접 갱신 (앱과 무관한 별개 결함)
3. 그래도 남으면 실제 수신기의 `/playback-info` 응답과 리버스 이벤트를 캡처해 바이트 단위로 비교

## 주의할 점

- **전역 `renderer`는 여러 스레드가 만진다.** httpd 스레드(`/playback-info`, `/scrub`)와 GLib 메인루프(EOS, reset)가 동시에 접근한다. `renderer_lock`(재귀 뮤텍스)을 거치지 않는 새 접근을 추가하지 말 것. `video_renderer_destroy_instance`의 매개변수를 다시 `renderer`로 되돌리면 전역이 가려져 해제된 포인터가 남는다
- **EOS는 세션의 끝이 아니다.** 클라이언트가 연결돼 있으면 파이프라인을 파괴하지 않고 PAUSED로 둔다. 버스를 flush하면 이후 버퍼링 메시지가 사라져 시크가 PAUSED에 갇힌다
- **UYVY에서 0으로 채운 버퍼는 검정이 아니라 녹색이다.** 텍스처를 새로 잡을 때 지우지 말고 이전 텍스처를 새 프레임이 올 때까지 계속 그린다
- **OSD가 텍스처를 구울 때 GL 언팩 상태를 바꾸면 영상 업로드가 망가진다.** 들어올 때 `glGetIntegerv`로 저장하고 나갈 때 그대로 복원할 것. 추정값으로 되돌리지 말 것
- **커스텀 뷰를 NSMenu에 얹을 때** vibrancy를 허용하지 않으면 배경이 초기화되지 않은 채 남는다
- 손쉬운 사용 권한이 없으면 `osascript`로 메뉴바 메뉴를 열 수 없다. UI 확인은 사용자 스크린샷에 의존한다

## 남은 작업

- 유튜브 앱 진행 바 동기화 (위 참조)
- 메뉴바 메뉴는 육안 검증을 한 번도 하지 못했다. 진행/시크 행이 실제로 어떻게 보이는지 확인 필요
- 재생 패널 위치를 세션 간 기억할지
- 앨범 아트, 참고 UI 오른쪽의 `»` 자리 기능
- `sudo make install` / 실행 래퍼 스크립트
- 상류 보고: osxvideosink의 회전 크래시와 티어링 두 건
