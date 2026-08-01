# UxPlay Windows 작업 인수인계

브랜치: `windows-tray` (fork: `origin`) — `macos-airplay-ux`에서 갈라져 나왔고 그쪽 수정을 병합해 따라간다.
대상: Windows 10 22H2 / NVIDIA RTX 2080 / MSYS2 UCRT64 GStreamer 1.28.5

macOS 쪽 내용은 `HANDOFF.md`에 있다. 이 문서는 Windows에서만 해당되는 것만 적는다.

## 빌드와 실행

```sh
# MSYS2 UCRT64 툴체인이 PATH에 있어야 한다
cmake --build build
build/uxplay.exe -n UxPlay-Tray -hls -nohold -d
```

- **포트는 실행할 때마다 바뀐다.** 클라이언트는 mDNS로 찾으므로 문제가 없지만, 사람이 `netstat`이나 직접 접속으로 확인할 때는 매번 다시 봐야 한다. 이전 실행의 포트로 찔러보고 "연결 거부"를 서버 이상으로 읽는 실수를 하기 쉽다.
- 콘솔 창은 `-d`가 있을 때만 나온다. 트레이가 인터페이스이고, 디버그 로그가 갈 곳이 있을 때만 콘솔이 제 몫을 한다.
- 테스트 전 이전 인스턴스를 반드시 정리할 것. 둘 이상이면 로그와 동작이 뒤섞인다.
- 이 PC에서는 **WARP를 끊어야** iPad가 광고를 본다. 방화벽에 Block 규칙은 없다.

## 배포본 만들기

`dist/UxPlay-Windows/`에 자립형 폴더를 만든다. MSYS2가 설치되지 않은 PC에서도 그대로 돈다.

1. `build/uxplay.exe`
2. `ucrt64/lib/gstreamer-1.0/*.dll` (플러그인 241개)
3. `ucrt64/libexec/gstreamer-1.0/gst-plugin-scanner.exe`
4. 위 전부의 의존 DLL을 `objdump -p`로 재귀 수집해 함께 (207개)
5. `UxPlay.bat` — 폴더 기준으로 `GST_PLUGIN_SYSTEM_PATH`, `GST_PLUGIN_PATH`, `GST_PLUGIN_SCANNER`를 잡고 실행

약 230MB. 검증은 **PATH에서 MSYS2를 완전히 뺀 채** 실행해 볼 것 — 빠뜨린 DLL은 그때만 드러난다.

## 이 브랜치가 Windows에 더한 것

- **트레이 상태 아이템** (`renderers/windows_tray.{c,h}`) — 맥 메뉴바 아이템과 같은 API. 클라이언트 이름·상태, 볼륨, 곡 정보, Display 선택, `Fullscreen on connect`, `Console window`, Disconnect, Quit
- **화면 위 재생 패널** (`renderers/windows_osd.{c,h}`) — cairooverlay로 그린다. 볼륨, `|◀ ◀◀ ▮▮ ▶▶ ▶|`, 전체화면, 스크러버, 닫기 버튼
- **키** — 창 프로시저에서 받는다. `←/→` 10초 이동, `↑/↓` 볼륨 1/16, `Enter/Esc` 전체화면 진입·해제, `Space` 재생·정지
- **이전/다음 항목** — `dacp_resolve()`를 최소 mDNS SRV 조회로 대체해 `HAVE_DNS_SD` 없이 동작하게 했다. 번들 `mdnsd`는 등록만 되고 browse/resolve가 없어 이 경로가 통째로 빠져 있었다. 실기 확인: 양방향 모두 `dacp_remote: {next,prev}item -> OK` 뒤에 앱이 `playlistRemove` → `/stop` → 새 `/play`로 응답

## 반드시 알아야 할 것

### playsink이 붙이는 변환기가 재생성된 파이프라인을 죽인다 (해결)

**증상.** 비디오 창을 닫고 다시 연결하면 두 번째 영상이 재생되지 않는다. 오디오는 정상, 로그에 에러 0건, `videosink`가 READY에 머문다. 프로세스의 **첫 HLS 파이프라인만** 재생되고 그 뒤는 코덱과 무관하게 전부 실패한다.

**원인.** OSD는 playbin의 `video-filter`로 들어가는데, playsink은 필터가 있으면 그 앞뒤로 `conv`/`scale`을 덧붙인다. 그것들이 링크되며 올려보낸 `reconfigure`가 재생성된 파이프라인에서 하드웨어 디코더 협상 도중에 도착한다. 그러면 `gst_d3d12_decoder_negotiate`가 두 번째로 돌면서 `Selected output type`에 도달하지 못하고, 디코더가 프레임을 한 장도 내지 않는다.

**수정.** `GST_PLAY_FLAG_NATIVE_VIDEO`로 playsink이 아무것도 덧붙이지 않게 한다. OSD 빈은 이미 앞뒤로 `videoconvert`를 갖고 있어 부족한 것이 없다. Windows·HLS 파이프라인에만 건다.

**맥에는 이 문제가 없다** — OSD를 싱크가 직접 그려서 `video-filter` 자체가 없다. 그것이 유일한 구조 차이였고, 필터를 빼고 돌려 보는 것으로 원인을 갈랐다 (`UXPLAY_NO_OSD=1`, 진단용으로 남겨 둠).

### `GST_PLAY_FLAG_FORCE_SW_DECODERS`는 playbin3에서 무효

`gst-inspect-1.0 playbin3`에 그대로 적혀 있다: `force-sw-decoders — Force only software-based decoders (no effect for playbin3)`. 실측으로도 플래그 유무와 무관하게 `d3d12h264dec`가 선택된다.

`video_renderer.c`가 이 플래그를 거는 것은 맥의 VideoToolbox 크래시 회피용인데, **playbin3를 쓰는 한 양쪽 플랫폼 모두 걸려 있지 않다.** 하드웨어 디코더를 실제로 배제해야 한다면 `decodebin3` 쪽에서 처리하거나 팩토리 rank를 내려야 한다. 맥 쪽에도 해당된다.

### 창의 X와 OSD 닫기 버튼은 서로 다른 길로 들어온다

- **타이틀바 X** — 창 프로시저의 `WM_CLOSE`. 창 스레드에서 직접이라 파이프라인 상태와 무관하다
- **OSD 닫기 버튼** — 클릭이 싱크의 GstNavigation 이벤트가 되어 파이프라인 버스를 타고 메인루프까지 와야 한다. 게다가 `controls_alpha() > 0`, 즉 컨트롤이 화면에 떠 있을 때만 히트테스트를 한다

둘 다 `uxplay-disconnect`로 합류시켰고(OSD 쪽은 창에 `WM_CLOSE`를 보내 같은 경로로 들어온다), 그 뒤는 동일하다. 다만 **OSD 버튼은 클릭이 감지되는 단계가 아직 파이프라인에 의존한다.** 영상이 멈춘 세션에서는 창의 X만 확실하다 — 남은 작업이다.

### 창 훅은 창이 생기는 즉시 걸어야 한다

`WM_CLOSE`도 키도 전부 우리가 서브클래싱한 창 프로시저에 있다. 이것을 PLAYING 도달 시점에만 걸면, 거기까지 못 간 세션은 싱크의 원래 프로시저를 그대로 쓰게 되어 X가 아무 데도 연결되지 않는다. 재생이 시작되지 않는 세션이야말로 X가 필요한 상황이므로, 훅은 재생 여부와 무관해야 한다.

### d3d12videosink는 창이 닫힌 것을 다음 프레임에서야 안다

`gstd3d12videosink.cpp:1696`의 `Output window was closed` 에러는 `show_frame`에서 나온다. 일시정지 중이거나 항목 사이라 다음 프레임이 없으면 그 에러는 영영 오지 않는다. 맥의 싱크와 발화 조건이 다르므로, **이 에러에 기대어 창 닫힘을 감지하면 안 된다.**

## 진단에 쓴 것

- `UXPLAY_NO_OSD=1` — OSD 필터를 비디오 경로에서 뺀다. 비디오 경로가 의심스러울 때 맥과 같은 모양으로 돌려 보는 용도
- `GST_DEBUG_FILE`로 GStreamer 로그를 따로 받을 것. uxplay 자신의 로그와 섞으면 둘 다 못 읽는다
- 프레임이 사라지는 위치를 가를 때 쓴 카테고리: `souphttpsrc:5,hlsdemux2:5,adaptivedemux2:5` (데이터가 오는가) → `videodecoder:5,d3d12decoder:5` (디코더가 내는가) → `GST_PADS:4,GST_CAPS:4` (협상이 끝나는가)
- 로그는 계속 커지므로 **스냅샷을 떠서 고정된 사본으로 분석할 것.** 줄 번호가 흔들리면 판정이 어긋난다

## 남은 작업

- OSD 닫기 버튼의 클릭 감지를 파이프라인에서 떼기
- 하드웨어 디코더를 실제로 배제할지 결정 (위 `force-sw-decoders` 참조)
- 배포본 만들기를 스크립트로 (지금은 수동 절차)
