/* 노노무라 병원의 사람들 XP — 한글 대사 편집기 핵심부 (브라우저·node 공용, 외부 의존성 없음)
 *
 * MES.ARC → 각 스크립트(LZSS)를 풀어 명령 단위로 읽고, 대사(TXT 명령)를 한글로 보여 준다. 고친 대사는 길이가 바뀌어도
 * 점프·라벨 주소를 모두 다시 맞춰 새 스크립트를 만들고 다시 LZSS 로 묶는다. 한글은 NonoKR 방식(일본어 한자 자리에 한글을
 * 둔 Shift-JIS 코드)이며, 지도에 없는 새 글자는 빈 한자 자리에 넣고 font16.bin 에 글꼴을 그려 넣는다.
 */
(function (root) {
'use strict';
function u16(b, o) { return b[o] | (b[o+1] << 8); }
function u32(b, o) { return (b[o] | (b[o+1] << 8) | (b[o+2] << 16) | (b[o+3] << 24)) >>> 0; }
function w32(b, o, v) { b[o] = v & 255; b[o+1] = (v >>> 8) & 255; b[o+2] = (v >>> 16) & 255; b[o+3] = (v >>> 24) & 255; }

/* ---------- ARC ---------- */
function arcRead(a) {
    var n = u32(a, 0), out = [];
    for (var i = 0; i < n; i++) { var nm = ''; for (var c = 0; c < 20 && a[4 + i*28 + c]; c++) nm += String.fromCharCode(a[4 + i*28 + c]);
        var size = (u32(a, 4 + i*28 + 20) ^ 0x15056109) >>> 0, off = (u32(a, 4 + i*28 + 24) ^ 0x32300645) >>> 0;
        out.push({ name: nm, raw: a.subarray(off, off + size) }); }
    return out;
}
function arcWrite(ents) {
    var n = ents.length, total = 4 + n * 28; ents.forEach(function (e) { total += e.raw.length; });
    var o = new Uint8Array(total), pos = 4 + n * 28; w32(o, 0, n);
    ents.forEach(function (e, i) { for (var c = 0; c < e.name.length && c < 20; c++) o[4 + i*28 + c] = e.name.charCodeAt(c);
        w32(o, 4 + i*28 + 20, (e.raw.length ^ 0x15056109) >>> 0); w32(o, 4 + i*28 + 24, (pos ^ 0x32300645) >>> 0); o.set(e.raw, pos); pos += e.raw.length; });
    return o;
}

/* ---------- LZSS (4 KB 창, 시작 0xFEE, 플래그 비트 1 = 그대로) ---------- */
function lzssDecode(src) {
    var win = new Uint8Array(4096), wp = 0xFEE, out = [], i = 0, n = src.length;
    while (i < n) { var fl = src[i++];
        for (var k = 0; k < 8 && i < n; k++) {
            if (fl >> k & 1) { var c = src[i++]; out.push(c); win[wp] = c; wp = (wp + 1) & 0xFFF; }
            else { if (i + 1 >= n) { i = n; break; } var lo = src[i], hi = src[i + 1]; i += 2; var off = lo | ((hi & 0xF0) << 4), len = (hi & 0xF) + 3;
                for (var j = 0; j < len; j++) { var c2 = win[(off + j) & 0xFFF]; out.push(c2); win[wp] = c2; wp = (wp + 1) & 0xFFF; } } } }
    return Uint8Array.from(out);
}
function lzssEncode(src) {      /* 창 안(0xFEE 부터 채운 0 포함하지 않음)에서 가장 긴 일치, 해시 사슬 */
    var n = src.length, out = [], head = new Int32Array(65536).fill(-1), prev = new Int32Array(n), i = 0;
    var flagPos = 0, flag = 0, bit = 8;
    function put(isLit, a, b) {
        if (bit === 8) { flagPos = out.length; out.push(0); flag = 0; bit = 0; }
        if (isLit) { flag |= 1 << bit; out.push(a); } else { out.push(a, b); }
        out[flagPos] = flag; bit++;
    }
    function ins(p) { if (p + 2 < n) { var h = (src[p] << 8 | src[p + 1]) ^ (src[p + 2] << 4); h &= 0xFFFF; prev[p] = head[h]; head[h] = p; } }
    while (i < n) {
        var best = 0, bpos = 0;
        if (i + 2 < n) { var h = ((src[i] << 8 | src[i + 1]) ^ (src[i + 2] << 4)) & 0xFFFF, c = head[h], tries = 0;
            while (c >= 0 && i - c < 4096 - 18 && tries++ < 256) { var l = 0; while (l < 18 && i + l < n && src[c + l] === src[i + l]) l++;
                if (l > best) { best = l; bpos = c; if (l === 18) break; } c = prev[c]; } }
        if (best >= 3) { var ring = (0xFEE + bpos) & 0xFFF; put(false, ring & 0xFF, ((ring >> 4) & 0xF0) | (best - 3)); for (var k = 0; k < best; k++) ins(i + k); i += best; }
        else { put(true, src[i]); ins(i); i++; }
    }
    return Uint8Array.from(out);
}

/* ---------- MES 바이트코드 (tools/mesdis.py 와 같은 규칙) ---------- */
function parseMes(d) {
    var nl = u32(d, 0), labels = [], base = 4 + nl * 4, p = base, st = [];
    for (var i = 0; i < nl; i++) labels.push(u32(d, 4 + i * 4));
    function U8() { if (p >= d.length) throw new Error('eof'); return d[p++]; }
    function expr() { var depth = 0; for (;;) { var b = U8(); if (b === 0xff) break;
            if (b === 0x80) { U8(); depth++; } else if (b === 0xa0 || b === 0xc0 || b === 0xf9) { U8(); }
            else if (b >= 0xe0 && b <= 0xf0) { if (b !== 0xe5) depth--; } else if (b === 0xf1) { p += 2; depth++; } else if (b === 0xf2) { p += 4; depth++; }
            else if (b === 0xf3 || b === 0xf6) { p += 2; depth++; } else if (b === 0xf5 || b === 0xf7) { } else if (b === 0xf8) { U8(); depth++; } else depth++; } }
    function elist() { expr(); while (U8()) expr(); }
    function cstr() { var s = p; while (d[p]) p++; p++; return [s, p - 1]; }
    function params() { for (;;) { var t = U8(); if (t === 0) return; if (t === 1) cstr(); else if (t === 2) expr(); else throw new Error('param ' + t); } }
    while (p < d.length) {
        var s0 = p, op = U8(), rec = { start: s0 - base, op: op, targets: [] };
        switch (op) {
        case 0: break;
        case 1: case 2: var r = cstr(); rec.text = [r[0], r[1]]; break;
        case 3: p += 2; elist(); break;
        case 4: U8(); elist(); break;
        case 5: expr(); elist(); break;
        case 6: case 7: case 8: expr(); U8(); elist(); break;
        case 9: p += 2; elist(); break;
        case 10: expr(); elist(); break;
        case 0xb: expr(); rec.targets.push(p); p += 4; break;
        case 0xc: rec.targets.push(p); p += 4; break;
        case 0xd: expr(); params(); break;
        case 0xe: case 0xf: case 0x11: case 0x12: case 0x15: case 0x1d: case 0x1b: params(); break;
        case 0x10: params(); rec.targets.push(p); p += 4; break;
        case 0x13: U8(); break;
        case 0x14: case 0x1c: expr(); rec.targets.push(p); p += 4; break;
        case 0x16: U8(); elist(); break;
        case 0x17: case 0x1f: p += 4; break;
        case 0x18: expr(); break;
        case 0x19: case 0x1a: break;
        default: throw new Error('unknown op ' + op.toString(16) + ' at ' + (s0 - base));
        }
        rec.end = p - base; st.push(rec);
    }
    return { labels: labels, base: base, st: st };
}
/* edits: { 명령번호: 새 문자열 바이트(0 없이) } → 새 스크립트 (주소 다시 맞춤) */
function rebuildMes(d, pm, edits) {
    var base = pm.base, parts = [], map = new Map(), pos = 0;
    pm.st.forEach(function (r, i) {
        map.set(r.start, pos);
        var seg;
        if (edits[i] !== undefined && r.text) { var a = d.subarray(base + r.start, r.text[0]), b = d.subarray(r.text[1], base + r.end);
            seg = new Uint8Array(a.length + edits[i].length + b.length); seg.set(a); seg.set(edits[i], a.length); seg.set(b, a.length + edits[i].length);
            r._newText = a.length; }
        else seg = d.slice(base + r.start, base + r.end);
        r._seg = seg; r._new = pos; pos += seg.length;
    });
    map.set(d.length - base, pos);
    var out = new Uint8Array(base + pos); out.set(d.subarray(0, base));
    pm.labels.forEach(function (l, i) { if (!map.has(l)) throw new Error('label ' + l); w32(out, 4 + i * 4, map.get(l)); });
    pm.st.forEach(function (r) {
        out.set(r._seg, base + r._new);
        r.targets.forEach(function (tp) {   /* tp: 원래 파일 안 위치 → 새 위치 */
            var rel = tp - (base + r.start), shift = (r._newText !== undefined && tp > r.text[1]) ? (r._seg.length - (r.end - r.start)) : 0;
            var at = base + r._new + rel + shift, old = u32(d, tp);
            if (!map.has(old)) throw new Error('target ' + old); w32(out, at, map.get(old)); });
    });
    return out;
}

/* ---------- 글자 변환 ---------- */
var HALF = { '　': ' ', '．': '.', '，': ',', '？': '?', '！': '!', '（': '(', '）': ')', '［': '[', '］': ']', '～': '~', '：': ':', '；': ';', '－': '-', '＋': '+', '＝': '=', '％': '%', '＆': '&', '／': '/', '＿': '_', '’': "'", '”': '"' };
var FULL = {}; for (var k in HALF) FULL[HALF[k]] = k;
function makeTables(map) {      /* map: [[sjis, unicode], ...] */
    var dec = new Map(), enc = new Map(), td = new TextDecoder('shift_jis');
    for (var l = 0x81; l <= 0xFC; l++) { if (l >= 0xA0 && l < 0xE0) continue;
        for (var t = 0x40; t <= 0xFC; t++) { if (t === 0x7F) continue; var ch = td.decode(new Uint8Array([l, t])); if (ch.length === 1 && ch !== '�') { dec.set(l << 8 | t, ch); if (!enc.has(ch)) enc.set(ch, l << 8 | t); } } }
    map.forEach(function (m) { var ch = String.fromCodePoint(m[1]); dec.set(m[0], ch); enc.set(ch, m[0]); });
    return { dec: dec, enc: enc, kr: new Set(map.map(function (m) { return m[0]; })) };
}
function decodeText(b, T) {     /* 바이트 → 보기 좋은 글(전각 문장부호는 반각으로) */
    var s = '';
    for (var i = 0; i < b.length;) { var c = b[i];
        if (((c >= 0x81 && c <= 0x9F) || (c >= 0xE0 && c <= 0xFC)) && i + 1 < b.length) { var code = c << 8 | b[i + 1], ch = T.dec.get(code); i += 2;
            if (ch === undefined) ch = '{' + code.toString(16) + '}'; else if (HALF[ch]) ch = HALF[ch];
            else if (ch >= '０' && ch <= 'ｚ' && !T.kr.has(code)) ch = String.fromCharCode(ch.charCodeAt(0) - 0xFEE0);
            s += ch; }
        else { s += c < 0x80 ? String.fromCharCode(c) : '{' + c.toString(16) + '}'; i++; } }
    return s;
}
/* 글 → 바이트. 지도에 없는 한글은 newMap(새 SJIS 자리 배정)에 추가. 쓸 수 없는 글자가 있으면 bad 에 */
function encodeText(s, T, alloc, bad) {
    var out = [];
    for (var i = 0; i < s.length; i++) { var ch = s[i];
        if (ch === '{') { var e = s.indexOf('}', i); if (e > i) { var v = parseInt(s.slice(i + 1, e), 16); if (v > 0xff) out.push(v >> 8, v & 255); else out.push(v); i = e; continue; } }
        if (FULL[ch]) ch = FULL[ch];
        else if (ch >= '0' && ch <= 'z' && /[0-9A-Za-z]/.test(ch)) ch = String.fromCharCode(ch.charCodeAt(0) + 0xFEE0);
        var code = T.enc.get(ch);
        if (code === undefined && ch >= '가' && ch <= '힣' && alloc) code = alloc(ch);
        if (code === undefined) { bad && bad.push(s[i]); continue; }
        out.push(code >> 8, code & 255); }
    return Uint8Array.from(out);
}
function glyphIndex(code) { var l = code >> 8, t = code & 255, li = l <= 0x9F ? l - 0x81 : l - 0xE0 + 31; return 95 + li * 189 + (t - 0x40); }

var api = { arcRead: arcRead, arcWrite: arcWrite, lzssDecode: lzssDecode, lzssEncode: lzssEncode, parseMes: parseMes, rebuildMes: rebuildMes, makeTables: makeTables, decodeText: decodeText, encodeText: encodeText, glyphIndex: glyphIndex };
if (typeof module !== 'undefined' && module.exports) module.exports = api;
root.EditCore = api;
})(typeof self !== 'undefined' ? self : this);
