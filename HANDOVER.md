# 인수인계: 野々村病院の人々 (NONOXP) — New 3DS 네이티브 포트 + 한글판 + 입체 3D

제작자 표기(배포 시 반드시 유지): **제작자 : 디시인사이드 스위치CFW 갤러리 -루카-**

이 문서는 다음 작업자(사람 또는 AI)가 바로 이어서 최적화·수정할 수 있도록 현재 상태, 구조, 빌드·테스트 방법, 지켜야 할 약속을 정리한 것입니다. 세부 포맷·역공학 기록은 `docs/FORMATS.md`(1~33단계 작업 일지)에 있습니다.

---

## 1. 한눈에 보기

| 항목 | 내용 |
|---|---|
| 원본 | elf 「野々村病院の人々」 Windows XP판(2003), 엔진 AI5WIN |
| 방식 | 에뮬레이터가 아니라 **AI5WIN 스크립트 VM을 C로 다시 구현**한 네이티브 포트(.3dsx) |
| 대상 기기 | **New 3DS**(사용자 기기). 구형 3DS는 속도 보장 안 됨 |
| 언어 | 한국어(사용자가 준 한글 패치 MES.ARC/Bg.Arc/NonoKR.dll 기반) |
| 화면 | 상단 = 게임 그림(입체 3D), 하단 = 대사·선택지·모든 메뉴 |
| 입체 | AI(Depth Anything V2) 깊이 지도 `DEPTH.PAK` + 기기에서 시점 와프 |
| 빌드 | GitHub Actions(devkitARM 컨테이너). 사용자는 **PC가 없음** → 로컬 빌드 불가 전제 |

## 2. 저장소와 브랜치

- 소스: GitHub `hyunwoo597-boop/aistorage-`(대소문자 `AIstorage-`) 브랜치 **`nonoxp-3ds`** (공개 저장소)
- 빌드 결과: push하면 `.github/workflows/build.yml`이 돌아 **`nonoxp-3ds-bin`** 브랜치에 `nonoxp.3dsx`, `nonoxp.smdh`를 강제 push. 커밋 메시지 `build <전체 SHA>`로 어느 소스인지 확인.
  - 받기: `git fetch origin nonoxp-3ds-bin && git show FETCH_HEAD:nonoxp.3dsx > nonoxp.3dsx`
- 깊이 지도 생성: **비공개** 저장소 `hyunwoo597-boop/test`, 브랜치 `nonoxp-depth`(입력 그림 + 스크립트 + 워크플로) → 결과는 `nonoxp-depth-out` 브랜치. 게임 그림이 들어가므로 **공개 저장소에 그림/깊이 결과를 올리지 말 것.** 스크립트 사본은 이 저장소 `tools/depth/`에 있음.

## 3. SD 카드 배치 (`sdmc:/3ds/nonoxp/`)

| 파일 | 출처 |
|---|---|
| `nonoxp.3dsx` | 빌드 결과 |
| `MES.ARC` | 한글 패치의 mes.arc (대본) |
| `BG.ARC` | 한글 패치의 Bg.Arc (그림, 104MB) |
| `DATA.ARC` | 원본 CD (UI 그림·.a6 히트 테이블 등) |
| `VOICE.ARC` | 원본 CD (음성, 그대로) |
| `SOUND.ARC` | 원본 `MUSIC.ARC`(WAV 263MB)를 `tools/mksound.py`로 OGG 변환(18MB) |
| `font16.bin` | `assets/kr/font16.bin` (한글 매핑 글꼴, `tools/mkfont.py`) |
| `patch/` 폴더 | `assets/kr/patch/*` (titlelogo.gcc, oplan.gcc, mwaku.gcc). 같은 이름의 아카이브 항목을 덮어씀 |
| `DEPTH.PAK` | AI 깊이 지도(약 11MB, 아래 6장). 없으면 예전 추정 방식으로 동작 |
| `save/`, `log.txt` | 실행 중 생성 |

원본 CD 820MB 중 IE6·DirectX·복사방지(PROTECT)·설치 프로그램 약 220MB는 불필요, 음악은 OGG로 압축 → 포트는 약 300MB.

## 4. 소스 구조

```
src/            엔진(플랫폼 무관)
  vm.c          MES 바이트코드 VM (명령 00~1F, 식 평가, PROC/LIB 호출)
  sys.c         SYS/UTIL 명령 구현, 그래픽 합성(서피스·DEP 층), 텍스트·백로그·메뉴, 저장/불러오기, DEPTH.PAK 로더
  arc.c         ARC 아카이브(+ patch/ 폴더 덮어쓰기)
  ai5.h         공용 선언 (plat_* 인터페이스, sys_* API)
  gccdec.h      GCC 이미지 디코더(G24/R24, 알파)
platform/3ds/   3DS 전용
  plat_3ds.c    입력, 상·하단 그리기, 입체 3D 전체, 스플래시, main
  snd_3ds.c     ndsp 사운드 (Tremor로 OGG 재생, WAV 음성)
  splash_data.c 시작 MPO 입체 그림(tools/mksplash.py 생성)
  ui_icons.c    하단 "A버튼" 안내 아이콘(tools/mkicons.py)
  ui_toast.c    (현재 미사용) 하단 안내 토스트(tools/mktoast.py)
platform/headless/plat_headless.c   PC 테스트용(화면 덤프·자동 진행·장면 점프)
tools/          포맷·에셋·깊이 도구 (아래 표)
assets/         font16.bin(일본어), kr/(한글 글꼴·매핑·patch 그림), splash/HNI_9001.MPO, icon.png
docs/FORMATS.md 포맷 분석 + 단계별 작업 일지(가장 자세한 기록)
```

주요 도구: `arc.py`(ARC 풀기) · `mesdis.py`(MES 역어셈블) · `lz.py`/`lzss_enc` · `mkfont.py`(한글 글꼴; NonoKR.dll의 SJIS→한글 매핑 `assets/kr/nonokr_map.json`) · `mksound.py` · `mklogo_kr.py`/`mkmenulogo_kr.py`/`patch_sysmenu.py`(한글 UI 그림) · `mksplash.py` · `mkicons.py` · `depth/*`(깊이 지도) · `hosttest/mk_warp_test.py`(PC에서 3D 결과 재현).

## 5. 엔진 핵심 메모 (자세한 건 FORMATS.md)

- 메모리 배치: flag +0x80(8bit), v16 +0x1084, v32 +0x10EC, sys32 +0x1154, m32 +0x2000. `sys32[1]` = 기준+0x100000 스크래치(불러오기 후 BGM 이름 복원에 필요).
- MENUS 100 = 이야기 진행 루프, 0 = 라이브러리 메뉴. `.a6` 히트 테이블은 SYS 8,0으로 로드, 하단 메뉴 레이아웃은 `cur_a6`로 구분.
- 포트 쪽 패치: `sys_fix_script`(LIBLARY 버튼 제거 등), `FLAG 4016 = 1`(START.MES 타이틀 메뉴 표시), SETAC(0x06) 바이트 쓰기 수정, SYS 10,1 크로스페이드, SYS 28(타이틀로/종료 확인).
- 하단 화면: `sys_bottom_rgb`(메뉴·대화상자 배치), `sys_bottom_map`(터치→640 좌표), 갤러리는 `sys_bottom_viewport`(1:1 창 드래그).
- 키: A/터치 = 결정, B = 취소(메뉴), START = 시스템 메뉴, L = 백로그, R(누름) = 스킵, 십자키 = 이동, **왼쪽 슬라이드 패드 = 3D 시차(보기 각도)**, START+SELECT = 종료.

## 6. 입체 3D 파이프라인 (최근 작업의 중심)

### 6-1. 층 값(DEP) — `src/sys.c`
서피스마다 픽셀별 1바이트 층 값을 같이 합성한다.
- `0` 배경(지도 없음) · `1` 스프라이트(지도 없음, 글자 포함) · `2..127` 지도 있는 배경/CG(AI 깊이) · `128..253` 지도 있는 캐릭터.
- `load_image`: DEPTH.PAK에 같은 이름이 있으면 지도(1/4 또는 1/2 해상도, 크기 비율로 자동)를 2..127로 기록. 지도 없는 **부분 그림**은 밑의 지도 깊이를 유지.
- `gfx_blend`(알파, 캐릭터) → `dep_sprite`: 2..127 → +126(캐릭터 범위). 지도 없는 조각(표정·눈·입)은 밑 캐릭터 깊이 유지.
- `gfx_copy_key`(색키, 그림 변형 조각) → `dep_key`: 그림의 일부로 취급(배경 범위 유지). G23A 같은 "의자+인물 사각형" 조각이 네모로 튀어나오던 문제의 해결책.
- `gfx_copy`: 화면 90% 미만의 지도 없는 조각은 밑 깊이 유지.

### 6-2. 깊이 지도 만들기 — `platform/3ds/plat_3ds.c`
`draw_scene()`이 장면 서명(층 값 표본, AI가 없으면 색도 포함)이 바뀔 때만 다시 계산(전환 중엔 4프레임에 1번).
- **AI 전체 그림(CG)**: `build_dm_hi` → 320x240 `g_dmh`.
- **캐릭터 장면**: `build_dm_layers`(160x120 `g_dm`, 셀 단위, 블러 r=3; 배경엔 `bg_curve` S자 대비 곡선) + `build_dm_hi_layers`(외곽 10px 안쪽에만 1/2 해상도 세부 굴곡을 더함).
- 두 고해상도 경로 모두 `soften_steps()`: 13x13 평균 + 차이 ±12단계 제한 → 큰 단차는 경사로, 작은 굴곡은 유지(슬라이더 최대 잔상 방지).
- 지도 없는 전체 화면(안내문·메뉴): DEPTH.PAK이 있으면 평면 한 장. 얇은 글자·로고는 3칸 넓힌 평판(`text` 판정).
- DEPTH.PAK이 없을 때의 예전 추정 경로(`build_dm`, `build_room`, `build_shade`)도 남아 있음.

### 6-3. 강도·화면면 — `depth_stats()` + `draw_scene()` 슬라이더 곡선
- 장면 깊이 분포 3~98%를 예산 전체에 펼침. 주인공 = 화면 중앙 영역의 70% 지점(`g_sub`).
- 슬라이더 32단계 양자화, `e = q^0.7`, 뒤 `9e` px, 앞 `e(2+3q)` px(눈당), 화면면 `g_zp = lo + (sub-lo)(0.55-0.25q)`.
- 왼쪽 슬라이드 패드: `g_tilt`(놓으면 복귀) × 5px 시차, 입체와 합친 이동량은 최대 폭으로 제한.

### 6-4. 와프·그리기 — `make_half()` + `draw_eye_dm()`
- `make_half`: 640x480 → 320x240 평균(크로스페이드 섞기 포함)을 프레임당 1번, 두 눈이 공유. 그림이 같으면 재사용.
- `draw_eye_dm`: 깊이 256단계 이동표(tab) → 역방향 와프. 원천 열을 최대 3회 찾고 답이 갈리면(배경이 드러나는 곳) **먼 쪽**을 택함. 화면 끝은 끝 열 반복(사용자 요청: 화면 크기 건드리지 말 것).
- `depth_fx`(초점 흐림·그림자·공기감)는 **사용자 요청으로 전부 꺼짐**(코드만 남음, `make_half`에서 호출 안 함).

### 6-5. DEPTH.PAK 형식과 생성
- `"DPK1"`, u32 개수, 항목 24바이트 `{char name[16]; u16 w, h; u32 off}`, 데이터 = 8비트(255 = 가장 가까움). 엔진은 이름(대소문자 무시)으로 찾음.
- 현재 내용(172항목): 캐릭터 29장 + CG/배경 121장 = Large 모델 2배율 1/2 해상도(`gen2.py`, 스프라이트는 마스크 안 정규화), 나머지 base 모델 1/4 해상도(`gen.py`), 그림 변형 조각 18장(`gen_patch.py`: 밑그림과 합성해 계산 후 밑그림 척도로 맞춤), `TITLELOGO.GCC`(`add_logo.py`: OP 깊이 + 로고 평판).
- **가슴·엉덩이 강화**: `pose.py`(YOLOv8x-pose 관절) → `body_boost.py DEPTH.PAK pose.json out.pak 2.0`(가슴 2곳·골반/엉덩이 2곳 원형 가중치 안에서 국소 볼록함 증폭 + 약간 앞으로). 101장 적용, 강도 2.0.
- 생성 순서(비공개 test 저장소 CI): 그림 추출(`tools/arc.py` + `gcc2raw`) → gen.py(전체) → gen2.py(CG·캐릭터, `LIST`/`SHARD`/`NSHARD` 환경변수로 병렬) → gen_patch.py → merge.py → add_logo.py → pose.py → body_boost.py. 모델은 HF `onnx-community/depth-anything-v2-large`(가끔 다운로드가 멈추면 해당 shard만 다시 돌림).

## 7. 빌드·테스트 방법

1. **기기 빌드**: `nonoxp-3ds`에 push → Actions → `nonoxp-3ds-bin`에서 받기. 로컬에서는 devkitARM 필요(`make`).
2. **PC 헤드리스**: `gcc -O2 -w -Isrc -o ai5 src/*.c platform/headless/plat_headless.c -lm`
   - `./ai5 <데이터폴더> MAIN.MES <프레임수> <덤프간격> 15` → `/tmp/fr_*.ppm`(합성 화면)
   - 환경변수: `AI5_DEPTH=1`(층 값 `/tmp/dp_*.pgm`), `AI5_SPLIT=1`(상/하단 분리 덤프), `AI5_GOTO=S02.MES AI5_GOTO_AFTER=main.mes`(장면 점프), `AI5_KEYS`, `AI5_TRACE`, `AI5_UI`, `AI5_UIAT`.
3. **3D 결과 재현**: `python3 tools/hosttest/mk_warp_test.py > wt.c && gcc -O2 -w -o wt wt.c -lm && ./wt fr.ppm dp.pgm out` → 양쪽 눈 `out_L/R.ppm`, 깊이 `out_D.pgm`. plat_3ds.c의 코드를 그대로 잘라 쓰므로 3D 코드를 고치면 바로 PC에서 확인 가능.

## 8. 사용자와의 약속 (꼭 지킬 것)

- **바뀐 파일만** 전달(보통 `nonoxp.3dsx`, 깊이만 바뀌면 `DEPTH.PAK`). 전체 패키지 재전달 금지.
- 사용자는 PC가 없음 → 모든 빌드는 CI, 결과물은 휴대폰으로 받아 SD에 복사.
- 설명은 한국어, 짧고 구체적으로. 기기에서 확인 못 한 건 그렇다고 말할 것.
- 제작자 표기 유지. 게임 그림·깊이 결과는 공개 저장소에 올리지 않음.
- 사용자가 끈 기능은 다시 켜지 말 것: 상단 화면 하단 미러, 대화창 숨기기/환경설정 버튼, 그림자·공기감·초점 흐림 효과, 화면 확대/검은 띠, 자이로(→ 슬라이드 패드로 대체).

## 9. 남은 과제·아이디어 (우선순위 순)

1. **기기 실측 최적화**: New 3DS에서 프레임 시간 측정(svcGetSystemTick)으로 `draw_eye_dm`·`make_half`·깊이 재계산 비용 확인. 후보: 와프 내부 루프를 4픽셀 단위로 펼치기, `g_half`를 BGR 순서로 미리 저장해 쓰기 줄이기, `build_dm_layers` 정수화, 지도 재계산을 별도 코어(New 3DS 추가 코어, `threadCreate`)로.
2. **GPU(citro3d) 사용**: 지금은 전부 CPU. 깊이 지도를 텍스처로 올려 프래그먼트 대신 메시 변위(320x240 격자)로 그리면 와프 비용이 거의 0. 가장 큰 성능 개선 여지.
3. 스틱 시차 사용 시 외곽 잔상: 배경이 드러나는 곳을 이웃 배경으로 채우는 인페인팅(행 방향으로 먼 깊이 쪽 픽셀 복사) 개선.
4. `depth_fx`·`ui_toast` 등 꺼진 코드 정리(필요 없으면 삭제).
5. 깊이 품질: 가슴·엉덩이 강도(`body_boost.py` 4번째 인자) 조절, 남자 캐릭터 제외 옵션, 지도 없는 그림(애니메이션 프레임 AN 등) 추가.
6. 사용자 보고 대기 중: "불러오기 후 특정 장면에서 처음 불러온 화면이 다시 보임"(재현 사진 요청 상태).
7. CG 갤러리 잠금 썸네일의 일본어 로고 한글화, "미사용" 글자 정리(제안만 한 상태).

## 10. 최근 변경 흐름 (요약)
AI 깊이 도입(33) → 강도 조절 반복 → 글자 평판·평면 처리 → 배경 S곡선 → CG/캐릭터 Large 모델 고정밀 → 화면 크기 원복 + 와프 최적화 → 장면별 강도·화면면 → 슬라이더 곡선 → 보조 효과 추가 후 전부 제거 → 슬라이드 패드 시차 → 변형 조각 깊이 → 가슴·엉덩이 강화 → 외곽 단차 완화(잔상 제거). 각 단계 이유는 `docs/FORMATS.md` 33-x 항목.
