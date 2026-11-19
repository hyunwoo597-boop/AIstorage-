# NONOXP 3DS — 野々村病院の人々 New 3DS 네이티브 포트 (한글판, 입체 3D)

제작자 : 디시인사이드 스위치CFW 갤러리 -루카-

- 처음 보는 사람/AI는 **HANDOVER.md**부터 읽으세요(구조, 빌드, 3D 파이프라인, 약속, 남은 과제).
- 포맷 분석과 단계별 작업 기록: docs/FORMATS.md
- 빌드: 브랜치 nonoxp-3ds에 push → GitHub Actions → 브랜치 nonoxp-3ds-bin에 nonoxp.3dsx
- PC 테스트: `gcc -O2 -w -Isrc -o ai5 src/*.c platform/headless/plat_headless.c -lm`
- 게임 데이터(ARC 파일)와 DEPTH.PAK은 저장소에 없습니다. 본인 소유 디스크와 한글 패치에서 준비하세요(HANDOVER.md 3장).
