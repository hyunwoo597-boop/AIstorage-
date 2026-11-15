PC-side logic test for ctr/source/main.c using a fake <3ds.h> (no real 3DS needed).
Build:  gcc -O2 -DGAME_DIR0='"/path/to/gamecopy"' -Itest/stub3ds -Icore -Ictr/source -o stub_run \
          test/stub3ds/main_wrap.c test/stub3ds/harness.c ctr/source/vgascale.c core/*.c -lm
Run:    STUB_DUMP=1 STUB_MODE=1 ./stub_run 3600     (STUB_MODE=1: press Enter periodically, STUB_MODE=3: no input,
        STUB_SLOW=15: pretend the CPU is 15x slower)  -> /tmp/st_top_*.ppm, /tmp/st_bot_*.ppm, <gamedir>/nono_log.txt
