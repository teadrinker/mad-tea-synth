#ifndef VM_EMIT_SCRIPT_PRELUDE_H
#define VM_EMIT_SCRIPT_PRELUDE_H

// The runtime halves of the two script dialects: everything the emitted code
// calls that is not the script's own. Kept out of vm_emit_script.c because it
// is source text in another language, and reads better unbroken by C.
//
// ONE ENTRY PER HELPER, and only the ones a module actually reaches get
// emitted. The walker marks a helper as it writes the call (ss_call in
// vm_emit_script.c, which is the only way to spell one, so a new call site
// cannot forget), then ss_emit_prelude closes the set over `deps` and prints
// the survivors in table order. A `return 42` gets two lines of prelude; a DSP
// body that formats strings gets what it needs and nothing else.
//
// Four rules for editing this table:
//
//  1. **A helper's deps must have LOWER ids than it does.** Both the
//     dependency closure (one reverse pass) and the emission order (one
//     forward pass) rely on it, and neither checks.
//  2. `js` or `lua` may be NULL when a dialect has no such helper -- JS gets
//     slices from `subarray` and needs none of Lua's view builders; Lua's
//     integers are 64-bit and need none of JS's. A helper is never marked in a
//     dialect that has no text for it.
//  3. The fmt_* family is a port of vm/vm_fmt.h -- the interpreter's copy --
//     and of the EC_FMT_* block in vm_emit_c.c. All four must agree
//     character-for-character in OUTPUT, which is what makes differential
//     testing possible at all. Change one, change the others.
//  4. Lua tables here are 0-BASED. `#`, `ipairs`, `table.concat` over the
//     element range and `table.unpack` are all wrong on them.
//
// `alias` is the f32 variant name the C backend would call (`m_sinf` for
// `m_sin`). The two share one body -- the walker wraps the RESULT in
// Math.fround wherever the call's type is f32, which is exactly what returning
// a float from the C variant does -- so the chunk defines both names and the
// name lookup accepts either.
//
// NOTE: sin/cos/exp/log/pow/sqrt/atan2 map to the HOST's math library, not to
// common/math_pure.c's polynomial approximations, which is what the
// interpreter and emitted C use. They agree to a few ulp, not bit for bit.
// Everything else here is an exact port.

#define SS_MAX_HELPER_DEPS 5

// Helper ids. Order IS the emission order, and deps must point backwards.
enum {
    H_BCBUF,            // not a VM field: the module-scope scratch buffer the
                        // JS bitcasts pun through
    H_W32,
    H_TRUNC,
    H_F2I,
    H_F2L,
    H_TDIV,
    H_TMOD,
    H_FDIV,
    H_SHL32,
    H_SHL64,
    H_SAR32,
    H_SAR64,
    H_BC_I32_F32,
    H_BC_F32_I32,
    H_BC_I64_F64,
    H_BC_F64_I64,
    H_PACK_GET,
    H_PACK_SET,
    H_SLICE_AT,
    H_SLICE_N,
    H_SUB,
    H_SUBU,
    H_VIEW,
    H_EMPTY,
    H_SUBA,
    H_SUBAU,
    H_SUBS,
    H_SUBSU,
    H_ZEROS,
    H_ZEROSF,
    H_ACOPY,
    H_BYTES,
    H_STR,
    H_PRINT,
    H_FMT_BYTES,
    H_FMT_UDEC,
    H_FMT_I32,
    H_FMT_I64,
    H_FMT_FX,
    H_FMT_F64,
    H_M_FLOOR,
    H_M_CEIL,
    H_M_ROUND,
    H_M_FRAC,
    H_M_FABS,
    H_M_FMOD,
    H_M_FMIN,
    H_M_FMAX,
    H_M_CLAMP,
    H_M_MIX,
    H_M_IABS,
    H_M_IMIN,
    H_M_IMAX,
    H_M_ICLAMP,
    H_M_IPOW,
    H_M_LINEARSTEP,
    H_M_SMOOTHSTEP,
    H_M_SMOOTHERSTEP,
    H_M_LINEARSTEPA,
    H_M_SMOOTHSTEPA,
    H_M_SMOOTHERSTEPA,
    H_M_SIN,
    H_M_COS,
    H_M_TAN,
    H_M_ASIN,
    H_M_ACOS,
    H_M_ATAN,
    H_M_ATAN2,
    H_M_EXP,
    H_M_LOG,
    H_M_LOG2,
    H_M_LOG10,
    H_M_POW,
    H_M_SQRT,
    H_M_CBRT,
    H_M_SIN01,
    H_M_COS01,
    SS_N_HELPERS
};

typedef struct {
    const char *name;    // the VM field: "w32", "m_sin". NULL = not addressable
    const char *alias;   // f32 variant sharing one body ("m_sinf"), or NULL
    const char *js;      // NULL when this dialect has no such helper
    const char *lua;
    short       deps[SS_MAX_HELPER_DEPS];   // -1 terminated; ids below this one
} ScriptHelper;

#define SS_NO_DEPS   { -1, -1, -1, -1, -1 }
#define SS_DEPS1(a)  { (a), -1, -1, -1, -1 }
#define SS_DEPS2(a,b) { (a), (b), -1, -1, -1 }
#define SS_DEPS3(a,b,c) { (a), (b), (c), -1, -1 }
#define SS_DEPS4(a,b,c,d) { (a), (b), (c), (d), -1 }

static const ScriptHelper SCRIPT_HELPERS[SS_N_HELPERS] = {

// ---- integer semantics -----------------------------------------------------

[H_BCBUF] = { 0, 0,
"const __bcbuf = new ArrayBuffer(8);\n"
"const __bci32 = new Int32Array(__bcbuf);\n"
"const __bcf32 = new Float32Array(__bcbuf);\n",
0, SS_NO_DEPS },

// Lua integers are 64-bit, so an i32 has to be masked back down and
// sign-extended by hand after every arithmetic node. JS gets it from `| 0`.
[H_W32] = { "w32", 0,
"VM.w32 = function (x) { return x | 0; };\n",
"function VM.w32(x)\n"
"    x = x & 0xFFFFFFFF\n"
"    if x >= 0x80000000 then x = x - 0x100000000 end\n"
"    return x\n"
"end\n", SS_NO_DEPS },

[H_TRUNC] = { "trunc", 0,
"VM.trunc = function (x) { return x < 0 ? Math.ceil(x) : Math.floor(x); };\n",
"function VM.trunc(x)\n"
"    if x >= 0 then return math.floor(x) end\n"
"    return -math.floor(-x)\n"
"end\n", SS_NO_DEPS },

// A float to an integer saturates, nan giving 0, as the VM's cvt_to does it --
// `| 0` and a wrapped trunc would take out-of-range values mod 2^32 instead.
[H_F2I] = { "f2i", 0,
"VM.f2i = function (v) { return v !== v ? 0 : v >= 2147483647 ? 2147483647 : v <= -2147483648 ? -2147483648 : (v | 0); };\n",
"function VM.f2i(v)\n"
"    if v ~= v then return 0 end\n"
"    if v >= 2147483647 then return 2147483647 end\n"
"    if v <= -2147483648 then return -2147483648 end\n"
"    return math.tointeger(VM.trunc(v))\n"
"end\n", SS_DEPS1(H_TRUNC) },

[H_F2L] = { "f2l", 0, 0,
"function VM.f2l(v)\n"
"    if v ~= v then return 0 end\n"
"    if v >= 9.2233720368547758e18 then return math.maxinteger end\n"
"    if v <= -9.2233720368547758e18 then return math.mininteger end\n"
"    return math.tointeger(VM.trunc(v))\n"
"end\n", SS_DEPS1(H_TRUNC) },

// Lua's // FLOORS where C truncates, and integer //0 RAISES rather than
// returning garbage -- so both div and mod go through a helper even when the
// script disabled the zero guard. A raise is not a wrong number, it is a dead
// process.
[H_TDIV] = { "tdiv", 0,
"VM.tdiv = function (a, b) { return b === 0 ? 0 : (a / b) | 0; };\n",
"function VM.tdiv(a, b)\n"
"    if b == 0 then return 0 end\n"
"    local q = a // b\n"
"    if q < 0 and q * b ~= a then q = q + 1 end\n"
"    return q\n"
"end\n", SS_NO_DEPS },

[H_TMOD] = { "tmod", 0,
"VM.tmod = function (a, b) { return b === 0 ? 0 : (a % b) | 0; };\n",
"function VM.tmod(a, b)\n"
"    if b == 0 then return 0 end\n"
"    return a - VM.tdiv(a, b) * b\n"
"end\n", SS_DEPS1(H_TDIV) },

[H_FDIV] = { "fdiv", 0,
"VM.fdiv = function (a, b) { return b === 0 ? 0 : a / b; };\n",
"function VM.fdiv(a, b)\n"
"    if b == 0 then return 0.0 end\n"
"    return a / b\n"
"end\n", SS_NO_DEPS },

// The count is taken mod the width, as the VM does. JS shifts already yield an
// int32 and mask their count; Lua's need help.
[H_SHL32] = { "shl32", 0, 0,
"function VM.shl32(a, n)\n"
"    return VM.w32(a << (n & 31))\n"
"end\n", SS_DEPS1(H_W32) },

[H_SHL64] = { "shl64", 0, 0,
"function VM.shl64(a, n)\n"
"    return a << (n & 63)\n"
"end\n", SS_NO_DEPS },

// Lua's >> is LOGICAL. C's is arithmetic on a signed operand, which on a
// two's-complement integer is floor division by 2^n -- which is what // does.
[H_SAR32] = { "sar32", 0, 0,
"function VM.sar32(a, n)\n"
"    return a // (1 << (n & 31))\n"
"end\n", SS_NO_DEPS },

// 1 << 63 is negative in Lua, so a full-width shift is spelled out.
[H_SAR64] = { "sar64", 0, 0,
"function VM.sar64(a, n)\n"
"    n = n & 63\n"
"    if n == 63 then return a < 0 and -1 or 0 end\n"
"    return a // (1 << n)\n"
"end\n", SS_NO_DEPS },

[H_BC_I32_F32] = { "bc_i32_f32", 0,
"VM.bc_i32_f32 = function (v) { __bci32[0] = v; return __bcf32[0]; };\n",
0, SS_DEPS1(H_BCBUF) },

[H_BC_F32_I32] = { "bc_f32_i32", 0,
"VM.bc_f32_i32 = function (v) { __bcf32[0] = v; return __bci32[0]; };\n",
0, SS_DEPS1(H_BCBUF) },

[H_BC_I64_F64] = { "bc_i64_f64", 0, 0,
"function VM.bc_i64_f64(v) return (string.unpack(\"<d\", string.pack(\"<i8\", v))) end\n",
SS_NO_DEPS },

[H_BC_F64_I64] = { "bc_f64_i64", 0, 0,
"function VM.bc_f64_i64(v) return (string.unpack(\"<i8\", string.pack(\"<d\", v))) end\n",
SS_NO_DEPS },

// ---- sub-word packing (u1/u2/u4; u8/u16 index directly) --------------------

[H_PACK_GET] = { "pack_get", 0,
"VM.pack_get = function (w, i, bits) {\n"
"    const epw = (32 / bits) | 0;\n"
"    return (w[(i / epw) | 0] >>> ((i % epw) * bits)) & ((1 << bits) - 1);\n"
"};\n",
// The mask is not optional: a word read back from a 0-based table is a
// sign-extended Lua integer, and Lua's >> is logical, so the sign bits would
// shift down into the result.
"function VM.pack_get(w, i, bits)\n"
"    local epw = 32 // bits\n"
"    return ((w[i // epw] & 0xFFFFFFFF) >> ((i % epw) * bits)) & ((1 << bits) - 1)\n"
"end\n", SS_NO_DEPS },

[H_PACK_SET] = { "pack_set", 0,
"VM.pack_set = function (w, i, bits, v) {\n"
"    const epw = (32 / bits) | 0, sh = (i % epw) * bits, m = (1 << bits) - 1;\n"
"    const k = (i / epw) | 0;\n"
"    w[k] = (w[k] & ~(m << sh)) | ((v & m) << sh);\n"
"};\n",
"function VM.pack_set(w, i, bits, v)\n"
"    local epw = 32 // bits\n"
"    local sh, m, k = (i % epw) * bits, (1 << bits) - 1, i // epw\n"
"    w[k] = VM.w32((w[k] & ~(m << sh)) | ((v & m) << sh))\n"
"end\n", SS_DEPS1(H_W32) },

// ---- slices ----------------------------------------------------------------

[H_SLICE_AT] = { "slice_at", 0,
"VM.slice_at = function (n, s) { return s < 0 ? 0 : (s > n ? n : s); };\n",
"function VM.slice_at(n, s)\n"
"    if s < 0 then return 0 elseif s > n then return n end\n"
"    return s\n"
"end\n", SS_NO_DEPS },

[H_SLICE_N] = { "slice_n", 0,
"VM.slice_n = function (n, s, l) {\n"
"    const a = VM.slice_at(n, s);\n"
"    if (l < 0) l = 0;\n"
"    return l > n - a ? n - a : l;\n"
"};\n",
"function VM.slice_n(n, s, l)\n"
"    local a = VM.slice_at(n, s)\n"
"    if l < 0 then l = 0 end\n"
"    if l > n - a then return n - a end\n"
"    return l\n"
"end\n", SS_DEPS1(H_SLICE_AT) },

// JS: a subarray IS C's {ptr,len} with the offset pre-applied, so there is
// nothing to build and a subslice of a subslice composes.
[H_SUB] = { "sub", 0,
"VM.sub = function (a, at, n) {\n"
"    const s = VM.slice_at(a.length, at), l = VM.slice_n(a.length, at, n);\n"
"    return a.subarray(s, s + l);\n"
"};\n", 0, SS_DEPS2(H_SLICE_AT, H_SLICE_N) },

[H_SUBU] = { "subu", 0,
"VM.subu = function (a, at, n) { return a.subarray(at, at + n); };\n",
0, SS_NO_DEPS },

// Lua has neither pointer arithmetic nor array views, so a slice is the
// three-field record {a, off, len}. `off` is the field C does not need.
[H_VIEW] = { "view", 0, 0,
"function VM.view(t, len) return { a = t, off = 0, len = len } end\n", SS_NO_DEPS },

[H_EMPTY] = { "empty", 0, 0,
"function VM.empty() return { a = {}, off = 0, len = 0 } end\n", SS_NO_DEPS },

[H_SUBA] = { "suba", 0, 0,
"function VM.suba(t, tlen, at, n)\n"
"    return { a = t, off = VM.slice_at(tlen, at), len = VM.slice_n(tlen, at, n) }\n"
"end\n", SS_DEPS2(H_SLICE_AT, H_SLICE_N) },

[H_SUBAU] = { "subau", 0, 0,
"function VM.subau(t, at, n) return { a = t, off = at, len = n } end\n", SS_NO_DEPS },

[H_SUBS] = { "subs", 0, 0,
"function VM.subs(s, at, n)\n"
"    return { a = s.a, off = s.off + VM.slice_at(s.len, at), len = VM.slice_n(s.len, at, n) }\n"
"end\n", SS_DEPS2(H_SLICE_AT, H_SLICE_N) },

[H_SUBSU] = { "subsu", 0, 0,
"function VM.subsu(s, at, n) return { a = s.a, off = s.off + at, len = n } end\n", SS_NO_DEPS },

// ---- aggregates ------------------------------------------------------------
// 0-BASED tables. They are not Lua sequences: #, ipairs, table.concat,
// table.unpack and table.sort are all wrong on them.

[H_ZEROS] = { "zeros", 0, 0,
"function VM.zeros(n)\n"
"    local t = {}\n"
"    for i = 0, n - 1 do t[i] = 0 end\n"
"    return t\n"
"end\n", SS_NO_DEPS },

[H_ZEROSF] = { "zerosf", 0, 0,
"function VM.zerosf(n)\n"
"    local t = {}\n"
"    for i = 0, n - 1 do t[i] = 0.0 end\n"
"    return t\n"
"end\n", SS_NO_DEPS },

// table.move is the C-implemented bulk copy; a hand-written loop here is the
// one place the Lua backend visibly loses to memcpy.
[H_ACOPY] = { "acopy", 0,
"VM.acopy = function (a) { return a.slice(); };\n",
"function VM.acopy(a, n)\n"
"    local t = {}\n"
"    if n > 0 then table.move(a, 0, n - 1, 0, t) end\n"
"    return t\n"
"end\n", SS_NO_DEPS },

// ---- string building: a port of vm/vm_fmt.h --------------------------------

[H_BYTES] = { "bytes", 0,
"VM.bytes = function (s) {\n"
"    const a = new Uint8Array(s.length);\n"
"    for (let i = 0; i < s.length; i++) a[i] = s.charCodeAt(i);\n"
"    return a;\n"
"};\n",
"function VM.bytes(s)\n"
"    local t = {}\n"
"    for i = 1, #s do t[i - 1] = string.byte(s, i) end\n"
"    return { a = t, off = 0, len = #s }\n"
"end\n", SS_NO_DEPS },

// The only helper nothing emitted calls: it is the boundary a HOST reads a
// built string out through. Marked wherever IR_FMT is, on the grounds that a
// module which formats bytes has someone waiting to read them.
[H_STR] = { "str", 0,
"VM.str = function (s) {\n"
"    let r = \"\";\n"
"    for (let i = 0; i < s.length; i++) r += String.fromCharCode(s[i]);\n"
"    return r;\n"
"};\n",
"function VM.str(s)\n"
"    local out = {}\n"
"    for i = 0, s.len - 1 do out[i + 1] = string.char(s.a[s.off + i] & 0xFF) end\n"
"    return table.concat(out)\n"
"end\n", SS_NO_DEPS },

// print(): emitted ONLY under VM_PRINT_EMIT_CALL (vm.h). The default is to
// drop the statement entirely, so an ordinary export carries none of this.
// A host that wants the output somewhere other than stdout replaces VM.print
// after loading the module -- the emitted code only ever calls this name.
[H_PRINT] = { "print", 0,
"VM.print = function (s) {\n"
"    console.log(VM.str(s));\n"
"};\n",
"function VM.print(s)\n"
"    io.write(VM.str(s), \"\\n\")\n"
"end\n", SS_DEPS1(H_STR) },

[H_FMT_BYTES] = { "fmt_bytes", 0,
"VM.fmt_bytes = function (dst, at, src) {\n"
"    if (at < 0) at = 0;\n"
"    const cap = dst.length;\n"
"    for (let i = 0; i < src.length && at < cap; i++) dst[at++] = src[i];\n"
"    return at;\n"
"};\n",
"function VM.fmt_bytes(dst, at, src)\n"
"    if at < 0 then at = 0 end\n"
"    local cap, da, dof = dst.len, dst.a, dst.off\n"
"    local sa, sof, sn = src.a, src.off, src.len\n"
"    local i = 0\n"
"    while i < sn and at < cap do\n"
"        da[dof + at] = sa[sof + i]\n"
"        at = at + 1\n"
"        i = i + 1\n"
"    end\n"
"    return at\n"
"end\n", SS_NO_DEPS },

[H_FMT_UDEC] = { "fmt_udec", 0,
"VM.fmt_udec = function (dst, at, v, min_digits) {\n"
"    const tmp = []; let n = 0;\n"
"    if (min_digits > 19) min_digits = 19;\n"
"    while (v >= 1) { tmp[n++] = 48 + (v % 10); v = Math.floor(v / 10); }\n"
"    while (n < min_digits) tmp[n++] = 48;\n"
"    if (n === 0) tmp[n++] = 48;\n"
"    const cap = dst.length;\n"
"    while (n > 0 && at < cap) dst[at++] = tmp[--n];\n"
"    return at;\n"
"};\n",
"function VM.fmt_udec(dst, at, v, min_digits)\n"
"    local tmp, n = {}, 0\n"
"    v = math.tointeger(v) or VM.trunc(v)\n"
"    if min_digits > 19 then min_digits = 19 end\n"
"    while v > 0 do tmp[n] = 48 + (v % 10); n = n + 1; v = v // 10 end\n"
"    while n < min_digits do tmp[n] = 48; n = n + 1 end\n"
"    if n == 0 then tmp[0] = 48; n = 1 end\n"
"    local cap, da, dof = dst.len, dst.a, dst.off\n"
"    while n > 0 and at < cap do\n"
"        n = n - 1\n"
"        da[dof + at] = tmp[n]\n"
"        at = at + 1\n"
"    end\n"
"    return at\n"
"end\n", SS_DEPS1(H_TRUNC) },

[H_FMT_I32] = { "fmt_i32", 0,
"VM.fmt_i32 = function (dst, at, v) {\n"
"    if (at < 0) at = 0;\n"
"    if (v < 0) { if (at < dst.length) dst[at++] = 45; v = -v; }\n"
"    return VM.fmt_udec(dst, at, v, 0);\n"
"};\n",
"function VM.fmt_i32(dst, at, v)\n"
"    if at < 0 then at = 0 end\n"
"    if v < 0 then\n"
"        if at < dst.len then dst.a[dst.off + at] = 45; at = at + 1 end\n"
"        v = -v\n"
"    end\n"
"    return VM.fmt_udec(dst, at, v, 0)\n"
"end\n", SS_DEPS1(H_FMT_UDEC) },

// Lua integers are 64-bit and JS refuses i64 outright, so neither needs a
// second body -- but the name the walker prints has to exist.
[H_FMT_I64] = { "fmt_i64", 0,
"VM.fmt_i64 = VM.fmt_i32;\n",
"VM.fmt_i64 = VM.fmt_i32\n", SS_DEPS1(H_FMT_I32) },

[H_FMT_FX] = { "fmt_fx", 0,
"VM.fmt_fx = function (dst, at, raw, shift, decimals) {\n"
"    if (at < 0) at = 0;\n"
"    if (shift <= 0) return VM.fmt_i32(dst, at, raw);\n"
"    if (shift > 31) shift = 31;\n"
"    if (decimals < 0) decimals = 0;\n"
"    if (decimals > 9) decimals = 9;\n"
"    let r = raw;\n"
"    if (r < 0) { if (at < dst.length) dst[at++] = 45; r = -r; }\n"
"    const one = Math.pow(2, shift);\n"
"    let ip = Math.floor(r / one), fp = r - ip * one, scale = 1;\n"
"    for (let i = 0; i < decimals; i++) scale *= 10;\n"
"    let fr = Math.floor((fp * scale + one / 2) / one);\n"
"    if (fr >= scale) { fr -= scale; ip += 1; }\n"
"    at = VM.fmt_udec(dst, at, ip, 0);\n"
"    if (decimals > 0) {\n"
"        if (at < dst.length) dst[at++] = 46;\n"
"        at = VM.fmt_udec(dst, at, fr, decimals);\n"
"    }\n"
"    return at;\n"
"};\n",
"function VM.fmt_fx(dst, at, raw, shift, decimals)\n"
"    if at < 0 then at = 0 end\n"
"    if shift <= 0 then return VM.fmt_i32(dst, at, raw) end\n"
"    if shift > 31 then shift = 31 end\n"
"    if decimals < 0 then decimals = 0 end\n"
"    if decimals > 9 then decimals = 9 end\n"
"    local r = raw\n"
"    if r < 0 then\n"
"        if at < dst.len then dst.a[dst.off + at] = 45; at = at + 1 end\n"
"        r = -r\n"
"    end\n"
"    local one = 1 << shift\n"
"    local ip, fp, scale = r >> shift, r & (one - 1), 1\n"
"    for _ = 1, decimals do scale = scale * 10 end\n"
"    local fr = (fp * scale + (one >> 1)) >> shift\n"
"    if fr >= scale then fr = fr - scale; ip = ip + 1 end\n"
"    at = VM.fmt_udec(dst, at, ip, 0)\n"
"    if decimals > 0 then\n"
"        if at < dst.len then dst.a[dst.off + at] = 46; at = at + 1 end\n"
"        at = VM.fmt_udec(dst, at, fr, decimals)\n"
"    end\n"
"    return at\n"
"end\n", SS_DEPS2(H_FMT_UDEC, H_FMT_I32) },

[H_FMT_F64] = { "fmt_f64", 0,
"VM.fmt_f64 = function (dst, at, v, decimals) {\n"
"    if (at < 0) at = 0;\n"
"    if (decimals < 0) decimals = 0;\n"
"    if (decimals > 9) decimals = 9;\n"
"    if (!(v === v)) return VM.fmt_bytes(dst, at, VM.bytes(\"nan\"));\n"
"    if (v < 0) { if (at < dst.length) dst[at++] = 45; v = -v; }\n"
"    if (!(v < 1.8e19)) return VM.fmt_bytes(dst, at, VM.bytes(\"big\"));\n"
"    let scale = 1;\n"
"    for (let i = 0; i < decimals; i++) scale *= 10;\n"
"    let ip = Math.floor(v), fr = Math.floor((v - ip) * scale + 0.5);\n"
"    if (fr >= scale) { fr -= scale; ip += 1; }\n"
"    at = VM.fmt_udec(dst, at, ip, 0);\n"
"    if (decimals > 0) {\n"
"        if (at < dst.length) dst[at++] = 46;\n"
"        at = VM.fmt_udec(dst, at, fr, decimals);\n"
"    }\n"
"    return at;\n"
"};\n",
// The out-of-range guard is 9.0e18, not C's 1.8e19: C casts to unsigned long
// long, and Lua's integers are SIGNED 64-bit, so anything above 2^63 has no
// integer to be converted to.
"function VM.fmt_f64(dst, at, v, decimals)\n"
"    if at < 0 then at = 0 end\n"
"    if decimals < 0 then decimals = 0 end\n"
"    if decimals > 9 then decimals = 9 end\n"
"    if v ~= v then return VM.fmt_bytes(dst, at, VM.bytes(\"nan\")) end\n"
"    if v < 0 then\n"
"        if at < dst.len then dst.a[dst.off + at] = 45; at = at + 1 end\n"
"        v = -v\n"
"    end\n"
"    if not (v < 9.0e18) then return VM.fmt_bytes(dst, at, VM.bytes(\"big\")) end\n"
"    local scale = 1\n"
"    for _ = 1, decimals do scale = scale * 10 end\n"
"    local ip = VM.trunc(v)\n"
"    local fr = VM.trunc((v - ip) * scale + 0.5)\n"
"    if fr >= scale then fr = fr - scale; ip = ip + 1 end\n"
"    at = VM.fmt_udec(dst, at, ip, 0)\n"
"    if decimals > 0 then\n"
"        if at < dst.len then dst.a[dst.off + at] = 46; at = at + 1 end\n"
"        at = VM.fmt_udec(dst, at, fr, decimals)\n"
"    end\n"
"    return at\n"
"end\n", SS_DEPS4(H_TRUNC, H_BYTES, H_FMT_BYTES, H_FMT_UDEC) },

// ---- math builtins (the names vm_emit_c.c would call) ----------------------
// The Lua `+ 0.0` keeps these FLOAT-valued, as their C counterparts are:
// math.floor returns an integer, and an integer leaking into f64 code changes
// what // and % do to it later.

[H_M_FLOOR] = { "m_floor", "m_floorf",
"VM.m_floor = function (x) { return Math.floor(x); };\nVM.m_floorf = VM.m_floor;\n",
"function VM.m_floor(x) return math.floor(x) + 0.0 end\nVM.m_floorf = VM.m_floor\n",
SS_NO_DEPS },

[H_M_CEIL] = { "m_ceil", "m_ceilf",
"VM.m_ceil = function (x) { return Math.ceil(x); };\nVM.m_ceilf = VM.m_ceil;\n",
"function VM.m_ceil(x) return math.ceil(x) + 0.0 end\nVM.m_ceilf = VM.m_ceil\n",
SS_NO_DEPS },

[H_M_ROUND] = { "m_round", "m_roundf",
"VM.m_round = function (x) { return VM.trunc(x >= 0 ? x + 0.5 : x - 0.5); };\n"
"VM.m_roundf = VM.m_round;\n",
"function VM.m_round(x)\n"
"    if x >= 0 then return VM.trunc(x + 0.5) + 0.0 end\n"
"    return VM.trunc(x - 0.5) + 0.0\n"
"end\nVM.m_roundf = VM.m_round\n", SS_DEPS1(H_TRUNC) },

[H_M_FRAC] = { "m_frac", "m_fracf",
"VM.m_frac = function (x) { return x - Math.floor(x); };\nVM.m_fracf = VM.m_frac;\n",
"function VM.m_frac(x) return x - math.floor(x) end\nVM.m_fracf = VM.m_frac\n",
SS_NO_DEPS },

[H_M_FABS] = { "m_fabs", "m_fabsf",
"VM.m_fabs = function (x) { return Math.abs(x); };\nVM.m_fabsf = VM.m_fabs;\n",
"function VM.m_fabs(x) return math.abs(x) + 0.0 end\nVM.m_fabsf = VM.m_fabs\n",
SS_NO_DEPS },

// math_pure's fmod FLOORS -- it is x - floor(x/y)*y, not C's fmod().
[H_M_FMOD] = { "m_fmod", "m_fmodf",
"VM.m_fmod = function (x, y) { return x - Math.floor(x / y) * y; };\nVM.m_fmodf = VM.m_fmod;\n",
"function VM.m_fmod(x, y) return x - math.floor(x / y) * y end\nVM.m_fmodf = VM.m_fmod\n",
SS_NO_DEPS },

[H_M_FMIN] = { "m_fmin", "m_fminf",
"VM.m_fmin = function (x, y) { return x < y ? x : y; };\nVM.m_fminf = VM.m_fmin;\n",
"function VM.m_fmin(x, y) if x < y then return x end return y end\nVM.m_fminf = VM.m_fmin\n",
SS_NO_DEPS },

[H_M_FMAX] = { "m_fmax", "m_fmaxf",
"VM.m_fmax = function (x, y) { return x > y ? x : y; };\nVM.m_fmaxf = VM.m_fmax;\n",
"function VM.m_fmax(x, y) if x > y then return x end return y end\nVM.m_fmaxf = VM.m_fmax\n",
SS_NO_DEPS },

[H_M_CLAMP] = { "m_clamp", "m_clampf",
"VM.m_clamp = function (x, lo, hi) { return x < lo ? lo : (x > hi ? hi : x); };\n"
"VM.m_clampf = VM.m_clamp;\n",
"function VM.m_clamp(x, lo, hi)\n"
"    if x < lo then return lo elseif x > hi then return hi end\n"
"    return x\n"
"end\nVM.m_clampf = VM.m_clamp\n", SS_NO_DEPS },

[H_M_MIX] = { "m_mix", "m_mixf",
"VM.m_mix = function (a, b, t) { return a + t * (b - a); };\nVM.m_mixf = VM.m_mix;\n",
"function VM.m_mix(a, b, t) return a + t * (b - a) end\nVM.m_mixf = VM.m_mix\n",
SS_NO_DEPS },

[H_M_IABS] = { "m_iabs", 0,
"VM.m_iabs = function (x) { return x < 0 ? -x : x; };\n",
"function VM.m_iabs(x) if x < 0 then return -x end return x end\n", SS_NO_DEPS },

[H_M_IMIN] = { "m_imin", 0,
"VM.m_imin = function (x, y) { return x < y ? x : y; };\n",
"function VM.m_imin(x, y) if x < y then return x end return y end\n", SS_NO_DEPS },

[H_M_IMAX] = { "m_imax", 0,
"VM.m_imax = function (x, y) { return x > y ? x : y; };\n",
"function VM.m_imax(x, y) if x > y then return x end return y end\n", SS_NO_DEPS },

[H_M_ICLAMP] = { "m_iclamp", 0,
"VM.m_iclamp = function (x, lo, hi) { return x < lo ? lo : (x > hi ? hi : x); };\n",
"function VM.m_iclamp(x, lo, hi)\n"
"    if x < lo then return lo elseif x > hi then return hi end\n"
"    return x\n"
"end\n", SS_NO_DEPS },

[H_M_IPOW] = { "m_ipow", 0,
"VM.m_ipow = function (base, e) {\n"
"    if (e < 0) return base === 1 ? 1 : (base === -1 ? (((-e) & 1) ? -1 : 1) : 0);\n"
"    let r = 1;\n"
"    while (e > 0) {\n"
"        if (e & 1) r = Math.imul(r, base);\n"
"        e >>= 1;\n"
"        if (e) base = Math.imul(base, base);\n"
"    }\n"
"    return r;\n"
"};\n",
"function VM.m_ipow(base, e)\n"
"    if e < 0 then\n"
"        if base == 1 then return 1 end\n"
"        if base == -1 then if ((-e) & 1) ~= 0 then return -1 else return 1 end end\n"
"        return 0\n"
"    end\n"
"    local r = 1\n"
"    while e > 0 do\n"
"        if (e & 1) ~= 0 then r = VM.w32(r * base) end\n"
"        e = e >> 1\n"
"        if e ~= 0 then base = VM.w32(base * base) end\n"
"    end\n"
"    return r\n"
"end\n", SS_DEPS1(H_W32) },

[H_M_LINEARSTEP] = { "m_linearstep", "m_linearstepf",
"VM.m_linearstep = function (e0, e1, x) {\n"
"    const d = e1 - e0;\n"
"    const t = d !== 0 ? (x - e0) / d : (x < e0 ? 0 : 1);\n"
"    return t < 0 ? 0 : (t > 1 ? 1 : t);\n"
"};\nVM.m_linearstepf = VM.m_linearstep;\n",
"function VM.m_linearstep(e0, e1, x)\n"
"    local d = e1 - e0\n"
"    local t\n"
"    if d ~= 0 then t = (x - e0) / d elseif x < e0 then t = 0.0 else t = 1.0 end\n"
"    if t < 0.0 then return 0.0 elseif t > 1.0 then return 1.0 end\n"
"    return t\n"
"end\nVM.m_linearstepf = VM.m_linearstep\n", SS_NO_DEPS },

[H_M_SMOOTHSTEP] = { "m_smoothstep", "m_smoothstepf",
"VM.m_smoothstep = function (e0, e1, x) {\n"
"    const t = VM.m_linearstep(e0, e1, x);\n"
"    return t * t * (3 - 2 * t);\n"
"};\nVM.m_smoothstepf = VM.m_smoothstep;\n",
"function VM.m_smoothstep(e0, e1, x)\n"
"    local t = VM.m_linearstep(e0, e1, x)\n"
"    return t * t * (3.0 - 2.0 * t)\n"
"end\nVM.m_smoothstepf = VM.m_smoothstep\n", SS_DEPS1(H_M_LINEARSTEP) },

[H_M_SMOOTHERSTEP] = { "m_smootherstep", "m_smootherstepf",
"VM.m_smootherstep = function (e0, e1, x) {\n"
"    const t = VM.m_linearstep(e0, e1, x);\n"
"    return t * t * t * (t * (t * 6 - 15) + 10);\n"
"};\nVM.m_smootherstepf = VM.m_smootherstep;\n",
"function VM.m_smootherstep(e0, e1, x)\n"
"    local t = VM.m_linearstep(e0, e1, x)\n"
"    return t * t * t * (t * (t * 6.0 - 15.0) + 10.0)\n"
"end\nVM.m_smootherstepf = VM.m_smootherstep\n", SS_DEPS1(H_M_LINEARSTEP) },

// The "a" variants continue with a linear tail past edge1 instead of clamping,
// so the result stays in the same domain as x.
[H_M_LINEARSTEPA] = { "m_linearstepa", "m_linearstepaf",
"VM.m_linearstepa = function (e0, e1, x) {\n"
"    if (x < e0) return 0;\n"
"    const d = e1 - e0;\n"
"    if (d === 0) return x <= e0 ? 0 : x - e0;\n"
"    if (x > e1) return x - 0.5 * (e1 + e0);\n"
"    const t = (x - e0) / d;\n"
"    return 0.5 * t * t * d;\n"
"};\nVM.m_linearstepaf = VM.m_linearstepa;\n",
"function VM.m_linearstepa(e0, e1, x)\n"
"    if x < e0 then return 0.0 end\n"
"    local d = e1 - e0\n"
"    if d == 0.0 then if x <= e0 then return 0.0 else return x - e0 end end\n"
"    if x > e1 then return x - 0.5 * (e1 + e0) end\n"
"    local t = (x - e0) / d\n"
"    return 0.5 * t * t * d\n"
"end\nVM.m_linearstepaf = VM.m_linearstepa\n", SS_NO_DEPS },

[H_M_SMOOTHSTEPA] = { "m_smoothstepa", "m_smoothstepaf",
"VM.m_smoothstepa = function (e0, e1, x) {\n"
"    if (x < e0) return 0;\n"
"    const d = e1 - e0;\n"
"    if (d === 0) return x <= e0 ? 0 : x - e0;\n"
"    if (x > e1) return x - 0.5 * (e1 + e0);\n"
"    const t = (x - e0) / d, t2 = t * t;\n"
"    return (t2 * t - 0.5 * t2 * t2) * d;\n"
"};\nVM.m_smoothstepaf = VM.m_smoothstepa;\n",
"function VM.m_smoothstepa(e0, e1, x)\n"
"    if x < e0 then return 0.0 end\n"
"    local d = e1 - e0\n"
"    if d == 0.0 then if x <= e0 then return 0.0 else return x - e0 end end\n"
"    if x > e1 then return x - 0.5 * (e1 + e0) end\n"
"    local t = (x - e0) / d\n"
"    local t2 = t * t\n"
"    return (t2 * t - 0.5 * t2 * t2) * d\n"
"end\nVM.m_smoothstepaf = VM.m_smoothstepa\n", SS_NO_DEPS },

[H_M_SMOOTHERSTEPA] = { "m_smootherstepa", "m_smootherstepaf",
"VM.m_smootherstepa = function (e0, e1, x) {\n"
"    if (x < e0) return 0;\n"
"    const d = e1 - e0;\n"
"    if (d === 0) return x <= e0 ? 0 : x - e0;\n"
"    if (x > e1) return x - 0.5 * (e1 + e0);\n"
"    const t = (x - e0) / d, t2 = t * t;\n"
"    return t2 * t2 * ((t - 3) * t + 2.5) * d;\n"
"};\nVM.m_smootherstepaf = VM.m_smootherstepa;\n",
"function VM.m_smootherstepa(e0, e1, x)\n"
"    if x < e0 then return 0.0 end\n"
"    local d = e1 - e0\n"
"    if d == 0.0 then if x <= e0 then return 0.0 else return x - e0 end end\n"
"    if x > e1 then return x - 0.5 * (e1 + e0) end\n"
"    local t = (x - e0) / d\n"
"    local t2 = t * t\n"
"    return t2 * t2 * ((t - 3.0) * t + 2.5) * d\n"
"end\nVM.m_smootherstepaf = VM.m_smootherstepa\n", SS_NO_DEPS },

// From here down the body is the HOST's, not common/math_pure.c's polynomial
// approximation -- a few ulp apart, not bit for bit. See the file header.
[H_M_SIN] = { "m_sin", "m_sinf",
"VM.m_sin = Math.sin;\nVM.m_sinf = VM.m_sin;\n",
"VM.m_sin = math.sin\nVM.m_sinf = VM.m_sin\n", SS_NO_DEPS },

[H_M_COS] = { "m_cos", "m_cosf",
"VM.m_cos = Math.cos;\nVM.m_cosf = VM.m_cos;\n",
"VM.m_cos = math.cos\nVM.m_cosf = VM.m_cos\n", SS_NO_DEPS },

[H_M_TAN] = { "m_tan", "m_tanf",
"VM.m_tan = Math.tan;\nVM.m_tanf = VM.m_tan;\n",
"VM.m_tan = math.tan\nVM.m_tanf = VM.m_tan\n", SS_NO_DEPS },

[H_M_ASIN] = { "m_asin", "m_asinf",
"VM.m_asin = Math.asin;\nVM.m_asinf = VM.m_asin;\n",
"VM.m_asin = math.asin\nVM.m_asinf = VM.m_asin\n", SS_NO_DEPS },

[H_M_ACOS] = { "m_acos", "m_acosf",
"VM.m_acos = Math.acos;\nVM.m_acosf = VM.m_acos;\n",
"VM.m_acos = math.acos\nVM.m_acosf = VM.m_acos\n", SS_NO_DEPS },

[H_M_ATAN] = { "m_atan", "m_atanf",
"VM.m_atan = Math.atan;\nVM.m_atanf = VM.m_atan;\n",
"VM.m_atan = math.atan\nVM.m_atanf = VM.m_atan\n", SS_NO_DEPS },

[H_M_ATAN2] = { "m_atan2", "m_atan2f",
"VM.m_atan2 = Math.atan2;\nVM.m_atan2f = VM.m_atan2;\n",
"function VM.m_atan2(y, x) return math.atan(y, x) end\nVM.m_atan2f = VM.m_atan2\n",
SS_NO_DEPS },

[H_M_EXP] = { "m_exp", "m_expf",
"VM.m_exp = Math.exp;\nVM.m_expf = VM.m_exp;\n",
"VM.m_exp = math.exp\nVM.m_expf = VM.m_exp\n", SS_NO_DEPS },

[H_M_LOG] = { "m_log", "m_logf",
"VM.m_log = Math.log;\nVM.m_logf = VM.m_log;\n",
"function VM.m_log(x) return math.log(x) end\nVM.m_logf = VM.m_log\n", SS_NO_DEPS },

[H_M_LOG2] = { "m_log2", "m_log2f",
"VM.m_log2 = Math.log2;\nVM.m_log2f = VM.m_log2;\n",
"function VM.m_log2(x) return math.log(x, 2.0) end\nVM.m_log2f = VM.m_log2\n", SS_NO_DEPS },

[H_M_LOG10] = { "m_log10", "m_log10f",
"VM.m_log10 = Math.log10;\nVM.m_log10f = VM.m_log10;\n",
"function VM.m_log10(x) return math.log(x, 10.0) end\nVM.m_log10f = VM.m_log10\n", SS_NO_DEPS },

[H_M_POW] = { "m_pow", "m_powf",
"VM.m_pow = Math.pow;\nVM.m_powf = VM.m_pow;\n",
"function VM.m_pow(x, y) return x ^ y end\nVM.m_powf = VM.m_pow\n", SS_NO_DEPS },

[H_M_SQRT] = { "m_sqrt", "m_sqrtf",
"VM.m_sqrt = Math.sqrt;\nVM.m_sqrtf = VM.m_sqrt;\n",
"VM.m_sqrt = math.sqrt\nVM.m_sqrtf = VM.m_sqrt\n", SS_NO_DEPS },

[H_M_CBRT] = { "m_cbrt", "m_cbrtf",
"VM.m_cbrt = Math.cbrt;\nVM.m_cbrtf = VM.m_cbrt;\n",
"function VM.m_cbrt(x) return x ^ (1.0 / 3.0) end\nVM.m_cbrtf = VM.m_cbrt\n", SS_NO_DEPS },

// Input in turns, not radians.
[H_M_SIN01] = { "m_sin01", "m_sin01f",
"VM.m_sin01 = function (x) { return VM.m_sin(x * 6.2831853071795865); };\n"
"VM.m_sin01f = VM.m_sin01;\n",
"function VM.m_sin01(x) return VM.m_sin(x * 6.2831853071795865) end\n"
"VM.m_sin01f = VM.m_sin01\n", SS_DEPS1(H_M_SIN) },

[H_M_COS01] = { "m_cos01", "m_cos01f",
"VM.m_cos01 = function (x) { return VM.m_cos(x * 6.2831853071795865); };\n"
"VM.m_cos01f = VM.m_cos01;\n",
"function VM.m_cos01(x) return VM.m_cos(x * 6.2831853071795865) end\n"
"VM.m_cos01f = VM.m_cos01\n", SS_DEPS1(H_M_COS) },

};

#endif
