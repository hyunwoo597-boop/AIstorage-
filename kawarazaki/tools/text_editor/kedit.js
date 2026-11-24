/* 河原崎家の一族 mes.ARC: read, list the lines, put new text in, write back (jump targets re-aimed).
   Hangul goes in as FD hi lo (index = codepoint - 0xAC00, hi/lo = 0x40 + index / 188, 0x40 + index % 188) - the port's
   engine draws those from the Hangul part of kfont16.bin. Everything else stays Shift-JIS. */
var KEdit = (function () {
    function u32(b, o) { return (b[o] | b[o + 1] << 8 | b[o + 2] << 16 | b[o + 3] << 24) >>> 0; }
    function w32(b, o, v) { b[o] = v & 255; b[o + 1] = v >>> 8 & 255; b[o + 2] = v >>> 16 & 255; b[o + 3] = v >>> 24 & 255; }
    function arcRead(a) {
        var n = u32(a, 0), out = [];
        if (n < 1 || n > 4096 || 4 + n * 40 > a.length) throw new Error('mes.ARC 형식이 아닙니다');
        for (var i = 0; i < n; i++) { var p = 4 + i * 40, nm = ''; for (var c = 0; c < 32 && a[p + c]; c++) nm += String.fromCharCode(a[p + c]);
            var off = u32(a, p + 32), sz = u32(a, p + 36); if (off + sz > a.length) throw new Error('mes.ARC 가 잘렸습니다'); out.push({ name: nm, data: a.subarray(off, off + sz) }); }
        return out;
    }
    function arcWrite(ents) {
        var tot = 4 + ents.length * 40; ents.forEach(function (e) { tot += e.data.length; });
        var o = new Uint8Array(tot), at = 4 + ents.length * 40; w32(o, 0, ents.length);
        ents.forEach(function (e, i) { var p = 4 + i * 40; for (var c = 0; c < e.name.length && c < 31; c++) o[p + c] = e.name.charCodeAt(c);
            w32(o, p + 32, at); w32(o, p + 36, e.data.length); o.set(e.data, at); at += e.data.length; });
        return o;
    }
    var ARGOPS = [0x02,0x03,0x04,0x0e,0x0f,0x12,0x14,0x15,0x16,0x20,0x22,0x23,0x24,0x25,0x26,0x27,0x28,0x2b,0x2d,0x2e,0x2f,0x30,0x31,0x32,0x33,0x34];
    /* -> { base, texts: [[start, end)], addrs: [field positions holding code-relative u32 targets] } */
    function parse(d) {
        var n = u32(d, 0), base = 4 + 4 * n, p = base, tables = new Set(), conds = new Set(), texts = [], addrs = [];
        for (var i = 0; i < n; i++) addrs.push(4 + 4 * i);
        function u8() { if (p >= d.length) throw new Error('eof'); return d[p++]; }
        function expr() { for (var k = 0; k < 10000; k++) { var b = u8();
            if (b === 0xff) return; if (b < 0xe0) continue;
            if (b === 0xe5 || b === 0xf1 || b === 0xf3 || b === 0xf6 || b === 0xf8) p += 2; else if (b === 0xf2) p += 4;
            else if (b > 0xf9) throw new Error('expr ' + b.toString(16) + ' @' + (p - 1)); } throw new Error('expr'); }
        function args() { for (;;) { var b = d[p]; if (b === 0xff || b === 0xfe) { p++; return; }
            if (b === 0xf5) { p++; while (d[p] !== 0xff) p++; p++; } else expr(); } }
        function elist() { for (;;) { expr(); var b = u8(); if (b === 0xff || b === 0xfe) return; p--; } }
        function addr() { addrs.push(p); p += 4; }
        while (p < d.length) {
            var a = p - base;
            if (conds.has(a)) { expr(); continue; }
            if (tables.has(a)) { var m = u8(); for (var k = 0; k < m; k++) { if (u32(d, p)) addrs.push(p); p += 4; addrs.push(p); p += 4; } continue; }
            var op = u8();
            if (op === 0x00) { var s = p; while (d[p]) p++; texts.push([s, p]); p++; }
            else if (op === 0x01) addr();
            else if (ARGOPS.indexOf(op) >= 0) args();
            else if (op === 0x05 || op === 0x0a || op === 0x0c) { p += 2; elist(); }
            else if (op === 0x06 || op === 0x0b || op === 0x0d) { expr(); elist(); }
            else if (op === 0x07) { p++; expr(); }
            else if (op === 0x08 || op === 0x09) { p++; expr(); elist(); }
            else if (op === 0x10 || op === 0x11) { expr(); addr(); }
            else if (op === 0x13) { expr(); var t = u32(d, p); addr(); tables.add(t); var q = base + t, nn = d[q];
                for (var k2 = 0; k2 < nn; k2++) { var c = u32(d, q + 1 + 8 * k2); if (c) conds.add(c); } }
            else if (op === 0x21 || op === 0xff || op === 0xfe) {}
            else if (op === 0x35 || op === 0x37) p += 4;
            else throw new Error('op ' + op.toString(16) + ' @' + a);
        }
        return { base: base, texts: texts, addrs: addrs };
    }
    /* repl: { textIndex: Uint8Array (no NUL) } */
    function rebuild(d, P, repl) {
        var parts = [], last = 0, shifts = [];   /* shifts: [old position, cumulative delta after it] */
        var delta = 0;
        P.texts.forEach(function (t, i) { if (!repl[i]) return; parts.push(d.subarray(last, t[0]), repl[i]); last = t[1]; delta += repl[i].length - (t[1] - t[0]); shifts.push([t[1], delta]); });
        parts.push(d.subarray(last));
        var tot = 0; parts.forEach(function (x) { tot += x.length; }); var o = new Uint8Array(tot), at = 0; parts.forEach(function (x) { o.set(x, at); at += x.length; });
        function moved(pos) { var lo = 0, hi = shifts.length; while (lo < hi) { var mid = lo + hi >> 1; if (shifts[mid][0] <= pos) lo = mid + 1; else hi = mid; } return lo ? shifts[lo - 1][1] : 0; }
        P.addrs.forEach(function (f) { var nf = f + moved(f), old = u32(d, f); w32(o, nf, old + moved(P.base + old)); });
        return o;
    }
    /* ---- text <-> bytes ---- */
    var sjDec = null, sjEnc = null;
    function tables() {
        if (sjDec) return; sjDec = new TextDecoder('shift_jis'); sjEnc = new Map();
        for (var l = 0x81; l <= 0xfc; l++) { if (l >= 0xa0 && l < 0xe0) continue; for (var t = 0x40; t <= 0xfc; t++) { if (t === 0x7f) continue;
            var ch = sjDec.decode(new Uint8Array([l, t])); if (ch.length === 1 && ch !== '�' && !sjEnc.has(ch)) sjEnc.set(ch, [l, t]); } }
        for (var b = 0x20; b < 0x7f; b++) sjEnc.set(String.fromCharCode(b), [b]);
        for (var k = 0xa1; k <= 0xdf; k++) { var c2 = sjDec.decode(new Uint8Array([k])); if (!sjEnc.has(c2)) sjEnc.set(c2, [k]); }
    }
    function decode(b) {   /* '％' (81 93, the game's line break) -> '\n' */
        tables(); var s = '';
        for (var i = 0; i < b.length;) { var c = b[i];
            if (c === 0xfd && i + 2 < b.length + 0) { s += String.fromCharCode(0xac00 + (b[i + 1] - 0x40) * 188 + (b[i + 2] - 0x40)); i += 3; }
            else if ((c >= 0x81 && c <= 0x9f) || (c >= 0xe0 && c <= 0xfc)) { var ch = c === 0x81 && b[i + 1] === 0x93 ? '\n' : sjDec.decode(b.subarray(i, i + 2)); s += ch; i += 2; }
            else { s += sjDec.decode(b.subarray(i, i + 1)); i++; } }
        return s;
    }
    function encode(s, bad) {
        tables(); var o = [];
        for (var ch of s) { var cp = ch.codePointAt(0);
            if (ch === '\n') { o.push(0x81, 0x93); continue; }
            if (cp >= 0xac00 && cp <= 0xd7a3) { var x = cp - 0xac00; o.push(0xfd, 0x40 + Math.floor(x / 188), 0x40 + x % 188); continue; }
            var m = sjEnc.get(ch) || sjEnc.get(ch.normalize('NFKC'));
            if (ch === ' ') m = [0x20];
            if (!m) { if (bad) bad.add(ch); m = sjEnc.get('？'); }
            o.push.apply(o, m); }
        return new Uint8Array(o);
    }
    function canEncode(ch) { tables(); var cp = ch.codePointAt(0); return ch === '\n' || (cp >= 0xac00 && cp <= 0xd7a3) || sjEnc.has(ch) || sjEnc.has(ch.normalize('NFKC')); }
    return { arcRead: arcRead, arcWrite: arcWrite, parse: parse, rebuild: rebuild, decode: decode, encode: encode, canEncode: canEncode };
})();
if (typeof module !== 'undefined') module.exports = KEdit;
