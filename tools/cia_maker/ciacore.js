/* 노노무라 3DS — 게임 파일 내장 CIA 생성기 핵심부 (브라우저·node 공용, 외부 의존성 없음)
 *
 *   CiaCore.buildCia(baseCia, files)  : 게임 파일이 없는 nonomura.cia 에 RomFS 를 붙여 새 CIA 를 만든다
 *   CiaCore.parseZip(u8)              : zip → [{name, data}]
 *   CiaCore.pickGameFiles(entries)    : 폴더 구조를 걷어 내고 게임 폴더의 파일만 고른다
 */
(function (root) {
'use strict';

/* ---------- SHA-256 ---------- */
var K = new Uint32Array([
0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2]);
var W = new Int32Array(64);
function sha256(data) {
    var h0=0x6a09e667|0,h1=0xbb67ae85|0,h2=0x3c6ef372|0,h3=0xa54ff53a|0,h4=0x510e527f|0,h5=0x9b05688c|0,h6=0x1f83d9ab|0,h7=0x5be0cd19|0;
    var n = data.length, total = ((n + 9 + 63) >> 6) << 6;
    var tail = new Uint8Array(total - (n & ~63));
    tail.set(data.subarray(n & ~63));
    tail[n & 63] = 0x80;
    var bits = n * 8, tl = tail.length;
    tail[tl-1] = bits & 255; tail[tl-2] = (bits >>> 8) & 255; tail[tl-3] = (bits >>> 16) & 255; tail[tl-4] = (bits >>> 24) & 255;
    tail[tl-5] = Math.floor(n / 0x20000000) & 255;
    for (var off = 0; off < total; off += 64) {
        var src = off < (n & ~63) ? data : tail, so = off < (n & ~63) ? off : off - (n & ~63), i;
        for (i = 0; i < 16; i++) W[i] = (src[so+i*4] << 24) | (src[so+i*4+1] << 16) | (src[so+i*4+2] << 8) | src[so+i*4+3];
        for (i = 16; i < 64; i++) {
            var x = W[i-15], y = W[i-2];
            W[i] = (W[i-16] + (((x>>>7)|(x<<25)) ^ ((x>>>18)|(x<<14)) ^ (x>>>3)) + W[i-7] + (((y>>>17)|(y<<15)) ^ ((y>>>19)|(y<<13)) ^ (y>>>10))) | 0;
        }
        var a=h0,b=h1,c=h2,d=h3,e=h4,f=h5,g=h6,h=h7;
        for (i = 0; i < 64; i++) {
            var t1 = (h + (((e>>>6)|(e<<26)) ^ ((e>>>11)|(e<<21)) ^ ((e>>>25)|(e<<7))) + ((e & f) ^ (~e & g)) + K[i] + W[i]) | 0;
            var t2 = ((((a>>>2)|(a<<30)) ^ ((a>>>13)|(a<<19)) ^ ((a>>>22)|(a<<10))) + ((a & b) ^ (a & c) ^ (b & c))) | 0;
            h=g; g=f; f=e; e=(d+t1)|0; d=c; c=b; b=a; a=(t1+t2)|0;
        }
        h0=(h0+a)|0;h1=(h1+b)|0;h2=(h2+c)|0;h3=(h3+d)|0;h4=(h4+e)|0;h5=(h5+f)|0;h6=(h6+g)|0;h7=(h7+h)|0;
    }
    var out = new Uint8Array(32), hs = [h0,h1,h2,h3,h4,h5,h6,h7];
    for (var j = 0; j < 8; j++) { out[j*4]=hs[j]>>>24; out[j*4+1]=(hs[j]>>>16)&255; out[j*4+2]=(hs[j]>>>8)&255; out[j*4+3]=hs[j]&255; }
    return out;
}

/* ---------- inflate (RFC 1951) ---------- */
var LBASE=[3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258];
var LEXT =[0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0];
var DBASE=[1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577];
var DEXT =[0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13];
var CLORD=[16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15];
function mkHuff(lens, off, n) {
    var count = new Uint16Array(16), sym = new Uint16Array(n), offs = new Uint16Array(16), i;
    for (i = 0; i < n; i++) count[lens[off+i]]++;
    count[0] = 0;
    for (i = 1; i < 16; i++) offs[i] = offs[i-1] + count[i-1];
    for (i = 0; i < n; i++) if (lens[off+i]) sym[offs[lens[off+i]]++] = i;
    return { count: count, sym: sym };
}
function inflate(src, outSize) {
    var out = new Uint8Array(outSize), op = 0, ip = 0, bitbuf = 0, bitcnt = 0;
    function bits(n) {
        while (bitcnt < n) { if (ip >= src.length) throw new Error('압축 데이터가 중간에 끊겼습니다'); bitbuf |= src[ip++] << bitcnt; bitcnt += 8; }
        var v = bitbuf & ((1 << n) - 1); bitbuf >>>= n; bitcnt -= n; return v;
    }
    function decode(h) {
        var code = 0, first = 0, index = 0;
        for (var len = 1; len < 16; len++) {
            code |= bits(1);
            var c = h.count[len];
            if (code - c < first) return h.sym[index + (code - first)];
            index += c; first += c; first <<= 1; code <<= 1;
        }
        throw new Error('압축 데이터가 손상되었습니다');
    }
    var fixL = null, fixD = null, last;
    do {
        last = bits(1);
        var type = bits(2);
        if (type === 0) {
            bitbuf = 0; bitcnt = 0;
            var len = src[ip] | (src[ip+1] << 8); ip += 4;
            if (ip + len > src.length || op + len > outSize) throw new Error('압축 데이터가 손상되었습니다');
            out.set(src.subarray(ip, ip + len), op); op += len; ip += len;
        } else if (type === 1 || type === 2) {
            var hl, hd, i;
            if (type === 1) {
                if (!fixL) {
                    var fl = new Uint8Array(320);
                    for (i = 0; i < 144; i++) fl[i] = 8;
                    for (; i < 256; i++) fl[i] = 9;
                    for (; i < 280; i++) fl[i] = 7;
                    for (; i < 288; i++) fl[i] = 8;
                    for (i = 288; i < 318; i++) fl[i] = 5;
                    fixL = mkHuff(fl, 0, 288); fixD = mkHuff(fl, 288, 30);
                }
                hl = fixL; hd = fixD;
            } else {
                var nl = bits(5) + 257, nd = bits(5) + 1, nc = bits(4) + 4, lens = new Uint8Array(320);
                for (i = 0; i < nc; i++) lens[CLORD[i]] = bits(3);
                var hc = mkHuff(lens, 0, 19);
                lens = new Uint8Array(320);
                for (i = 0; i < nl + nd;) {
                    var s = decode(hc);
                    if (s < 16) lens[i++] = s;
                    else {
                        var prev = 0, rep;
                        if (s === 16) { if (!i) throw new Error('압축 데이터가 손상되었습니다'); prev = lens[i-1]; rep = 3 + bits(2); }
                        else if (s === 17) rep = 3 + bits(3);
                        else rep = 11 + bits(7);
                        if (i + rep > nl + nd) throw new Error('압축 데이터가 손상되었습니다');
                        while (rep--) lens[i++] = prev;
                    }
                }
                hl = mkHuff(lens, 0, nl); hd = mkHuff(lens, nl, nd);
            }
            for (;;) {
                var sy = decode(hl);
                if (sy < 256) { if (op >= outSize) throw new Error('압축 데이터가 손상되었습니다'); out[op++] = sy; }
                else if (sy === 256) break;
                else {
                    sy -= 257;
                    if (sy >= 29) throw new Error('압축 데이터가 손상되었습니다');
                    var ln = LBASE[sy] + bits(LEXT[sy]);
                    var ds = decode(hd);
                    if (ds >= 30) throw new Error('압축 데이터가 손상되었습니다');
                    var dist = DBASE[ds] + bits(DEXT[ds]);
                    if (dist > op || op + ln > outSize) throw new Error('압축 데이터가 손상되었습니다');
                    for (var k = 0; k < ln; k++, op++) out[op] = out[op - dist];
                }
            }
        } else throw new Error('압축 데이터가 손상되었습니다');
    } while (!last);
    if (op !== outSize) throw new Error('압축을 푼 크기가 맞지 않습니다');
    return out;
}

/* ---------- zip ---------- */
function u16(b, o) { return b[o] | (b[o+1] << 8); }
function u32(b, o) { return (b[o] | (b[o+1] << 8) | (b[o+2] << 16) | (b[o+3] << 24)) >>> 0; }
function w32(b, o, v) { b[o] = v & 255; b[o+1] = (v >>> 8) & 255; b[o+2] = (v >>> 16) & 255; b[o+3] = (v >>> 24) & 255; }
function isZip(b) { return b.length > 22 && b[0] === 0x50 && b[1] === 0x4b && (b[2] === 3 || b[2] === 5) ; }
function parseZip(b) {
    var e = -1;
    for (var i = b.length - 22; i >= 0 && i >= b.length - 22 - 65535; i--) if (u32(b, i) === 0x06054b50) { e = i; break; }
    if (e < 0) throw new Error('zip 파일이 아니거나 손상되었습니다');
    var n = u16(b, e + 10), p = u32(b, e + 16), out = [];
    for (var k = 0; k < n; k++) {
        if (p + 46 > b.length || u32(b, p) !== 0x02014b50) throw new Error('zip 목록이 손상되었습니다');
        var flag = u16(b, p + 8), method = u16(b, p + 10), csize = u32(b, p + 20), usize = u32(b, p + 24);
        var nl = u16(b, p + 28), xl = u16(b, p + 30), cl = u16(b, p + 32), lho = u32(b, p + 42);
        var name = '', ascii = true;
        for (var j = 0; j < nl; j++) { var c = b[p + 46 + j]; if (c >= 0x80) ascii = false; name += String.fromCharCode(c); }
        p += 46 + nl + xl + cl;
        if (!ascii && typeof TextDecoder !== 'undefined') { try { name = new TextDecoder('utf-8').decode(b.subarray(p - nl - xl - cl, p - xl - cl)); } catch (x) {} }
        name = name.replace(/\\/g, '/');
        if (name.charAt(name.length - 1) === '/') continue;                 /* 폴더 */
        if (flag & 1) throw new Error('암호가 걸린 zip 은 쓸 수 없습니다');
        if (csize === 0xffffffff || usize === 0xffffffff || lho === 0xffffffff) throw new Error('4GB 넘는 zip 은 쓸 수 없습니다');
        if (u32(b, lho) !== 0x04034b50) throw new Error('zip 내용이 손상되었습니다');
        var ds = lho + 30 + u16(b, lho + 26) + u16(b, lho + 28);
        if (ds + csize > b.length) throw new Error('zip 내용이 손상되었습니다');
        var raw = b.subarray(ds, ds + csize), data;
        if (method === 0) data = new Uint8Array(raw);
        else if (method === 8) data = inflate(raw, usize);
        else throw new Error('지원하지 않는 zip 압축 방식입니다 (' + name + ')');
        out.push({ name: name, data: data, ascii: ascii });
    }
    return out;
}

/* 폴더 구조를 걷어 내고 게임 폴더(SILV1.COM 이 있는 곳)의 파일만 남긴다.
 * SILV1.COM 이 없으면(번역 파일 묶음 등) 파일이 가장 많은 폴더를 쓴다. */
function pickGameFiles(entries) {
    var dirs = {}, anchor = null, best = -1, skipped = [];
    entries.forEach(function (e) {
        var i = e.name.lastIndexOf('/'), d = i < 0 ? '' : e.name.slice(0, i + 1), base = e.name.slice(i + 1);
        (dirs[d] = dirs[d] || []).push({ name: base, data: e.data, ascii: e.ascii !== false });
        if (base.toUpperCase() === 'SILV1.COM' && (anchor === null || d.length < anchor.length)) anchor = d;
    });
    if (anchor === null) for (var d in dirs) if (anchor === null || dirs[d].length > best || (dirs[d].length === best && d.length < anchor.length)) { best = dirs[d].length; anchor = d; }
    var files = [];
    (dirs[anchor] || []).forEach(function (f) {
        var up = f.name.toUpperCase();
        if (!f.ascii || !/^[\x21-\x7e]{1,12}$/.test(up) || /[\/\\:*?"<>|]/.test(up)) { skipped.push(f.name); return; }
        files.push({ name: up, data: f.data });
    });
    for (var d2 in dirs) if (d2 !== anchor) dirs[d2].forEach(function (f) { skipped.push(d2 + f.name); });
    return { files: files, skipped: skipped };
}

/* ---------- RomFS ---------- */
function align(x, a) { return Math.ceil(x / a) * a; }
function hashCount(n) {
    if (n < 3) return 3;
    if (n < 19) return n | 1;
    while (n % 2 === 0 || n % 3 === 0 || n % 5 === 0 || n % 7 === 0 || n % 11 === 0 || n % 13 === 0 || n % 17 === 0) n++;
    return n;
}
function nameHash(parent, name) {
    var h = (parent ^ 123456789) >>> 0;
    for (var i = 0; i < name.length; i++) { h = ((h >>> 5) | (h << 27)) >>> 0; h = (h ^ name.charCodeAt(i)) >>> 0; }
    return h;
}
var BLK = 0x1000;
/* files: [{name, data}] — 루트 폴더 하나에 모두 넣는다 */
function buildRomfs(files) {
    var n = files.length, i;
    var fhc = hashCount(n), dhc = hashCount(1);
    var metaOff = [], dataOff = [], mo = 0, dof = 0;
    for (i = 0; i < n; i++) {
        metaOff.push(mo); mo += 0x20 + align(files[i].name.length * 2, 4);
        dof = align(dof, 16); dataOff.push(dof); dof += files[i].data.length;
    }
    var dirHashOff = 0x28, dirMetaOff = dirHashOff + dhc * 4, fileHashOff = dirMetaOff + 0x18, fileMetaOff = fileHashOff + fhc * 4;
    var fileDataOff = align(fileMetaOff + mo, 16);
    var l3size = fileDataOff + dof;
    var l2size = align(l3size, BLK) / BLK * 0x20, l1size = align(l2size, BLK) / BLK * 0x20, msize = align(l1size, BLK) / BLK * 0x20;
    var l3pos = align(0x60 + msize, BLK), l1pos = l3pos + align(l3size, BLK), l2pos = l1pos + align(l1size, BLK);
    var total = l2pos + align(l2size, BLK);
    var r = new Uint8Array(total), l3 = r.subarray(l3pos);

    /* level 3 */
    [0x28, dirHashOff, dhc * 4, dirMetaOff, 0x18, fileHashOff, fhc * 4, fileMetaOff, mo, fileDataOff].forEach(function (v, k) { w32(l3, k * 4, v); });
    for (i = 0; i < dhc; i++) w32(l3, dirHashOff + i * 4, 0xffffffff);
    w32(l3, dirHashOff + (nameHash(0, '') % dhc) * 4, 0);
    w32(l3, dirMetaOff + 0, 0); w32(l3, dirMetaOff + 4, 0xffffffff); w32(l3, dirMetaOff + 8, 0xffffffff);
    w32(l3, dirMetaOff + 12, n ? 0 : 0xffffffff); w32(l3, dirMetaOff + 16, 0xffffffff); w32(l3, dirMetaOff + 20, 0);
    for (i = 0; i < fhc; i++) w32(l3, fileHashOff + i * 4, 0xffffffff);
    for (i = 0; i < n; i++) {
        var m = fileMetaOff + metaOff[i], nm = files[i].name, bucket = fileHashOff + (nameHash(0, nm) % fhc) * 4;
        w32(l3, m, 0); w32(l3, m + 4, i + 1 < n ? metaOff[i + 1] : 0xffffffff);
        w32(l3, m + 8, dataOff[i]); w32(l3, m + 12, 0);
        w32(l3, m + 16, files[i].data.length); w32(l3, m + 20, 0);
        w32(l3, m + 24, u32(l3, bucket)); w32(l3, bucket, metaOff[i]);
        w32(l3, m + 28, nm.length * 2);
        for (var c = 0; c < nm.length; c++) { l3[m + 32 + c * 2] = nm.charCodeAt(c) & 255; l3[m + 33 + c * 2] = nm.charCodeAt(c) >>> 8; }
        l3.set(files[i].data, fileDataOff + dataOff[i]);
    }

    /* 해시 단계 */
    function hashLevel(srcPos, srcSize, dstPos) {
        for (var b = 0, k = 0; b < srcSize; b += BLK, k++) r.set(sha256(r.subarray(srcPos + b, srcPos + b + BLK)), dstPos + k * 0x20);
    }
    hashLevel(l3pos, l3size, l2pos);
    hashLevel(l2pos, l2size, l1pos);
    hashLevel(l1pos, l1size, 0x60);

    /* IVFC 머리 */
    r[0] = 0x49; r[1] = 0x56; r[2] = 0x46; r[3] = 0x43; w32(r, 4, 0x10000); w32(r, 8, msize);
    var lo1 = 0, lo2 = align(l1size, BLK), lo3 = lo2 + align(l2size, BLK);
    w32(r, 0x0c, lo1); w32(r, 0x14, l1size); w32(r, 0x1c, 12);
    w32(r, 0x24, lo2); w32(r, 0x2c, l2size); w32(r, 0x34, 12);
    w32(r, 0x3c, lo3); w32(r, 0x44, l3size); w32(r, 0x4c, 12);
    w32(r, 0x54, 0x5c);
    return r;
}

/* ---------- RSA-2048 서명 (makerom 의 공개된 시험용 키 — makerom -target t 가 쓰는 것과 같다) ---------- */
var TEST_N = 'cac588c7f12a092b7649c0a835751082c2b5e5b2e9c81888f39889bf9de6e40b715ddd3f138271f2ed318699d947fec57a7593e1f86dc63d9be11599e1c2e05c384b35a24d3ee2cefbb308a3dd0c2631849227c88a8ec883a86ca7a339719ef1349101df114a9cf98bf92f46440a7238f38b6d233389bf6634a786e6adf2def9ab16a140eed8f76cdc0092cb3149fc266424088fc660ff1ee3f0ddfb6d0d0f497cad03ec9f6358fa46dfa2640ecc8557e72c617f59b8627d590ef684969942b0398380b5522e073f92e39ef547eba7d7d415f1228232be2ad08c01cc30a91196f6e92bea0ef82d0db191d51a9451b98539b0af9f549e99e146e56fe25f4b4e23';
var TEST_D = '3e2bbeba7f290252bf1bf1e4212fd9761e39234a6dff99f633aa2b62030a0e15ac16b9856377f5742461b1016eeb72241e5dfa8fa85a101447bd05a07ee5ff60872a1831c1396cd545bb290504fb7aa268215fed4efe646069bd96d0a7063d537b6892885086ee065d72739a39b6723b200139df37281ef53963bc2af25eab1a99e45bebe636306c40016160cc55896dca7ee064787f7b26ae3ea3124516f6c8d0b94f91111211bbbb7fabc782dc4a619c14ae29fd3a601393192f5449b244345814d72f7025a048667655879b25776d0b75988ba639403c217f2a24c1a5c1dc5a5754f603f6ad5133406d5c265e299282e529137d7dfe0873bc5dc4e92bd671';
var HAVE_BIG = typeof BigInt !== 'undefined';
function hex(b, o, n) { var s = ''; for (var i = 0; i < n; i++) s += ('0' + b[o + i].toString(16)).slice(-2); return s; }
function modpow(b, e, m) { var r = BigInt(1); b %= m; while (e > BigInt(0)) { if (e & BigInt(1)) r = r * b % m; e >>= BigInt(1); b = b * b % m; } return r; }
function emsa(msg) { return '0001' + new Array(203).join('ff') + '003031300d060960864801650304020105000420' + hex(sha256(msg), 0, 32); }
function sigValid(buf, sigOff, msg) {
    var v = modpow(BigInt('0x' + hex(buf, sigOff, 0x100)), BigInt(65537), BigInt('0x' + TEST_N)).toString(16);
    return ('0000' + v).slice(-512) === emsa(msg);
}
function sign(buf, sigOff, msg) {
    var v = modpow(BigInt('0x' + emsa(msg)), BigInt('0x' + TEST_D), BigInt('0x' + TEST_N)).toString(16);
    v = (new Array(513).join('0') + v).slice(-512);
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
    if (c.metaOff + c.metaSize > cia.length) throw new Error('CIA 파일이 잘렸습니다');
    var sigType = ((cia[c.tmdOff] << 24) | (cia[c.tmdOff + 1] << 16) | (cia[c.tmdOff + 2] << 8) | cia[c.tmdOff + 3]) >>> 0;
    var sigLen = { 0x10003: 0x240, 0x10004: 0x140, 0x10005: 0x80, 0x10000: 0x240, 0x10001: 0x140, 0x10002: 0x80 }[sigType];
    if (!sigLen) throw new Error('CIA 의 TMD 형식을 알 수 없습니다');
    c.tmdHdr = c.tmdOff + sigLen;
    c.contentCount = (cia[c.tmdHdr + 0x9e] << 8) | cia[c.tmdHdr + 0x9f];
    if (c.contentCount !== 1) throw new Error('콘텐츠가 하나인 CIA 만 쓸 수 있습니다');
    c.infoOff = c.tmdHdr + 0xc4; c.chunkOff = c.infoOff + 0x24 * 64;
    var n = c.contentOff;
    if (u32(cia, n + 0x100) !== 0x4843434e) throw new Error('CIA 내용이 암호화되어 있어 쓸 수 없습니다');
    if (!(cia[n + 0x18f] & 4)) throw new Error('암호화된 NCCH 는 쓸 수 없습니다');
    if (u32(cia, n + 0x180) !== 0x400) throw new Error('CIA 구조를 알 수 없습니다');
    c.titleId = ''; for (var i = 0; i < 8; i++) c.titleId += ('0' + cia[c.tmdHdr + 0x4c + i].toString(16)).slice(-2);
    c.romfsOff = u32(cia, n + 0x1b0) * 0x200; c.romfsSize = u32(cia, n + 0x1b4) * 0x200;
    c.exefsEnd = (u32(cia, n + 0x1a0) + u32(cia, n + 0x1a4)) * 0x200;
    return c;
}
function buildCia(base, files) {
    if (!files.length) throw new Error('게임 파일이 없습니다');
    var c = parseCia(base);
    var romfs = buildRomfs(files);
    var keep = c.romfsSize ? c.romfsOff : c.contentSize;           /* 이미 RomFS 가 있으면 떼어 낸다 */
    if (keep < c.exefsEnd) throw new Error('CIA 구조를 알 수 없습니다');
    var romOff = align(keep, 0x1000), ncchSize = romOff + romfs.length;
    var metaOff = a64(c.contentOff + ncchSize);
    var out = new Uint8Array(metaOff + c.metaSize);
    out.set(base.subarray(0, c.contentOff + keep));
    out.set(romfs, c.contentOff + romOff);
    out.set(base.subarray(c.metaOff, c.metaOff + c.metaSize), metaOff);

    /* 서명을 다시 할 수 있는지: 원본의 서명 세 개가 모두 시험용 키로 된 것일 때만 */
    var n = c.contentOff, resign = false;
    if (HAVE_BIG) {
        try {
            resign = sigValid(base, n, base.subarray(n + 0x100, n + 0x200)) &&
                     sigValid(base, n + 0x600, base.subarray(n + 0x700, n + 0xa00)) &&
                     sigValid(base, c.tmdOff + 4, base.subarray(c.tmdHdr, c.tmdHdr + 0xc4));
        } catch (e) { resign = false; }
    }

    /* NCCH */
    w32(out, n + 0x104, ncchSize / 0x200);
    w32(out, n + 0x1b0, romOff / 0x200); w32(out, n + 0x1b4, romfs.length / 0x200); w32(out, n + 0x1b8, 1);
    out.set(sha256(romfs.subarray(0, 0x200)), n + 0x1e0);
    out[n + 0x18d] |= 1;            /* 내용 종류: 데이터 있음 */
    out[n + 0x18f] &= ~2;           /* RomFS 없음 표시 해제 */
    out[n + 0x44f] &= ~1; out[n + 0x84f] &= ~1;                    /* 확장 머리의 'RomFS 안 씀' 표시 해제 */
    if (resign) sign(out, n + 0x600, out.subarray(n + 0x700, n + 0xa00));
    out.set(sha256(out.subarray(n + 0x200, n + 0x600)), n + 0x160); /* 확장 머리 해시 */
    if (resign) sign(out, n, out.subarray(n + 0x100, n + 0x200));

    /* TMD: 내용 크기·해시 → 기록 해시 → 머리 해시 → 서명 */
    var ch = c.chunkOff;
    out[ch + 8] = 0; out[ch + 9] = 0; out[ch + 10] = 0; out[ch + 11] = Math.floor(ncchSize / 4294967296) & 255;
    out[ch + 12] = (ncchSize >>> 24) & 255; out[ch + 13] = (ncchSize >>> 16) & 255; out[ch + 14] = (ncchSize >>> 8) & 255; out[ch + 15] = ncchSize & 255;
    out.set(sha256(out.subarray(n, n + ncchSize)), ch + 16);
    out.set(sha256(out.subarray(ch, ch + 0x30)), c.infoOff + 4);
    out.set(sha256(out.subarray(c.infoOff, c.infoOff + 0x24 * 64)), c.tmdHdr + 0xa4);
    if (resign) sign(out, c.tmdOff + 4, out.subarray(c.tmdHdr, c.tmdHdr + 0xc4));

    /* CIA 머리 */
    w32(out, 24, ncchSize >>> 0); w32(out, 28, Math.floor(ncchSize / 4294967296));
    return out;
}

var api = { sha256: sha256, inflate: inflate, isZip: isZip, parseZip: parseZip, pickGameFiles: pickGameFiles, buildRomfs: buildRomfs, parseCia: parseCia, buildCia: buildCia };
if (typeof module !== 'undefined' && module.exports) module.exports = api;
root.CiaCore = api;
})(typeof self !== 'undefined' ? self : this);
