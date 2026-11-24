# 河原崎家の一族 (Windows, AI.exe) → Switch native port — engine notes

Disc: `AI.exe` (elf, 2000s Windows edition). Archives are plain (no XOR): `u32 count`, then `count` x 40-byte entries
(`char name[32]`, `u32 offset`, `u32 size`), data stored raw.

| file | contents |
|---|---|
| mes.ARC | 90 scripts (.MES), uncompressed bytecode |
| gcc.ARC | 349 pictures, GCC `G24n` / `G24m` (same decoder as Nonomura: src/gccdec.h), 640x480 |
| sequence.ARC | `.ax` animations, `.msk` transition masks |
| voice.ARC | Ogg Vorbis voices |
| bgm.AWF, effect*.AWF | WAV banks: `u32 count`, entries `name[32] u32 off u32 size u32 -1 u32 0 u32 0` (to confirm) |

## MES bytecode (from AI.exe: dispatcher 0x411ae0, expression evaluator 0x427fd0, arg list 0x40e8c0)

File: `u32 n`, `n` x u32 message offsets (relative to code start), then code. Jump targets are relative to code start.

Expression (RPN, ends with 0xff): `<0x80` literal; `0x80-0x9f` reg v[b-0x80]; `0xa0-0xdf` byte array b-0xa0 [pop];
`e0 + e1 -(sat) e2 * e3 / e4 % e5 rand(u16) e6 && e7 || e8 & e9 | ea ^ eb < ec > ed <= ee >= ef == f0 !=`
(comparison directions to confirm), `f1 u16`, `f2 u32`, `f3 u16` flag bit, `f4` flag bit [pop], `f5` nop,
`f6 u16` word var, `f7` word var [pop], `f8 u16` dword var, `f9` dword var [pop].

Argument list (most commands): up to 16 args, each `f5 <bytes> ff` (string) or an expression; ends with `ff` (or `fe`).

Statements (opcode byte):
- `00` text, NUL-terminated (Shift-JIS) — slot 0x10
- `01 u32` jump · `10 expr u32` jump if zero · `11 expr u32` define procedure (body follows, u32 = end) · `12 args` call procedure
- `13 expr u32` menu: table at u32 = `u8 n`, n x (`u32 cond-expr addr or 0`, `u32 item addr`) · `14 args` run menu
- `05 u16 exprs` 4-bit flags · `06 expr exprs` · `07 u8 expr` pointer reg · `08/09 u8 expr exprs` byte/word poke
- `0a u16 exprs` / `0b expr exprs` word vars · `0c u16 exprs` / `0d expr exprs` dword vars (lists end with ff/fe)
- `35 u32` message number · `37 u32` skip 4 · `21` no args · `ff` end (return from run) · `fe` nop
- args commands: 02 03(load script) 04(call script) 0e 0f 14 15 16 20 22 23 24 25 26 27 28 2b 2d 2e 2f 30 31 32 33 34
- invalid: 17-1f 29 2a 2c 36

`kawarazaki/tools/kdis.py <dir of .MES>` parses all 90 scripts with every jump target on an instruction boundary.
