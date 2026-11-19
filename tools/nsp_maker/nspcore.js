/* 노노무라 병원의 사람들 XP (Switch) — 설치형 NSP 생성기 핵심부 (브라우저·node 공용). core.js(XpCore) 다음에 읽는다.
 *
 * hacbrewpack 과 같은 구성: Program NCA(ExeFS = main + main.npdm, RomFS = 게임 파일), Control NCA(RomFS = control.nacp +
 * 아이콘), Meta NCA(PFS0 = Application_<id>.cnmt), 모두 표준 암호(키 영역, 키 세대 0). 헤더 서명은 0 → sigpatch 가 있는 CFW 용.
 * 큰 데이터는 Blob 조각으로만 다루고, 해시·암호화는 8 MB 씩 읽으며 처리한다.
 *   NspCore.parseKeys(text)                      : prod.keys → { header_key, key_area_key_application_00 }
 *   NspCore.buildNsp({ keys, nso, npdm, nacp, icon, titleId, files }, prog) → Blob
 */
(function (root) {
'use strict';
var X = root.XpCore || (typeof require !== 'undefined' ? require('../cia_maker_xp/core.js') : null);
var BlobC = X.Blob, Sha = X.Sha, sha256 = X.sha256;
var subtle = (typeof crypto !== 'undefined' && crypto.subtle) ? crypto.subtle : require('crypto').webcrypto.subtle;
var getRandom = function (n) { var u = new Uint8Array(n); (typeof crypto !== 'undefined' && crypto.getRandomValues ? crypto : require('crypto').webcrypto).getRandomValues(u); return u; };

function w32(b, o, v) { b[o] = v & 255; b[o+1] = (v >>> 8) & 255; b[o+2] = (v >>> 16) & 255; b[o+3] = (v >>> 24) & 255; }
function w64(b, o, v) { w32(b, o, v % 4294967296); w32(b, o + 4, Math.floor(v / 4294967296)); }
function align(x, a) { return Math.ceil(x / a) * a; }
function sizeOf(d) { return d.size !== undefined ? d.size : d.length; }
function rd(blob, off, len) { return blob.slice(off, off + len).arrayBuffer().then(function (b) { return new Uint8Array(b); }); }
function tick() { return new Promise(function (r) { setTimeout(r, 0); }); }
function hexb(s) { s = s.replace(/[^0-9a-fA-F]/g, ''); var u = new Uint8Array(s.length / 2); for (var i = 0; i < u.length; i++) u[i] = parseInt(s.substr(i * 2, 2), 16); return u; }
function hex(u) { var s = ''; for (var i = 0; i < u.length; i++) s += ('0' + u[i].toString(16)).slice(-2); return s; }
function utf8(s) { return new TextEncoder().encode(s); }

/* ---------- VBlob: 조각(Blob / Uint8Array / RawFile / VBlob)을 복사하지 않고 이어 붙인 읽기 전용 덩어리 ----------
   브라우저 Blob 에 CD 이미지 속 파일을 넣으려면 메모리로 꺼내야 하는데(수백 MB), 암호화 결과까지 더하면 브라우저 한도를 넘는다.
   VBlob 은 읽을 때만 해당 조각을 읽는다. */
function VBlob(parts) { this.parts = []; this.offs = []; var o = 0;
    for (var i = 0; i < parts.length; i++) { var p = parts[i], n = sizeOf(p); if (!n) continue; this.parts.push(p); this.offs.push(o); o += n; } this.size = o; }
VBlob.prototype.slice = function (a, b) {
    if (b === undefined || b > this.size) b = this.size; if (a < 0) a = 0; if (b < a) b = a; var out = [];
    for (var i = 0; i < this.parts.length; i++) { var s = this.offs[i], e = s + sizeOf(this.parts[i]); if (e <= a || s >= b) continue;
        var p = this.parts[i], x = Math.max(a, s) - s, y = Math.min(b, e) - s; out.push(p instanceof Uint8Array ? p.subarray(x, y) : p.slice(x, y)); }
    return new VBlob(out);
};
VBlob.prototype.arrayBuffer = async function () {
    var out = new Uint8Array(this.size), o = 0;
    for (var i = 0; i < this.parts.length; i++) { var p = this.parts[i]; var d = p instanceof Uint8Array ? p : new Uint8Array(await p.arrayBuffer()); out.set(d, o); o += d.length; }
    return out.buffer;
};
function cat(parts) { return new VBlob(parts); }
function arcBuildV(entries) {   /* XpCore.arcBuild 과 같지만 VBlob (RawFile 조각을 그대로 담을 수 있음) */
    var n = entries.length, tab = new Uint8Array(4 + n * 28), pos = 4 + n * 28; w32(tab, 0, n);
    entries.forEach(function (e, i) { var sz = sizeOf(e.data); for (var c = 0; c < e.name.length && c < 20; c++) tab[4 + i*28 + c] = e.name.charCodeAt(c);
        w32(tab, 4 + i*28 + 20, (sz ^ 0x15056109) >>> 0); w32(tab, 4 + i*28 + 24, (pos ^ 0x32300645) >>> 0); pos += sz; });
    return cat([tab].concat(entries.map(function (e) { return e.data; })));
}

/* ---------- 키 ---------- */
function parseKeys(text) {
    var k = {}; text.split(/\r?\n/).forEach(function (l) { var m = /^\s*([a-z0-9_]+)\s*=\s*([0-9a-fA-F]+)\s*$/.exec(l); if (m) k[m[1].toLowerCase()] = m[2]; });
    if (!k.header_key || k.header_key.length !== 64) throw new Error('prod.keys 에 header_key 가 없습니다');
    if (!k.key_area_key_application_00 || k.key_area_key_application_00.length !== 32) throw new Error('prod.keys 에 key_area_key_application_00 이 없습니다');
    return { header_key: hexb(k.header_key), kak: hexb(k.key_area_key_application_00) };
}

/* ---------- AES (WebCrypto) ---------- */
async function aesKey(raw, alg) { return subtle.importKey('raw', raw, { name: alg }, false, ['encrypt', 'decrypt']); }
async function ecbEncrypt(key, block) {     /* 블록 하나: CBC(IV 0) 의 첫 블록 = ECB */
    var r = new Uint8Array(await subtle.encrypt({ name: 'AES-CBC', iv: new Uint8Array(16) }, key, block)); return r.subarray(0, 16);
}
async function xtsEncrypt(rawKey, data, sectorSize, firstSector) {   /* 닌텐도식 AES-128-XTS (섹터 번호 빅엔디언 트윅) */
    var k1 = await aesKey(rawKey.subarray(0, 16), 'AES-CBC'), k2 = await aesKey(rawKey.subarray(16, 32), 'AES-CBC'), out = new Uint8Array(data.length);
    for (var s = 0; s * sectorSize < data.length; s++) {
        var tw = new Uint8Array(16), n = firstSector + s; for (var i = 15; i >= 0 && n > 0; i--) { tw[i] = n % 256; n = Math.floor(n / 256); }
        var T = new Uint8Array(await ecbEncrypt(k2, tw));
        /* 블록 전체를 한 번에: P xor T → ECB(여러 블록) 대신 블록마다 */
        for (var b = 0; b < sectorSize; b += 16) {
            var o = s * sectorSize + b, blk = new Uint8Array(16); for (var j = 0; j < 16; j++) blk[j] = data[o + j] ^ T[j];
            var c = await ecbEncrypt(k1, blk); for (j = 0; j < 16; j++) out[o + j] = c[j] ^ T[j];
            var carry = 0; for (j = 0; j < 16; j++) { var v = (T[j] << 1) | carry; carry = (T[j] >> 7) & 1; T[j] = v & 255; } if (carry) T[0] ^= 0x87;   /* GF(2^128) x2, 리틀엔디언 */
        }
    }
    return out;
}
async function ctrEncrypt(key, data, ctrHi, absOffset) {    /* data 는 absOffset(16 의 배수)에서 시작 */
    var ctr = new Uint8Array(16); ctr.set(ctrHi, 0); var n = absOffset / 16;
    for (var i = 15; i >= 8; i--) { ctr[i] = n % 256; n = Math.floor(n / 256); }
    return new Uint8Array(await subtle.encrypt({ name: 'AES-CTR', counter: ctr, length: 64 }, key, data));
}

/* ---------- PFS0 ---------- */
function buildPfs0(files) {      /* files: [{name, data: Uint8Array|Blob}] → Blob (머리는 0x20 정렬) */
    var names = files.map(function (f) { return utf8(f.name); }), st = 0; names.forEach(function (n) { st += n.length + 1; });
    var hdrLen = 0x10 + files.length * 0x18 + st, padded = align(hdrLen, 0x20); st += padded - hdrLen;
    var h = new Uint8Array(0x10 + files.length * 0x18 + st); h.set([0x50, 0x46, 0x53, 0x30]); w32(h, 4, files.length); w32(h, 8, st);
    var off = 0, so = 0, base = 0x10 + files.length * 0x18;
    files.forEach(function (f, i) { var e = 0x10 + i * 0x18; w64(h, e, off); w64(h, e + 8, sizeOf(f.data)); w32(h, e + 16, so); h.set(names[i], base + so); so += names[i].length + 1; off += sizeOf(f.data); });
    return cat([h].concat(files.map(function (f) { return f.data; })));
}

/* ---------- Switch RomFS (루트 폴더 하나) ---------- */
function nameHash(parent, nameBytes) { var h = (parent ^ 123456789) >>> 0; for (var i = 0; i < nameBytes.length; i++) { h = ((h >>> 5) | (h << 27)) >>> 0; h = (h ^ nameBytes[i]) >>> 0; } return h; }
function hashCount(n) { if (n < 3) return 3; if (n < 19) return n | 1; while (n % 2 === 0 || n % 3 === 0 || n % 5 === 0 || n % 7 === 0 || n % 11 === 0 || n % 13 === 0 || n % 17 === 0) n++; return n; }
function buildSwitchRomfs(files) {
    var n = files.length, names = files.map(function (f) { return utf8(f.name); });
    var dataOff = [], dof = 0;
    files.forEach(function (f) { dof = align(dof, 0x10); dataOff.push(dof); dof += sizeOf(f.data); });
    var fileDataStart = 0x200, dataEnd = align(fileDataStart + dof, 4);
    var dhc = hashCount(1), fhc = hashCount(n);
    var dirMetaSize = 0x18, fileMetaOff = [], fms = 0;
    names.forEach(function (nm) { fileMetaOff.push(fms); fms += 0x20 + align(nm.length, 4); });
    var dirHashOff = dataEnd, dirMetaOffAbs = dirHashOff + dhc * 4, fileHashOff = dirMetaOffAbs + dirMetaSize, fileMetaOffAbs = fileHashOff + fhc * 4;
    var meta = new Uint8Array(dhc * 4 + dirMetaSize + fhc * 4 + fms), m0 = dirHashOff;
    var W32 = function (o, v) { w32(meta, o - m0, v); };
    for (var i = 0; i < dhc; i++) W32(dirHashOff + i * 4, 0xffffffff);
    W32(dirHashOff + (nameHash(0, []) % dhc) * 4, 0);
    W32(dirMetaOffAbs, 0); W32(dirMetaOffAbs + 4, 0xffffffff); W32(dirMetaOffAbs + 8, 0xffffffff); W32(dirMetaOffAbs + 12, n ? 0 : 0xffffffff); W32(dirMetaOffAbs + 16, 0xffffffff); W32(dirMetaOffAbs + 20, 0);
    for (i = 0; i < fhc; i++) W32(fileHashOff + i * 4, 0xffffffff);
    for (i = 0; i < n; i++) {
        var e = fileMetaOffAbs + fileMetaOff[i], b = fileHashOff + (nameHash(0, names[i]) % fhc) * 4;
        W32(e, 0); W32(e + 4, i + 1 < n ? fileMetaOff[i + 1] : 0xffffffff);
        w64(meta, e + 8 - m0, dataOff[i]); w64(meta, e + 16 - m0, sizeOf(files[i].data));
        W32(e + 24, meta[b - m0] | (meta[b - m0 + 1] << 8) | (meta[b - m0 + 2] << 16) | (meta[b - m0 + 3] << 24)); W32(b, fileMetaOff[i]);
        W32(e + 28, names[i].length); meta.set(names[i], e + 32 - m0);
    }
    var hdr = new Uint8Array(0x200), H = function (o, v) { w64(hdr, o, v); };
    H(0, 0x50); H(8, dirHashOff); H(0x10, dhc * 4); H(0x18, dirMetaOffAbs); H(0x20, dirMetaSize); H(0x28, fileHashOff); H(0x30, fhc * 4); H(0x38, fileMetaOffAbs); H(0x40, fms); H(0x48, fileDataStart);
    var parts = [hdr], pos = fileDataStart;
    files.forEach(function (f, k) { var at = fileDataStart + dataOff[k]; if (at > pos) parts.push(new Uint8Array(at - pos)); parts.push(f.data); pos = at + sizeOf(f.data); });
    if (dataEnd > pos) parts.push(new Uint8Array(dataEnd - pos));
    parts.push(meta);
    return cat(parts);
}

/* ---------- 해시 단계 ---------- */
async function hashBlocks(blob, bs, prog) {     /* blob(크기는 bs 의 배수) 의 블록마다 SHA-256 → Uint8Array */
    var n = blob.size / bs, out = new Uint8Array(n * 32), k = 0, CH = Math.max(bs, (8 << 20) - (8 << 20) % bs);
    for (var p = 0; p < blob.size; p += CH) { var d = await rd(blob, p, Math.min(CH, blob.size - p));
        for (var b = 0; b < d.length; b += bs) out.set(sha256(d.subarray(b, b + bs)), (k++) * 32);
        prog && prog((p + d.length) / blob.size); await tick(); }
    return out;
}
function padTo(blob, a) { var s = sizeOf(blob), t = align(Math.max(s, 1), a); return cat(t > s ? [blob, new Uint8Array(t - s)] : [blob]); }
/* PFS0 섹션: [해시 표][PFS0] — 블록 0x1000, 데이터는 블록 배수로 채움 */
async function pfsSection(pfs) {
    var bs = 0x1000, data = padTo(pfs, bs), table = await hashBlocks(data, bs);
    var tsize = table.length, dataOff = align(tsize, 0x200), sec = cat([table, new Uint8Array(dataOff - tsize), data]);
    var hd = new Uint8Array(0xF8); hd.set(sha256(table), 0); w32(hd, 0x20, bs); w32(hd, 0x24, 2);
    w64(hd, 0x28, 0); w64(hd, 0x30, tsize); w64(hd, 0x38, dataOff); w64(hd, 0x40, data.size);
    return { blob: sec, fsType: 1, hashType: 2, hashData: hd };
}
/* RomFS 섹션: IVFC 6단계, 블록 0x4000. 모든 단계를 블록 배수로 채워 기록 */
async function romfsSection(romfs, prog) {
    var bs = 0x4000, levels = [padTo(romfs, bs)];
    var h = await hashBlocks(levels[0], bs, prog);
    for (var i = 0; i < 5; i++) { var lv = padTo(h, bs); levels.unshift(lv); if (i < 4) h = await hashBlocks(lv, bs); }
    /* levels[0] = 1단계(가장 작은 해시) … levels[5] = 데이터 */
    var master = sha256(new Uint8Array(await levels[0].arrayBuffer()));
    var hd = new Uint8Array(0xF8); hd.set([0x49, 0x56, 0x46, 0x43]); w32(hd, 4, 0x20000); w32(hd, 8, 0x20); w32(hd, 0xC, 7);
    var off = 0, parts = [];
    for (var j = 0; j < 6; j++) { var e = 0x10 + j * 0x18; w64(hd, e, off); w64(hd, e + 8, levels[j].size); w32(hd, e + 16, 14); off += levels[j].size; parts.push(levels[j]); }
    hd.set(master, 0xC0);
    return { blob: cat(parts), fsType: 0, hashType: 3, hashData: hd };
}

/* ---------- NCA ---------- */
async function buildNca(keys, contentType, titleId, sections, prog) {
    var keyArea = new Uint8Array(0x40); var secKey = getRandom(16); keyArea.set(secKey, 0x20);       /* 2번 = AES-CTR 키 */
    var kak = await aesKey(keys.kak, 'AES-CBC'), encArea = new Uint8Array(0x40);
    for (var q = 0; q < 4; q++) encArea.set(await ecbEncrypt(kak, keyArea.subarray(q * 16, q * 16 + 16)), q * 16);
    var ctrKey = await aesKey(secKey, 'AES-CTR');
    var hdr = new Uint8Array(0xC00), pos = 0xC00, starts = [];
    sections.forEach(function (s) { starts.push(pos); pos = align(pos + s.blob.size, 0x200); });
    var total = pos;
    hdr.set([0x4E, 0x43, 0x41, 0x33], 0x200); hdr[0x204] = 0; hdr[0x205] = contentType; hdr[0x206] = 0; hdr[0x207] = 0;
    w64(hdr, 0x208, total); hdr.set(hexb(titleId).reverse(), 0x210); w32(hdr, 0x218, 0); w32(hdr, 0x21C, 0x000C1100); hdr[0x220] = 0; hdr[0x221] = 0;
    sections.forEach(function (s, i) {
        var e = 0x240 + i * 0x10; w32(hdr, e, starts[i] / 0x200); w32(hdr, e + 4, (starts[i] + s.blob.size) / 0x200 | 0); w32(hdr, e + 8, 1);
        if ((starts[i] + s.blob.size) % 0x200) w32(hdr, e + 4, Math.ceil((starts[i] + s.blob.size) / 0x200));
        var f = new Uint8Array(0x200); f[0] = 2; f[2] = s.fsType; f[3] = s.hashType; f[4] = 3; f.set(s.hashData, 8);
        w32(f, 0x140, i); w32(f, 0x144, 0);                                   /* CTR 윗부분 = 섹션 번호 */
        hdr.set(f, 0x400 + i * 0x200); hdr.set(sha256(f), 0x280 + i * 0x20);
        s.ctrHi = new Uint8Array(8); var v = i; for (var j = 7; j >= 0; j--) { s.ctrHi[j] = v & 255; v = Math.floor(v / 256); }
    });
    hdr.set(encArea, 0x300);
    var encHdr = await xtsEncrypt(keys.header_key, hdr, 0x200, 0);
    var parts = [encHdr], sha = new Sha(); sha.update(encHdr);
    for (var i = 0; i < sections.length; i++) {
        var s = sections[i], CH = 8 << 20;
        for (var p = 0; p < s.blob.size; p += CH) {
            var d = await rd(s.blob, p, Math.min(CH, s.blob.size - p)), padLen = (16 - d.length % 16) % 16;
            if (padLen) { var dd = new Uint8Array(d.length + padLen); dd.set(d); d = dd; }
            var c = await ctrEncrypt(ctrKey, d, s.ctrHi, starts[i] + p); sha.update(c); parts.push(new BlobC([c]));
            prog && prog((p + d.length) / s.blob.size, i); await tick();
        }
        var end = starts[i] + align(s.blob.size, 16), next = i + 1 < sections.length ? starts[i + 1] : total;
        if (next > end) { var z = await ctrEncrypt(ctrKey, new Uint8Array(next - end), s.ctrHi, end); sha.update(z); parts.push(z); }
    }
    var hash = sha.digest();
    return { blob: new BlobC(parts), hash: hash, id: hex(hash.subarray(0, 16)), size: total };
}

/* ---------- CNMT ---------- */
function buildCnmt(titleId, contents) {     /* contents: [{hash, id, size, type}] (1 Program, 3 Control) */
    var c = new Uint8Array(0x20 + 0x10 + contents.length * 0x38 + 0x20), tid = hexb(titleId).reverse();
    c.set(tid, 0); w32(c, 8, 0); c[0xC] = 0x80; w32(c, 0xE, 0x10 | (contents.length << 16)); c[0x12] = 0; c[0x13] = 0; c[0x14] = 0;
    var patch = hexb(titleId); patch[6] = (patch[6] & 0xF0) | 0x08; c.set(patch.reverse(), 0x20); w32(c, 0x28, 0); w32(c, 0x2C, 0);
    contents.forEach(function (x, i) { var o = 0x30 + i * 0x38; c.set(x.hash, o); c.set(hexb(x.id), o + 0x20); w32(c, o + 0x30, x.size % 4294967296); c[o + 0x34] = Math.floor(x.size / 4294967296) & 255; c[o + 0x35] = 0; c[o + 0x36] = x.type; c[o + 0x37] = 0; });
    return c;
}

/* ---------- NSP ---------- */
var LANGS = ['AmericanEnglish', 'BritishEnglish', 'Japanese', 'French', 'German', 'LatinAmericanSpanish', 'Spanish', 'Italian', 'Dutch', 'CanadianFrench', 'Portuguese', 'Russian', 'Korean', 'TraditionalChinese', 'SimplifiedChinese'];
async function buildNsp(o, prog) {
    var P = function (a, b, m) { return function (f) { prog && prog(a + (b - a) * Math.min(f, 1), m); }; };
    /* Program NCA */
    var exefs = await pfsSection(buildPfs0([{ name: 'main', data: o.nso }, { name: 'main.npdm', data: o.npdm }]));
    var secs = [exefs];
    if (o.files && o.files.length) secs.push(await romfsSection(buildSwitchRomfs(o.files), P(0.02, 0.35, '데이터 확인값 계산')));   /* 없으면 게임 파일은 SD 카드에서 */
    var program = await buildNca(o.keys, 0, o.titleId, secs, function (f, i) { if (i === 1) P(0.35, 0.9, '암호화')(f); });
    /* Control NCA: control.nacp + 언어별 아이콘 */
    var cfiles = [{ name: 'control.nacp', data: o.nacp }];
    for (var li = 0; li < LANGS.length; li++) { var t = o.nacp.subarray(li * 0x300, li * 0x300 + 0x200); if (t[0]) cfiles.push({ name: 'icon_' + LANGS[li] + '.dat', data: o.icon }); }
    var nacp = new Uint8Array(o.nacp);   /* 아이콘이 있는 언어와 NACP 가 맞도록(그대로) */
    var control = await buildNca(o.keys, 2, o.titleId, [await romfsSection(buildSwitchRomfs(cfiles))]);
    /* Meta NCA */
    var cnmt = buildCnmt(o.titleId, [{ hash: program.hash, id: program.id, size: program.size, type: 1 }, { hash: control.hash, id: control.id, size: control.size, type: 3 }]);
    var meta = await buildNca(o.keys, 1, o.titleId, [await pfsSection(buildPfs0([{ name: 'Application_' + o.titleId.replace(/^0x/, '').toLowerCase() + '.cnmt', data: cnmt }]))]);
    prog && prog(0.95, 'NSP 묶는 중');
    var nsp = new BlobC(buildPfs0([{ name: program.id + '.nca', data: program.blob }, { name: control.id + '.nca', data: control.blob }, { name: meta.id + '.cnmt.nca', data: meta.blob }]).parts);   /* 진짜 Blob (암호화된 조각뿐) */
    prog && prog(1, '완료');
    return nsp;
}

var api = { VBlob: VBlob, cat: cat, arcBuildV: arcBuildV, parseKeys: parseKeys, buildNsp: buildNsp, buildPfs0: buildPfs0, buildSwitchRomfs: buildSwitchRomfs, pfsSection: pfsSection, romfsSection: romfsSection, buildNca: buildNca, buildCnmt: buildCnmt, xtsEncrypt: xtsEncrypt };
if (typeof module !== 'undefined' && module.exports) module.exports = api;
root.NspCore = api;
})(typeof self !== 'undefined' ? self : this);
