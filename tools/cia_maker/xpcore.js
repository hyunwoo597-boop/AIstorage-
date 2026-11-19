/* 노노무라 병원의 사람들 XP (3DS) — 게임 파일 내장 CIA 생성기 핵심부 (브라우저·node 공용, 외부 의존성 없음)
 *
 * 큰 파일(수백 MB)을 휴대폰에서도 다룰 수 있게 모든 데이터를 Blob 조각으로 다루고, 해시는 조각을 차례로 읽으며 계산한다.
 *   XpCore.findGame(files)          : 고른 파일들(CD 분할 zip / 이미지 / ARC)에서 게임 파일 Blob 을 찾는다
 *   XpCore.musicToAdpcm(blob, prog) : MUSIC.ARC(WAV) → SOUND.ARC(AI5A = IMA ADPCM, 1/4 크기) Blob
 *   XpCore.buildCia(base, files, prog): 게임 파일 없는 nonoxp.cia 에 RomFS 를 붙인 새 CIA Blob
 */
(function (root) {
'use strict';
var BlobC = typeof Blob !== 'undefined' ? Blob : require('buffer').Blob;

/* ---------- SHA-256 (조각 단위로 이어서 계산) ---------- */
var K = new Uint32Array([
0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2]);
function Sha() { this.h = new Int32Array([0x6a09e667,0xbb67ae85|0,0x3c6ef372,0xa54ff53a|0,0x510e527f,0x9b05688c|0,0x1f83d9ab,0x5be0cd19]); this.buf = new Uint8Array(64); this.bl = 0; this.n = 0; this.w = new Int32Array(64); }
Sha.prototype.block = function (d, o) {
    var W = this.w, H = this.h, i;
    for (i = 0; i < 16; i++) W[i] = (d[o+i*4] << 24) | (d[o+i*4+1] << 16) | (d[o+i*4+2] << 8) | d[o+i*4+3];
    for (i = 16; i < 64; i++) { var x = W[i-15], y = W[i-2];
        W[i] = (W[i-16] + (((x>>>7)|(x<<25)) ^ ((x>>>18)|(x<<14)) ^ (x>>>3)) + W[i-7] + (((y>>>17)|(y<<15)) ^ ((y>>>19)|(y<<13)) ^ (y>>>10))) | 0; }
    var a=H[0],b=H[1],c=H[2],e0=H[3],e=H[4],f=H[5],g=H[6],h=H[7];
    for (i = 0; i < 64; i++) {
        var t1 = (h + (((e>>>6)|(e<<26)) ^ ((e>>>11)|(e<<21)) ^ ((e>>>25)|(e<<7))) + ((e & f) ^ (~e & g)) + K[i] + W[i]) | 0;
        var t2 = ((((a>>>2)|(a<<30)) ^ ((a>>>13)|(a<<19)) ^ ((a>>>22)|(a<<10))) + ((a & b) ^ (a & c) ^ (b & c))) | 0;
        h=g; g=f; f=e; e=(e0+t1)|0; e0=c; c=b; b=a; a=(t1+t2)|0;
    }
    H[0]=(H[0]+a)|0;H[1]=(H[1]+b)|0;H[2]=(H[2]+c)|0;H[3]=(H[3]+e0)|0;H[4]=(H[4]+e)|0;H[5]=(H[5]+f)|0;H[6]=(H[6]+g)|0;H[7]=(H[7]+h)|0;
};
Sha.prototype.update = function (d) {
    var i = 0, n = d.length; this.n += n;
    if (this.bl) { while (this.bl < 64 && i < n) this.buf[this.bl++] = d[i++]; if (this.bl === 64) { this.block(this.buf, 0); this.bl = 0; } }
    for (; i + 64 <= n; i += 64) this.block(d, i);
    while (i < n) this.buf[this.bl++] = d[i++];
    return this;
};
function sha256(d) { return new Sha().update(d).digest(); }
Sha.prototype.digest = function () {
    var n = this.n, bl = this.bl, padLen = (bl < 56 ? 64 : 128) - bl, pad = new Uint8Array(padLen);
    pad[0] = 0x80; var bits = n * 8;
    for (var k = 0; k < 8; k++) { pad[padLen - 1 - k] = bits % 256; bits = Math.floor(bits / 256); }
    this.update(pad); this.n -= padLen;
    var out = new Uint8Array(32);
    for (var j = 0; j < 8; j++) { var v = this.h[j]; out[j*4] = v >>> 24; out[j*4+1] = (v >>> 16) & 255; out[j*4+2] = (v >>> 8) & 255; out[j*4+3] = v & 255; }
    return out;
};

/* ---------- 작은 도구 ---------- */
function u16(b, o) { return b[o] | (b[o+1] << 8); }
function u32(b, o) { return (b[o] | (b[o+1] << 8) | (b[o+2] << 16) | (b[o+3] << 24)) >>> 0; }
function w32(b, o, v) { b[o] = v & 255; b[o+1] = (v >>> 8) & 255; b[o+2] = (v >>> 16) & 255; b[o+3] = (v >>> 24) & 255; }
function align(x, a) { return Math.ceil(x / a) * a; }
function zeros(n) { return new Uint8Array(n); }
function rd(blob, off, len) { return blob.slice(off, off + len).arrayBuffer().then(function (b) { return new Uint8Array(b); }); }
function tick() { return new Promise(function (r) { setTimeout(r, 0); }); }
/* blob 을 앞에서부터 chunk 단위로 읽으며 fn(조각, 위치) */
async function scan(blob, chunk, fn) {
    for (var p = 0; p < blob.size; p += chunk) { var d = await rd(blob, p, Math.min(chunk, blob.size - p)); await fn(d, p); }
}

/* ---------- 게임 파일 찾기 ---------- */
var WANT = ['MES.ARC', 'BG.ARC', 'DATA.ARC', 'MUSIC.ARC', 'VOICE.ARC'];
/* ISO9660 (2048 바이트 섹터 또는 2352 바이트 원시 섹터) 루트 폴더에서 ARC 파일을 꺼낸다 */
async function isoFiles(img) {
    var ss = 0, dataOff = 0;
    for (var cand of [[2048, 0], [2352, 16], [2352, 24]]) {
        var pvd = await rd(img, 16 * cand[0] + cand[1], 2048);
        if (pvd[0] === 1 && String.fromCharCode.apply(null, pvd.subarray(1, 6)) === 'CD001') { ss = cand[0]; dataOff = cand[1]; break; }
    }
    if (!ss) return null;
    var pvd2 = await rd(img, 16 * ss + dataOff, 2048);
    var rootLba = u32(pvd2, 156 + 2), rootLen = u32(pvd2, 156 + 10);
    function sectors(lba, len) {           /* 파일 내용: 2048 섹터면 그대로 Blob, 원시 섹터(2352)면 필요할 때 읽는 RawFile */
        if (ss === 2048) return img.slice(lba * 2048, lba * 2048 + len);
        return new RawFile(img, lba, len, ss, dataOff);
    }
    var dir = await rd(img, rootLba * ss + dataOff, 2048), out = {};
    /* 루트 폴더가 여러 섹터일 수 있음 */
    var dirAll = [];
    for (var k = 0; k * 2048 < rootLen; k++) dirAll.push(await rd(img, (rootLba + k) * ss + dataOff, 2048));
    for (var di = 0; di < dirAll.length; di++) { dir = dirAll[di];
        for (var p = 0; p < 2048 && dir[p];) { var l = dir[p], lba = u32(dir, p + 2), len = u32(dir, p + 10), nl = dir[p + 32], flags = dir[p + 25];
            var nm = String.fromCharCode.apply(null, dir.subarray(p + 33, p + 33 + nl)).replace(/;1$/, '').toUpperCase();
            if (!(flags & 2) && WANT.indexOf(nm) >= 0) out[nm] = sectors(lba, len);
            p += l; } }
    return out;
}
/* 원시 섹터 CD 이미지 속 파일. 섹터마다 Blob 조각을 만들면 브라우저가 멈추므로(조각 수십만 개) 큰 덩어리로 읽어 잘라 낸다.
   Blob 처럼 size / slice(a,b).arrayBuffer() 를 지원하고, toBlob() 은 내용을 4 MB 덩어리로 꺼내 진짜 Blob 을 만든다. */
function RawFile(img, lba, len, ss, dataOff, base) { this.img = img; this.lba = lba; this.size = len; this.ss = ss; this.dataOff = dataOff; this.base = base || 0; }
RawFile.prototype.slice = function (a, b) {
    if (b === undefined || b > this.size) b = this.size; if (a < 0) a = 0; if (b < a) b = a;
    var r = new RawFile(this.img, this.lba, b - a, this.ss, this.dataOff, this.base + a); return r;
};
RawFile.prototype.arrayBuffer = async function () {
    var out = new Uint8Array(this.size), pos = 0, off = this.base, ss = this.ss, CH = 512;   /* 512 섹터(약 1.2 MB)씩 읽기 */
    while (pos < this.size) {
        var sec = Math.floor(off / 2048), inSec = off % 2048, n = Math.min(CH, Math.ceil((inSec + this.size - pos) / 2048));
        var raw = new Uint8Array(await this.img.slice((this.lba + sec) * ss, (this.lba + sec + n) * ss).arrayBuffer());
        for (var k = 0; k < n && pos < this.size; k++) {
            var from = k * ss + this.dataOff + (k === 0 ? inSec : 0), take = Math.min(2048 - (k === 0 ? inSec : 0), this.size - pos);
            out.set(raw.subarray(from, from + take), pos); pos += take;
        }
        off = this.base + pos;
    }
    return out.buffer;
};
RawFile.prototype.toBlob = async function (prog) {
    var parts = [], CH = 4 << 20;
    for (var p = 0; p < this.size; p += CH) { parts.push(new Uint8Array(await this.slice(p, Math.min(this.size, p + CH)).arrayBuffer())); prog && prog((p + CH) / this.size); await tick(); }
    return new BlobC(parts);
};
async function asBlob(x, prog) { return x instanceof RawFile ? await x.toBlob(prog) : x; }

/* 분할 zip(Disc.z01 + Disc.zip, 무압축 저장)에서 첫 파일의 내용 Blob */
async function storedZipMember(parts) {
    var all = new BlobC(parts), h = await rd(all, 0, 64), o = 0;
    if (u32(h, 0) === 0x08074b50) o = 4;
    if (u32(h, o) !== 0x04034b50) return null;
    var method = u16(h, o + 8), csize = u32(h, o + 18), nlen = u16(h, o + 26), xlen = u16(h, o + 28);
    if (method !== 0) return null;   /* 압축된 zip 은 지원하지 않음(풀어서 이미지로 넣기) */
    var start = o + 30 + nlen + xlen;
    if (csize === 0xffffffff || csize === 0) csize = all.size - start;   /* zip64 또는 크기가 뒤(데이터 설명자)에 있는 분할 zip: 끝까지 (CD 이미지는 LBA 로 찾으므로 뒤에 붙은 zip 정보는 상관없음) */
    return all.slice(start, start + csize);
}
/* files: [{name, blob}] — 원본 게임 쪽 입력. 반환 { 'MES.ARC': Blob, ... } */
async function findGame(files, log) {
    var found = {}, z01 = [], zips = [];
    for (var f of files) { var up = f.name.toUpperCase();
        if (WANT.indexOf(up) >= 0) { found[up] = f.blob; continue; }
        if (/\.Z\d\d$/.test(up)) z01.push(f); else if (/\.ZIP$/.test(up)) zips.push(f);
        else if (/\.(IMG|ISO|BIN)$/.test(up)) { var iso = await isoFiles(f.blob); if (iso) { log && log(f.name + ': CD 이미지에서 ' + Object.keys(iso).join(', ')); Object.assign(found, Object.assign(iso, found)); } }
    }
    if (zips.length) {
        z01.sort(function (a, b) { return a.name.toUpperCase() < b.name.toUpperCase() ? -1 : 1; });
        var member = await storedZipMember(z01.map(function (f) { return f.blob; }).concat([zips[0].blob]));
        if (member) { var iso2 = await isoFiles(member); if (iso2) { log && log('분할 zip 안 CD 이미지에서 ' + Object.keys(iso2).join(', ')); for (var k in iso2) if (!found[k]) found[k] = iso2[k]; } }
        else log && log('zip 을 읽을 수 없습니다(압축된 zip). 풀어서 Disc.img 나 ARC 파일로 넣어 주세요.');
    }
    return found;
}

/* ---------- ARC (게임 아카이브) ---------- */
async function arcList(blob) {
    var h = await rd(blob, 0, 4), n = u32(h, 0), t = await rd(blob, 4, n * 28), e = [];
    for (var i = 0; i < n; i++) { var nm = ''; for (var c = 0; c < 20 && t[i*28+c]; c++) nm += String.fromCharCode(t[i*28+c]);
        e.push({ name: nm, size: (u32(t, i*28+20) ^ 0x15056109) >>> 0, off: (u32(t, i*28+24) ^ 0x32300645) >>> 0 }); }
    return e;
}
function arcBuild(entries) {      /* entries: [{name, data(Uint8Array|Blob)}] → Blob */
    var n = entries.length, tab = new Uint8Array(4 + n * 28), pos = 4 + n * 28; w32(tab, 0, n);
    entries.forEach(function (e, i) { var sz = e.data.size !== undefined ? e.data.size : e.data.length;
        for (var c = 0; c < e.name.length && c < 20; c++) tab[4 + i*28 + c] = e.name.charCodeAt(c);
        w32(tab, 4 + i*28 + 20, (sz ^ 0x15056109) >>> 0); w32(tab, 4 + i*28 + 24, (pos ^ 0x32300645) >>> 0); pos += sz; });
    return new BlobC([tab].concat(entries.map(function (e) { return e.data; })));
}

/* ---------- 음악: WAV → AI5A (IMA ADPCM) ---------- */
var STEP = [7,8,9,10,11,12,13,14,16,17,19,21,23,25,28,31,34,37,41,45,50,55,60,66,73,80,88,97,107,118,130,143,157,173,190,209,230,253,279,307,337,371,408,449,494,544,598,658,724,796,876,963,1060,1166,1282,1411,1552,1707,1878,2066,2272,2499,2749,3024,3327,3660,4026,4428,4871,5358,5894,6484,7132,7845,8630,9493,10442,11487,12635,13899,15289,16818,18500,20350,22385,24623,27086,29794,32767];
var IDX = [-1,-1,-1,-1,2,4,6,8,-1,-1,-1,-1,2,4,6,8];
var BS = 2048;
function wavToAdpcm(w) {
    if (u32(w, 0) !== 0x46464952) return null;
    var p = 12, ch = 0, rate = 0, bits = 0, fmt = 0, data = null, ls = 0, le = 0;
    while (p + 8 <= w.length) { var id = u32(w, p), n = u32(w, p + 4), c = p + 8;
        if (id === 0x20746d66) { fmt = u16(w, c); ch = u16(w, c + 2); rate = u32(w, c + 4); bits = u16(w, c + 14); }
        else if (id === 0x61746164) data = w.subarray(c, Math.min(c + n, w.length));
        else if (id === 0x6c706d73 && n >= 60 && u32(w, c + 28)) { ls = u32(w, c + 44); le = u32(w, c + 48) + 1; }
        p += 8 + n + (n & 1); }
    if (fmt !== 1 || bits !== 16 || !data || ch < 1 || ch > 2) return null;
    var frames = Math.floor(data.length / (2 * ch)), blocks = Math.ceil(frames / BS), bb = 4 * ch + BS / 2 * ch;
    var out = new Uint8Array(24 + blocks * bb), dv = new DataView(data.buffer, data.byteOffset, data.length);
    out.set([0x41, 0x49, 0x35, 0x41]); w32(out, 4, ls); w32(out, 8, le > frames ? frames : le); w32(out, 12, rate);
    out[16] = ch; out[17] = 0; out[18] = BS & 255; out[19] = BS >> 8; w32(out, 20, frames);
    var pred = [0, 0], idx = [0, 0];
    for (var b = 0; b < blocks; b++) {
        var o = 24 + b * bb;
        for (var c2 = 0; c2 < ch; c2++) {
            var f0 = b * BS; var first = f0 < frames ? dv.getInt16((f0 * ch + c2) * 2, true) : 0;
            pred[c2] = first;     /* 블록 시작마다 실제 값으로 다시 맞춤(오차가 쌓이지 않게) */
            out[o + c2 * 4] = first & 255; out[o + c2 * 4 + 1] = (first >> 8) & 255; out[o + c2 * 4 + 2] = idx[c2];
            var nb = o + 4 * ch + c2 * (BS / 2), P = pred[c2], I = idx[c2];
            for (var i = 0; i < BS; i++) {
                var f = f0 + i, s = f < frames ? dv.getInt16((f * ch + c2) * 2, true) : P;
                var st = STEP[I], diff = s - P, code = 0;
                if (diff < 0) { code = 8; diff = -diff; }
                var d = st >> 3;
                if (diff >= st) { code |= 4; diff -= st; d += st; }
                if (diff >= (st >> 1)) { code |= 2; diff -= st >> 1; d += st >> 1; }
                if (diff >= (st >> 2)) { code |= 1; d += st >> 2; }
                P += (code & 8) ? -d : d; if (P > 32767) P = 32767; if (P < -32768) P = -32768;
                I += IDX[code]; if (I < 0) I = 0; if (I > 88) I = 88;
                out[nb + (i >> 1)] |= code << ((i & 1) * 4);
            }
            pred[c2] = P; idx[c2] = I;
        }
    }
    return out;
}
async function musicToAdpcm(music, prog) {
    var list = await arcList(music), out = [], done = 0, total = 0;
    list.forEach(function (e) { total += e.size; });
    for (var e of list) {
        var w = await rd(music, e.off, e.size), a = wavToAdpcm(w);
        out.push({ name: e.name, data: new BlobC([a || w]) });
        done += e.size; prog && prog(done / total, e.name); await tick();
    }
    return arcBuild(out);
}

/* ---------- RomFS (루트 폴더 하나, Blob 조각) ---------- */
function hashCount(n) {
    if (n < 3) return 3; if (n < 19) return n | 1;
    while (n % 2 === 0 || n % 3 === 0 || n % 5 === 0 || n % 7 === 0 || n % 11 === 0 || n % 13 === 0 || n % 17 === 0) n++;
    return n;
}
function nameHash(parent, name) {
    var h = (parent ^ 123456789) >>> 0;
    for (var i = 0; i < name.length; i++) { h = ((h >>> 5) | (h << 27)) >>> 0; h = (h ^ name.charCodeAt(i)) >>> 0; }
    return h;
}
var BLK = 0x1000;
async function buildRomfs(files, prog) {   /* files: [{name, data: Blob|Uint8Array}] */
    var n = files.length, i;
    var sizeOf = function (d) { return d.size !== undefined ? d.size : d.length; };
    var fhc = hashCount(n), dhc = hashCount(1), metaOff = [], dataOff = [], mo = 0, dof = 0;
    for (i = 0; i < n; i++) { metaOff.push(mo); mo += 0x20 + align(files[i].name.length * 2, 4); dof = align(dof, 16); dataOff.push(dof); dof += sizeOf(files[i].data); }
    var dirHashOff = 0x28, dirMetaOff = dirHashOff + dhc * 4, fileHashOff = dirMetaOff + 0x18, fileMetaOff = fileHashOff + fhc * 4;
    var fileDataOff = align(fileMetaOff + mo, 16), l3size = fileDataOff + dof;
    var l3 = new Uint8Array(fileDataOff);
    [0x28, dirHashOff, dhc * 4, dirMetaOff, 0x18, fileHashOff, fhc * 4, fileMetaOff, mo, fileDataOff].forEach(function (v, k) { w32(l3, k * 4, v); });
    for (i = 0; i < dhc; i++) w32(l3, dirHashOff + i * 4, 0xffffffff);
    w32(l3, dirHashOff + (nameHash(0, '') % dhc) * 4, 0);
    w32(l3, dirMetaOff + 0, 0); w32(l3, dirMetaOff + 4, 0xffffffff); w32(l3, dirMetaOff + 8, 0xffffffff);
    w32(l3, dirMetaOff + 12, n ? 0 : 0xffffffff); w32(l3, dirMetaOff + 16, 0xffffffff); w32(l3, dirMetaOff + 20, 0);
    for (i = 0; i < fhc; i++) w32(l3, fileHashOff + i * 4, 0xffffffff);
    var parts = [l3], pos = fileDataOff;
    for (i = 0; i < n; i++) {
        var m = fileMetaOff + metaOff[i], nm = files[i].name, bucket = fileHashOff + (nameHash(0, nm) % fhc) * 4, sz = sizeOf(files[i].data);
        w32(l3, m, 0); w32(l3, m + 4, i + 1 < n ? metaOff[i + 1] : 0xffffffff);
        w32(l3, m + 8, dataOff[i]); w32(l3, m + 12, 0); w32(l3, m + 16, sz % 4294967296); w32(l3, m + 20, Math.floor(sz / 4294967296));
        w32(l3, m + 24, u32(l3, bucket)); w32(l3, bucket, metaOff[i]); w32(l3, m + 28, nm.length * 2);
        for (var c = 0; c < nm.length; c++) { l3[m + 32 + c * 2] = nm.charCodeAt(c) & 255; l3[m + 33 + c * 2] = nm.charCodeAt(c) >>> 8; }
        var at = fileDataOff + dataOff[i]; if (at > pos) parts.push(zeros(at - pos));
        parts.push(files[i].data); pos = at + sz;
    }
    var l3al = align(l3size, BLK); if (l3al > pos) parts.push(zeros(l3al - pos));
    var L3 = new BlobC(parts);
    var l2size = l3al / BLK * 0x20, l1size = align(l2size, BLK) / BLK * 0x20, msize = align(l1size, BLK) / BLK * 0x20;
    var L2 = new Uint8Array(align(l2size, BLK)), L1 = new Uint8Array(align(l1size, BLK));
    /* level 3 → level 2: 4 KB 마다 SHA-256 (8 MB 씩 읽음) */
    var k2 = 0;
    await scan(L3, 8 << 20, async function (d, p) {
        for (var b = 0; b < d.length; b += BLK) L2.set(sha256(d.subarray(b, b + BLK)), (k2++) * 0x20);
        prog && prog((p + d.length) / L3.size); await tick();
    });
    for (var b1 = 0, k1 = 0; b1 < l2size; b1 += BLK, k1++) L1.set(sha256(L2.subarray(b1, b1 + BLK)), k1 * 0x20);
    var l3pos = align(0x60 + msize, BLK), head = new Uint8Array(l3pos);
    for (var b0 = 0, k0 = 0; b0 < l1size; b0 += BLK, k0++) head.set(sha256(L1.subarray(b0, b0 + BLK)), 0x60 + k0 * 0x20);
    head.set([0x49, 0x56, 0x46, 0x43]); w32(head, 4, 0x10000); w32(head, 8, msize);
    var lo2 = align(l1size, BLK), lo3 = lo2 + align(l2size, BLK);
    w32(head, 0x0c, 0); w32(head, 0x14, l1size); w32(head, 0x1c, 12);
    w32(head, 0x24, lo2); w32(head, 0x2c, l2size); w32(head, 0x34, 12);
    w32(head, 0x3c, lo3); w32(head, 0x44, l3size % 4294967296); w32(head, 0x48, Math.floor(l3size / 4294967296)); w32(head, 0x4c, 12);
    w32(head, 0x54, 0x5c);
    return new BlobC([head, L3, L1, L2]);
}

/* ---------- RSA-2048 서명 (makerom 의 공개 시험용 키 — makerom -target t 와 같음) ---------- */
var TEST_N = 'cac588c7f12a092b7649c0a835751082c2b5e5b2e9c81888f39889bf9de6e40b715ddd3f138271f2ed318699d947fec57a7593e1f86dc63d9be11599e1c2e05c384b35a24d3ee2cefbb308a3dd0c2631849227c88a8ec883a86ca7a339719ef1349101df114a9cf98bf92f46440a7238f38b6d233389bf6634a786e6adf2def9ab16a140eed8f76cdc0092cb3149fc266424088fc660ff1ee3f0ddfb6d0d0f497cad03ec9f6358fa46dfa2640ecc8557e72c617f59b8627d590ef684969942b0398380b5522e073f92e39ef547eba7d7d415f1228232be2ad08c01cc30a91196f6e92bea0ef82d0db191d51a9451b98539b0af9f549e99e146e56fe25f4b4e23';
var TEST_D = '3e2bbeba7f290252bf1bf1e4212fd9761e39234a6dff99f633aa2b62030a0e15ac16b9856377f5742461b1016eeb72241e5dfa8fa85a101447bd05a07ee5ff60872a1831c1396cd545bb290504fb7aa268215fed4efe646069bd96d0a7063d537b6892885086ee065d72739a39b6723b200139df37281ef53963bc2af25eab1a99e45bebe636306c40016160cc55896dca7ee064787f7b26ae3ea3124516f6c8d0b94f91111211bbbb7fabc782dc4a619c14ae29fd3a601393192f5449b244345814d72f7025a048667655879b25776d0b75988ba639403c217f2a24c1a5c1dc5a5754f603f6ad5133406d5c265e299282e529137d7dfe0873bc5dc4e92bd671';
function hex(b, o, n) { var s = ''; for (var i = 0; i < n; i++) s += ('0' + b[o + i].toString(16)).slice(-2); return s; }
function modpow(b, e, m) { var r = BigInt(1); b %= m; while (e > BigInt(0)) { if (e & BigInt(1)) r = r * b % m; e >>= BigInt(1); b = b * b % m; } return r; }
function emsa(msg) { return '0001' + new Array(203).join('ff') + '003031300d060960864801650304020105000420' + hex(sha256(msg), 0, 32); }
function sigValid(buf, sigOff, msg) { var v = modpow(BigInt('0x' + hex(buf, sigOff, 0x100)), BigInt(65537), BigInt('0x' + TEST_N)).toString(16); return ('0000' + v).slice(-512) === emsa(msg); }
function sign(buf, sigOff, msg) {
    var v = modpow(BigInt('0x' + emsa(msg)), BigInt('0x' + TEST_D), BigInt('0x' + TEST_N)).toString(16); v = (new Array(513).join('0') + v).slice(-512);
    for (var i = 0; i < 0x100; i++) buf[sigOff + i] = parseInt(v.substr(i * 2, 2), 16);
}

/* ---------- CIA ---------- */
function a64(x) { return align(x, 64); }
function rd64(b, o) { return u32(b, o) + u32(b, o + 4) * 4294967296; }
function parseCia(cia) {
    if (cia.length < 0x2040 || u32(cia, 0) !== 0x2020) throw new Error('CIA 파일이 아닙니다');
    var c = { certSize: u32(cia, 8), tikSize: u32(cia, 12), tmdSize: u32(cia, 16), metaSize: u32(cia, 20), contentSize: rd64(cia, 24) };
    c.certOff = a64(0x2020); c.tikOff = a64(c.certOff + c.certSize); c.tmdOff = a64(c.tikOff + c.tikSize);
    c.contentOff = a64(c.tmdOff + c.tmdSize); c.metaOff = a64(c.contentOff + c.contentSize);
    var sigType = ((cia[c.tmdOff] << 24) | (cia[c.tmdOff + 1] << 16) | (cia[c.tmdOff + 2] << 8) | cia[c.tmdOff + 3]) >>> 0;
    var sigLen = { 0x10003: 0x240, 0x10004: 0x140, 0x10005: 0x80, 0x10000: 0x240, 0x10001: 0x140, 0x10002: 0x80 }[sigType];
    if (!sigLen) throw new Error('CIA 의 TMD 형식을 알 수 없습니다');
    c.tmdHdr = c.tmdOff + sigLen; c.contentCount = (cia[c.tmdHdr + 0x9e] << 8) | cia[c.tmdHdr + 0x9f];
    if (c.contentCount !== 1) throw new Error('콘텐츠가 하나인 CIA 만 쓸 수 있습니다');
    c.infoOff = c.tmdHdr + 0xc4; c.chunkOff = c.infoOff + 0x24 * 64;
    var n = c.contentOff;
    if (u32(cia, n + 0x100) !== 0x4843434e || !(cia[n + 0x18f] & 4)) throw new Error('암호화된 CIA 는 쓸 수 없습니다');
    c.romfsOff = u32(cia, n + 0x1b0) * 0x200; c.romfsSize = u32(cia, n + 0x1b4) * 0x200;
    c.exefsEnd = (u32(cia, n + 0x1a0) + u32(cia, n + 0x1a4)) * 0x200;
    return c;
}
async function buildCia(base, files, prog) {
    var c = parseCia(base), n = c.contentOff;
    for (var fi = 0; fi < files.length; fi++) if (files[fi].data instanceof RawFile) {
        var nm = files[fi].name; files[fi] = { name: nm, data: await files[fi].data.toBlob(function (f) { prog && prog(0.05 * Math.min(f, 1), 'CD 이미지에서 ' + nm + ' 꺼내는 중'); }) }; }
    var romfs = await buildRomfs(files, function (f) { prog && prog(0.05 + 0.6 * f, '데이터 확인값 계산'); });
    var keep = c.romfsSize ? c.romfsOff : c.contentSize;
    var romOff = align(keep, 0x1000), ncchSize = romOff + romfs.size, metaOff = a64(c.contentOff + ncchSize);
    var head = new Uint8Array(c.contentOff + romOff); head.set(base.subarray(0, c.contentOff + keep));
    var resign = false;
    try { resign = sigValid(base, n, base.subarray(n + 0x100, n + 0x200)) && sigValid(base, n + 0x600, base.subarray(n + 0x700, n + 0xa00)) && sigValid(base, c.tmdOff + 4, base.subarray(c.tmdHdr, c.tmdHdr + 0xc4)); } catch (e) { resign = false; }
    var sb = await rd(romfs, 0, 0x200);
    w32(head, n + 0x104, ncchSize / 0x200);
    w32(head, n + 0x1b0, romOff / 0x200); w32(head, n + 0x1b4, romfs.size / 0x200); w32(head, n + 0x1b8, 1);
    head.set(sha256(sb), n + 0x1e0);
    head[n + 0x18d] |= 1; head[n + 0x18f] &= ~2; head[n + 0x44f] &= ~1; head[n + 0x84f] &= ~1;
    if (resign) sign(head, n + 0x600, head.subarray(n + 0x700, n + 0xa00));
    head.set(sha256(head.subarray(n + 0x200, n + 0x600)), n + 0x160);
    if (resign) sign(head, n, head.subarray(n + 0x100, n + 0x200));
    /* 콘텐츠(NCCH 전체) 해시 */
    var hs = new Sha(); hs.update(head.subarray(n));
    await scan(romfs, 8 << 20, async function (d, p) { hs.update(d); prog && prog(0.65 + 0.33 * (p + d.length) / romfs.size, '설치 파일 확인값 계산'); await tick(); });
    var ch = c.chunkOff;
    head[ch + 8] = 0; head[ch + 9] = 0; head[ch + 10] = 0; head[ch + 11] = Math.floor(ncchSize / 4294967296) & 255;
    head[ch + 12] = (ncchSize >>> 24) & 255; head[ch + 13] = (ncchSize >>> 16) & 255; head[ch + 14] = (ncchSize >>> 8) & 255; head[ch + 15] = ncchSize & 255;
    head.set(hs.digest(), ch + 16);
    head.set(sha256(head.subarray(ch, ch + 0x30)), c.infoOff + 4);
    head.set(sha256(head.subarray(c.infoOff, c.infoOff + 0x24 * 64)), c.tmdHdr + 0xa4);
    if (resign) sign(head, c.tmdOff + 4, head.subarray(c.tmdHdr, c.tmdHdr + 0xc4));
    w32(head, 24, ncchSize % 4294967296); w32(head, 28, Math.floor(ncchSize / 4294967296));
    var tailPad = metaOff - (c.contentOff + ncchSize);
    prog && prog(1, '완료');
    return new BlobC([head, romfs, zeros(tailPad), base.subarray(c.metaOff, c.metaOff + c.metaSize)]);
}

var api = { RawFile: RawFile, asBlob: asBlob, Sha: Sha, sha256: sha256, findGame: findGame, isoFiles: isoFiles, arcList: arcList, arcBuild: arcBuild, wavToAdpcm: wavToAdpcm, musicToAdpcm: musicToAdpcm, buildRomfs: buildRomfs, parseCia: parseCia, buildCia: buildCia, Blob: BlobC };
if (typeof module !== 'undefined' && module.exports) module.exports = api;
root.XpCore = api;
})(typeof self !== 'undefined' ? self : this);
