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

## 유튜브 앱의 진행 바가 갱신되지 않음 — 해결 (2026-07-31)

전진/백 양방향, 끝으로 시크까지 사용자 실기 확인. 영상 종료를 클라이언트에 알리는 건은 아래에
미해결로 남겼다.

**원인.** `rate`는 클라이언트가 위치 점프를 받아들이는 허가다. `hls_seek_resuming`이 시크 정착
구간 내내 `rate 1.0`을 고정 발신하고 있었고, 그래서 클라이언트가 받은 것은 "정상 속도 재생
중인데 시간이 뒤로 갔다"는 불가능한 보고였다. 전진은 증가라 모순이 없어 통과하고, 백은
폐기된다. 폐기 후 클라이언트 시계는 우리보다 앞선 채 남으므로 이후 전진 보고도 계속 폐기된다.
클라이언트 자신의 시크가 항상 됐던 이유는 그쪽이 `rate=0`을 먼저 보내 불연속을 미리 알기
때문이다.

**수정** (`video_get_playback_info_locked`). 시크 정착 중에는 파이프라인 실제 상태를 보고한다 —
PLAYING 도달 전까지 `rate 0`, 그 뒤 `1`. 클라이언트가 받는 순서는 `1 → 0 → 1`로, 자기 시크에서
스스로 만드는 것과 같은 모양이다. 시크 밖의 리버퍼링 완충은 그대로 두어 일반 정체는 여전히
정지로 읽히지 않는다. 이것은 이전 세션이 "0을 보내면 클라이언트가 정지로 latch된다"며 넣었던
가림을 되돌린 것이다. **그 가림 자체가 원인이었다.**

**확인된 범위.** 일반 시크 11회 양방향 — 로그에 `rate 0.000000`이 시크 창마다 나가고 새 위치에서
1로 복귀, 사용자 실기 확인.

**끝으로 시크하면 갱신되지 않던 문제 (해결, 사용자 확인).** 위 수정은 클라이언트가 rate의
`0 → 1` 엣지에서 위치를 확정한다는 데 기댄다. 그런데 `video_renderer_seek`의 끝 클램프가
`hls_duration - 1000` **나노초**, 즉 끝에서 1마이크로초였다. 거기 착지하면 즉시 EOS라 파이프라인이
PLAYING에 도달할 수 없고, rate가 1로 돌아오지 않아 엣지가 생기지 않는다. 클램프를 끝에서 **1초**로
바꿔 재생이 실제로 재개되게 했다 (2초 미만 영상은 기존값). 부수적으로 `GST_MESSAGE_EOS`에서
`hls_seek_resuming`을 해제한다 — 그러지 않으면 PLAYING에서만 풀리는 이 플래그가 영영 남아
`rate 0`이 고정된다.

이 플래그에는 여전히 타임아웃이 없다. 시크가 PLAYING에도 EOS에도 도달하지 못하는 경로가 또 있으면
같은 고착이 재현된다. 그런 경로가 있는지는 확인되지 않았다.

### 클라이언트가 준 시작 위치를 잃어버리던 문제 (해결)

항목이 바뀔 때 `/play`의 `Start-Position-Seconds`가 무시되고 0:00부터 재생됐다. 측정: 18:31.8과
1:57.4를 요청받고 둘 다 0:00.5부터 시작.

- **교체될 파이프라인이 요청을 지웠다.** 버스 콜백의 "이미 지났으면 시크 불필요" 판정이 아직
  살아있는 이전 파이프라인의 위치를 읽었다. 이전 항목은 18분대, 새 시작 위치는 1:57 →
  `pos >= target` 참 → 폐기. 재초기화 후 시크 시도가 0회였다.
  → `video_renderer_set_start()`에서 `hls_seek_enabled = FALSE`. 나가는 파이프라인의 시크 가능
  범위는 그 항목의 것이므로 거둔다. 새 파이프라인이 자기 seeking 질의에 답하면 다시 켜진다.
- **올라오는 중에 건 시크가 preroll에 덮였다.** 시크가 성공을 반환하고도 항목이 0부터 재생됐고,
  직후 PAUSED→PLAYING이 한 번 더 왔다. 성공 반환 시 목표를 즉시 지워서 재시도가 없었다.
  → 성공만으로 지우지 않는다. 위치가 실제로 목표에 1초 이내로 도달했을 때만 지운다. 무한 재시도
  방지로 12회 상한.
- `GST_SEEK_FLAG_KEY_UNIT` → `ACCURATE`. 스크럽에서 5~12초 언더슈트를 만들던 그 플래그가 여기에도
  남아 있었다.

시크가 매번 상한(12회)까지 가는 것은 프리롤 중 위치 질의가 도달을 알려주기 전에 재시도가 도는
탓이다. 결과는 정상이지만 불필요한 flushing seek이 나간다 — 다듬을 여지.

### 이전/다음 항목 (`|◀ ◀◀ ▮▮ ▶▶ ▶|`)

**프로토콜.** AirPlay HTTP 채널에는 없다. 클라이언트가 보내는 재생목록 트래픽은 `playlistRemove`
뿐이고 그것도 방향이 반대다(`playlistInsert`는 세션 전체에서 0회, 핸들러도 `FIXME: not yet
implemented`). 경로는 **DACP** 하나다:

- 클라이언트가 첫 RTSP 요청에 `DACP-ID`와 `Active-Remote`를 실어 보낸다 (`GET /info`, 세션당 49회)
- 클라이언트가 `iTunes_Ctrl_<DACP-ID>._dacp._tcp`를 광고한다
- 해석해서 `GET /ctrl-int/1/{previtem,nextitem}` + `Active-Remote` 헤더

실기 확인: `nextitem` → HTTP 200 → 유튜브 앱이 `playlistRemove` → `stop` → 새 `/play`로 응답.

**구현.** `lib/dacp_remote.{c,h}` 신규. `DNSServiceResolve`(3초 타임아웃) 후 HTTP GET, detached
스레드에서 — 둘 다 블로킹인데 호출자가 UI 스레드다. 번들 `mdnsd`는 등록만 가능하고 browse/resolve가
없으므로 `HAVE_DNS_SD`로 감쌌다 (CMakeLists가 dns_sd 백엔드일 때 정의). 없으면 명령이 경고만 남기고
컴파일은 통과한다.

**이 경로는 신호만 보낸다.** 파이프라인도 렌더러 상태도 리버스 이벤트도 건드리지 않는다. 이후는
클라이언트가 `playlistRemove → stop → play`로 몰고 가고 평소의 `/play` 경로를 탄다.

**패널이 넓어졌다.** 상단 행 왼쪽은 볼륨(히트 영역이 행 왼쪽 끝에서 162까지), 오른쪽은 전체화면
버튼으로 고정이라, 버튼 5개를 그 사이에 넣으려면
`2 × (162 + TRANSPORT_HIT/2 + 2 × TRANSPORT_SPACING) = 550`이 필요하다. `PLAYBACK_BAR_MIN_WIDTH`
360 → 560, `MAX_WIDTH` 520 → 600, 간격 52 → 48. 겹치면 히트 테스트가 볼륨을 먼저 보므로 버튼이
죽는다. 부작용: 창 폭 632px 미만이면 패널이 안 뜬다.

### 메뉴바에서 모니터 선택

메뉴바 메뉴에 `Display` 서브메뉴. 번호 + `localizedName` + 해상도, 현재 선택에 체크. 모니터가
1대면 항목을 숨긴다.

- **메뉴를 열 때마다 목록을 다시 만든다** (`menuWillOpen:`). 실행 중에 케이블을 뽑거나 뚜껑을 닫으면
  시작 시점에 만든 목록은 없는 화면을 제시한다. 인덱스도 매번 범위를 확인하고, 벗어나면 "원래 있던
  화면"으로 되돌린다
- 경로: 메뉴 → `statusbar_set_display_handler` → `video_renderer_set_display()` → 싱크의 새
  `display-index` 속성 → `performSelectorOnMainThread`로 `setDisplayIndex:`
- 의사 전체화면 중이면 새 화면을 덮도록 프레임을 바꾸고, 아니면 크기를 유지한 채 그 화면
  `visibleFrame` 가운데로 놓는다. `enterPseudoFullScreen`도 이제 선택된 화면을 쓴다 (전에는 창이
  얹혀 있던 화면)
- 창은 세션마다 새로 만들어지므로 `video_renderer_start()` 뒤에 선택을 다시 적용한다 (3곳).
  선택 전에는 `-1`이라 기존 동작 그대로

### 미해결: 영상이 끝났다는 것을 클라이언트에 알릴 방법

EOS 시 `video_eos_watch_callback`은 `commanded_rate`를 0으로 두고 세션만 유지하며, **클라이언트에게
아무것도 보내지 않는다.** 그 뒤로는 폴링마다 "위치는 끝, rate 0"만 반복 응답한다. 클라이언트는
항목이 끝났다는 것을 알 수 없다.

거기서 `raop_announce_playback_stopped()`(리버스 채널 `stopped`)를 보내봤다 — **앱이 검은 화면을
내고 이후 아무것도 재생되지 않았다. 즉시 되돌렸다.** 클라이언트는 `stopped`를 세션 종료로 받는다.
영상의 끝은 세션의 끝이 아니다(뒤로 스크럽하거나 다음 항목을 넣을 수 있다). 무엇으로 알리는지는
아직 모른다 — `stopped`는 아니다.

### 이 조사에서 얻은 교훈 (같은 함정 반복 방지)

- **"전진은 된다"가 가장 중요한 단서였는데 늦게 읽었다.** 전진이 리버스 이벤트 0회로 잘 됐다는
  것은 클라이언트가 `/playback-info`의 position을 그대로 따라온다는 뜻이고, 그러면 통보 경로
  부재·`loadedTimeRanges`·버퍼 플래그·상태 이벤트는 전부 후보에서 빠진다. 남는 것은 값이
  작아질 때만 거부되는 이유 하나뿐이다.
- **깨진 뒤에 뜬 숫자를 정상 동작의 근거로 쓰지 말 것.** 재생 중 `/play`의
  `Start-Position-Seconds`가 우리 위치와 104초 어긋난 것을 "클라이언트는 원래 position을 따라오지
  않는다"의 증거로 썼다가 방향을 통째로 잃었다. 그 값은 이미 멈춘 뒤의 상태였다.
- **추측을 수정이라고 내보내고 검증을 사용자에게 넘기지 말 것.** `loading` 이벤트,
  `loadedTimeRanges`, `paused` 이벤트를 차례로 넣었고 전부 틀렸다.
- **로그를 먼저 볼 것.** 이 결함은 코드 독해로 여섯 번 헛짚었고, 매번 로그가 그 가설을 즉시
  반박할 수 있었다. 특히 클라이언트가 실제로 무엇을 보내는지부터 세라:
  `grep -oE "^(GET|POST|PUT) [^ ]+" log | sort | uniq -c | sort -rn`.
  이걸로 `POST /scrub`이 세션당 1회뿐이고 나머지 시크는 전부 화면 패널에서 온다는 것을 알았다.

### 아직 검증되지 않은 채 남은 변경

동작하는 현재 상태에 함께 들어 있으나 이번 결함의 원인은 아니었다. 하나씩 떼어내며 확인할 것:

- `loadedTimeRanges`를 `seekableTimeRanges`와 같은 `0 → duration`으로 (원래는 `position → 끝`)
- `raop_announce_seek()` — 수신기측 시크 시 리버스 채널로 `paused` 발신
- `/scrub` 핸들러가 더 이상 `playing`을 발신하지 않음
- `/playback-info`가 `rate > 0`일 때 `playing` 발신

### 함께 고친, 독립적으로 확인된 결함

- `playback_state_event`가 리버스 소켓을 찾기 **전에** `last_state`를 찍어, 전송되지 않은 상태가
  전송됨으로 기록되고 이후 같은 상태가 영구히 중복 처리됐다. 전송 성공 후에만 기록하도록 수정.
  세션 간에 살아남던 것도 `POST /play`에서 리셋
- `GST_MESSAGE_BUFFERING`의 버퍼 플래그가 `percent > 0` 가드 안에 있어 0% 메시지 하나로 고착될 수
  있었다. 매번 레벨에서 유도하고 PLAYING 도달 시 해제
- `video_get_playback_info_locked`가 `renderer == NULL` 조기 반환 전에 `buffer_empty`/`buffer_full`을
  채우지 않아 초기화 안 된 스택 값이 plist에 실렸다
- `readyToPlay`·`playbackBufferEmpty`·`playbackBufferFull`·`playbackLikelyToKeepUp`을 `plist_new_bool`로
- **화면 패널 시크바 클릭이 무시됐다** (`cocoawindow.m`). `controlsAtPoint:begin:YES`가
  `scrubbing`만 세우고 `osdPosition`은 `mouseDragged`에서만 갱신돼, 드래그 없는 클릭은 원래 위치로
  시크했다. 볼륨 슬라이더도 같았다. 누른 지점을 즉시 적용하도록 수정
- **드래그 중 노브가 되돌려졌다.** `setPlaybackPosition:`(초당 1회 폴링)이 드래그가 쓰는 같은
  `osdPosition`을 덮어썼다. 스크러빙 중에는 무시

### 당시 남겨둔 기록 (해결 전, 참고용)

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

**원인을 로그에서 확인했다** (2026-07-31). 앱이 직접 시크한 구간의 인과 사슬:

```
POST /scrub?position=822.154        앱이 시크
Buffering :0 percent done           플러싱 시크가 버퍼를 비움
playback_info: PAUSED rate 1.0 buffer_empty 1    ← 앱에 "버퍼 비었음"을 보냄
POST /rate?value=0.000000           앱이 그걸 stall로 보고 스스로 일시정지
playback_state_event: sent state=paused
Buffering :100 percent done         버스 핸들러가 무조건 PLAYING으로 되돌림
playback_info: PLAYING rate 1.0     위치는 계속 진행
```

앱은 자기가 정지시켰다고 믿는데 우리는 몰래 재생을 재개한다. 앱은 다시 `rate=1`을 보낼 이유가 없으니 진행 바가 영구히 멈춘다. 짧은 전진 시크는 버퍼 안이라 `Buffering :0`이 안 뜨고, 뒤로 시크는 항상 버퍼를 비운다 — 방향 비대칭이 여기서 나온다. 폴링이 시크와 PLAYING 도달 사이의 창을 비껴가면 같은 시크도 멀쩡하다.

**들어간 수정:**

1. **시크가 정착하는 동안 `playbackBufferEmpty`를 보내지 않는다** (`hls_seek_resuming`이면 억제). 우리 자신이 낸 플러싱 시크로 빈 버퍼는 클라이언트가 알 일이 아니고, 시크 내내 일부러 보내는 `rate 1.0`과도 모순이다. 일반 재생 중의 stall은 그대로 보고한다
2. **버퍼링 100%에서 `hls_commanded_rate > 0`일 때만 재생을 재개한다.** 무조건 PLAYING으로 올리는 바람에 클라이언트의 명시적 `rate=0`을 덮어썼다. 시크는 스스로 commanded rate를 1로 놓으므로 시크 후 재개 경로는 그대로 산다
3. `readyToPlay`·`playbackBufferEmpty`·`playbackBufferFull`·`playbackLikelyToKeepUp`을 `plist_new_bool`로 (위 표의 최우선 항목이었다)
4. `GST_MESSAGE_BUFFERING`에서 버퍼 플래그를 `percent > 0` 가드 밖으로 꺼내 매번 레벨에서 유도하고, hls-playbin이 PLAYING에 도달하면 푼다. **영구 래치는 실제로 관측되지 않았다** (로그에서 0→100이 항상 완료됨) — 다만 창을 좁혀 준다
5. `video_get_playback_info_locked`가 `renderer == NULL` 조기 반환 전에 `buffer_empty`/`buffer_full`을 채우도록. 초기화 안 된 스택 값이 plist에 실렸다

**아직 미검증.** 실기 확인 필요. 확인 포인트: 뒤로 시크 후 `POST /rate?value=0`이 **더 이상 오지 않는지**. 그게 오면 1번이 부족한 것이고, 안 오는데도 바가 멈추면 원인이 또 다르다.

**측정 방법 메모.** `-d 1`로 띄우고 로그를 파일로 받은 뒤:
`grep -oE "^(GET|POST|PUT) [^ ]+" log | sort | uniq -c | sort -rn` 로 클라이언트가 실제로 뭘 보내는지부터 센다. 이번에 `POST /scrub`이 세션 전체에서 1회뿐이고 나머지 12번의 `SCRUB: seek to`는 전부 화면 패널(`uxplay.cpp:770/784/821`)에서 나왔다는 걸 이걸로 알았다.

**남은 것.** 화면 패널 위치를 파이프라인에서 직접 갱신 (앱과 무관한 별개 결함), 그래도 남으면 실제 수신기의 `/playback-info` 응답과 리버스 이벤트를 캡처해 바이트 단위로 비교.

## 주의할 점

- **전역 `renderer`는 여러 스레드가 만진다.** httpd 스레드(`/playback-info`, `/scrub`)와 GLib 메인루프(EOS, reset)가 동시에 접근한다. `renderer_lock`(재귀 뮤텍스)을 거치지 않는 새 접근을 추가하지 말 것. `video_renderer_destroy_instance`의 매개변수를 다시 `renderer`로 되돌리면 전역이 가려져 해제된 포인터가 남는다
- **EOS는 세션의 끝이 아니다.** 클라이언트가 연결돼 있으면 파이프라인을 파괴하지 않고 PAUSED로 둔다. 버스를 flush하면 이후 버퍼링 메시지가 사라져 시크가 PAUSED에 갇힌다
- **UYVY에서 0으로 채운 버퍼는 검정이 아니라 녹색이다.** 텍스처를 새로 잡을 때 지우지 말고 이전 텍스처를 새 프레임이 올 때까지 계속 그린다
- **OSD가 텍스처를 구울 때 GL 언팩 상태를 바꾸면 영상 업로드가 망가진다.** 들어올 때 `glGetIntegerv`로 저장하고 나갈 때 그대로 복원할 것. 추정값으로 되돌리지 말 것
- **커스텀 뷰를 NSMenu에 얹을 때** vibrancy를 허용하지 않으면 배경이 초기화되지 않은 채 남는다
- 손쉬운 사용 권한이 없으면 `osascript`로 메뉴바 메뉴를 열 수 없다. UI 확인은 사용자 스크린샷에 의존한다

## 남은 작업

- 영상 종료를 클라이언트에 알리는 방법 (위 "미해결" 참조). `stopped`는 아니다 — 앱이 검은 화면을 낸다
- 검증되지 않은 채 남은 변경 4건을 하나씩 떼어내며 확인 (위 참조)
- 메뉴바 메뉴는 육안 검증을 한 번도 하지 못했다. 진행/시크 행이 실제로 어떻게 보이는지 확인 필요
- 재생 패널 위치를 세션 간 기억할지
- 앨범 아트, 참고 UI 오른쪽의 `»` 자리 기능
- `sudo make install` / 실행 래퍼 스크립트
- 상류 보고: osxvideosink의 회전 크래시와 티어링 두 건
