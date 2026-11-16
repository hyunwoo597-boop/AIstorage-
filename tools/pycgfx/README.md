이 폴더는 [skyfloogle/pycgfx](https://github.com/skyfloogle/pycgfx) (MIT, LICENSE 참고)의 사본입니다.
glTF 장면을 3DS 홈 메뉴 배너용 CGFX 로 바꾸는 라이브러리(`cgfx/`)와 변환기(`convert.py` = 원본 `main.py`).

원본에서 바꾼 것 (`convert.py`, "nonomura" 주석):
- 그림 너비를 512 까지 허용 (원본은 256 으로 줄임)
- 조명 없는 재질 지원: glTF 재질에 `KHR_materials_unlit` 확장이 있으면 그림 색을 그대로 냄

`gltflib` 는 쓰지 않고, `tools/gen_banner_cgfx.py` 가 같은 모양의 간단한 객체를 직접 만들어 넘깁니다.
