// vm/vm_lib.h - built-ins written in the language itself.
//
// Each entry is one `name = (params) => body` in the DSL, attached to a
// compilation unit only when that name resolves to nothing there and compiled
// once per call-site type signature like any other template. So `sum` over
// [3]i32 is integer code, over [4]f64 is double code, and under
// `#rewire f64 -> fx16` is fixed-point code at that unit's own shift -- which is
// the whole reason these are source and not C bindings.
//
// Two groups, and they reach a script differently:
//
//   * the aggregate and geometry helpers have no C built-in, so they attach
//     whenever a unit uses them;
//   * the step functions shadow a registered native, so they are reached only
//     under `#enable source_builtins`. The native stays the default because it
//     is ~11x cheaper in the interpreter, which is what a live preview runs.
//
// A body may call another built-in by name; the resolver looks in the library
// first, so a unit's own `linearstep` cannot hijack `smoothstep`'s internals.

#ifndef VM_LIB_H
#define VM_LIB_H

// ---- aggregates ----
// The accumulator is `xs[0] * 0`, not `0` and not `xs[0]`. `0` would be an i32
// and the first `t + xs[i]` a lossy assignment; `xs[0]` would bind t to a ROW
// (a slice into the argument) and the accumulation would write into the
// caller's array. `xs[0] * 0` is a vector expression, so it materialises a
// fresh value of the right element type, width and shift -- one lane per column
// over rows, a scalar over a flat array -- and `* 0` is 0 at every fixed-point
// shift, which `+ 0` also is but which identity elimination would delete,
// handing the alias back.
//
// It also cannot fault on an empty input, which is why `sum([])` needs no
// guard: `x * 0` folds to a typed zero at compile time, so the index is never
// evaluated, and `for i in 0..0` runs no iterations. A guard returning a
// literal 0 would be worse than unnecessary -- over rows the two returns (an
// i32 and a [2]i32) have no common type.
#define VM_LIB_SRC_SUM \
    "sum = (xs) => {\n" \
    "  t = xs[0] * 0\n" \
    "  for i in 0..xs.len do t = t + xs[i]\n" \
    "  t\n" \
    "}\n"

// Over rows, `.len` is the ROW count and `t` is one lane per column, so this is
// the per-column mean without saying so anywhere. An empty input divides 0 by
// 0, which the div-by-zero guard answers with 0.
#define VM_LIB_SRC_MEAN \
    "mean = (xs) => sum(xs) / xs.len\n"

// ---- integer power ----
// The i64 half of ipow: natives have no i64 slot, so `x ** 7` and `ipow(x, 3)`
// on an i64 base come here rather than to the f64 pow they used to fall back to.
// Same results as m_ipow: a negative exponent is 0 except for a base of 1 or -1,
// and the base is not squared after the last bit, so it cannot overflow early.
// Reached only from the compiler (the name cannot clash with a user's).
#define VM_LIB_SRC_IPOW64 \
    "__ipow64 = (b: i64, e) =>\n" \
    "    if e < 0\n" \
    "        return (b == 1 || b == -1) ? ((e & 1) != 0 ? b : b * b) : b * 0\n" \
    "    r = b * 0 + 1\n" \
    "    x = b\n" \
    "    n = e\n" \
    "    while n > 0\n" \
    "        if (n & 1) != 0\n" \
    "            r = r * x\n" \
    "        n = n >> 1\n" \
    "        if n > 0\n" \
    "            x = x * x\n" \
    "    r\n"

// ---- polar ----
// Exact inverses of each other, and of the reactive engine's components of the
// same name (rvm/rvm_comp.c), whose forward direction these are.
#define VM_LIB_SRC_FROMPOLAR \
    "fromPolar = (p) => [p[0] * cos(p[1]), p[0] * sin(p[1])]\n"

#define VM_LIB_SRC_TOPOLAR \
    "toPolar = (p) => [sqrt(p[0]*p[0] + p[1]*p[1]), atan2(p[1], p[0])]\n"

// Turning a point, written so that it inverts as a rotation rather than into
// whichever operand comes first.
#define VM_LIB_SRC_ROTATE \
    "rotate = (p, a) => [p[0]*cos(a) - p[1]*sin(a), p[0]*sin(a) + p[1]*cos(a)]\n"

#define VM_LIB_SRC_ROTATEX \
    "rotateX = (p, a) => [p[0], p[1]*cos(a) - p[2]*sin(a), p[1]*sin(a) + p[2]*cos(a)]\n"

#define VM_LIB_SRC_ROTATEY \
    "rotateY = (p, a) => [p[2]*sin(a) + p[0]*cos(a), p[1], p[2]*cos(a) - p[0]*sin(a)]\n"

#define VM_LIB_SRC_ROTATEZ \
    "rotateZ = (p, a) => [p[0]*cos(a) - p[1]*sin(a), p[0]*sin(a) + p[1]*cos(a), p[2]]\n"

// Z, then Y, then X: R = Rx Ry Rz. style only picks what the reactive
// component writes back (0 the point, 1 the angles).
#define VM_LIB_SRC_EULER \
    "euler = (p, r, style = 0) => rotateX(rotateY(rotateZ(p, r[2]), r[1]), r[0])\n"

// The perspective divide. z <= 0 gives inf / a mirrored point here; the
// reactive component draws nothing there instead.
#define VM_LIB_SRC_PROJECT \
    "project = (p, f) => [p[0] * f / p[2], p[1] * f / p[2]]\n"

// ---- colour ----
// A palette entry: 0xAABBGGRR, red in the LOW byte, alpha opaque, each channel
// clamped to 0..255. Source rather than a host native so every backend gets it,
// including the ones with no way to call into C.
#define VM_LIB_SRC_RGB \
    "rgb = (r, g, b) => min(max(i32(r), 0), 255) | (min(max(i32(g), 0), 255) << 8)" \
    " | (min(max(i32(b), 0), 255) << 16) | (255 << 24)\n"

// ---- geometry, GLSL names and argument order ----
// Integer literals throughout, never 2.0 / 1.0: an int operand takes the other
// side's kind, so [3]f32 in gives [3]f32 out and fx8 stays fx8, where a float
// literal is f64 and would widen every f32 caller to double.
#define VM_LIB_SRC_DOT \
    "dot = (a, b) => sum(a * b)\n"

#define VM_LIB_SRC_LENGTH \
    "length = (v) => sqrt(dot(v, v))\n"

#define VM_LIB_SRC_DISTANCE \
    "distance = (a, b) => length(a - b)\n"

#define VM_LIB_SRC_NORMALIZE \
    "normalize = (v) => v / length(v)\n"

#define VM_LIB_SRC_REFLECT \
    "reflect = (i, n) => {\n" \
    "  d = dot(n, i)\n" \
    "  i - (d + d) * n\n" \
    "}\n"

// Total internal reflection (k < 0) is the zero vector. One expression rather
// than an early `return i * 0`: that return would be i's kind and the tail
// eta's, and two array returns of different kinds have no join.
#define VM_LIB_SRC_REFRACT \
    "refract = (i, n, eta) => {\n" \
    "  d = dot(n, i)\n" \
    "  k = 1 - eta * eta * (1 - d * d)\n" \
    "  (eta * i - (eta * d + sqrt(max(k, 0))) * n) * (k < 0 ? 0 : 1)\n" \
    "}\n"

#define VM_LIB_SRC_CROSS \
    "cross = (a, b) => [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]]\n"

#define VM_LIB_SRC_FACEFORWARD \
    "faceforward = (n, i, nref) => n * (dot(nref, i) < 0 ? 1 : -1)\n"

// ---- the step family: only under #enable source_builtins ----
// Identical results to the C built-ins at f64 and at fx16 (tests/test_fx16.c
// pins the table); the difference is that these are compiled AT the call site's
// own shift, so fx8 and fx22 stop round-tripping through one fx16 helper.
// min/max rather than clamp, deliberately: clamp registers an fx16 variant, so
// a clamp inside the body would pin the result to Q16.16 whatever shift the call
// site uses -- the exact round-trip this feature exists to avoid. min and max
// are compiler-inlined to compare+select at ANY shift and link nothing.
#define VM_LIB_SRC_LINEARSTEP \
    "linearstep = (a, b, x) => min(max((x - a) / (b - a), 0.0), 1.0)\n"

#define VM_LIB_SRC_SMOOTHSTEP \
    "smoothstep = (a, b, x) => {\n" \
    "  t = linearstep(a, b, x)\n" \
    "  t * t * (3.0 - 2.0 * t)\n" \
    "}\n"

#define VM_LIB_SRC_SMOOTHERSTEP \
    "smootherstep = (a, b, x) => {\n" \
    "  t = linearstep(a, b, x)\n" \
    "  t * t * t * (t * (t * 6.0 - 15.0) + 10.0)\n" \
    "}\n"

// The "a" variants do not clamp the upper end: they ease INTO the b edge and
// keep rising with slope 1 past it, so a value beyond b still moves. The curve
// is scaled by the edge distance d, which is what makes the join smooth --
// exactly m_linearstepa / m_smoothstepa / m_smootherstepa.
#define VM_LIB_SRC_LINEARSTEPA \
    "linearstepa = (a, b, x) => {\n" \
    "  if x < a then return 0.0\n" \
    "  d = b - a\n" \
    "  if d == 0.0 then return x <= a ? 0.0 : x - a\n" \
    "  if x > b then return x - 0.5 * (b + a)\n" \
    "  t = (x - a) / d\n" \
    "  0.5 * t * t * d\n" \
    "}\n"

#define VM_LIB_SRC_SMOOTHSTEPA \
    "smoothstepa = (a, b, x) => {\n" \
    "  if x < a then return 0.0\n" \
    "  d = b - a\n" \
    "  if d == 0.0 then return x <= a ? 0.0 : x - a\n" \
    "  if x > b then return x - 0.5 * (b + a)\n" \
    "  t = (x - a) / d\n" \
    "  t2 = t * t\n" \
    "  (t2 * t - 0.5 * t2 * t2) * d\n" \
    "}\n"

#define VM_LIB_SRC_SMOOTHERSTEPA \
    "smootherstepa = (a, b, x) => {\n" \
    "  if x < a then return 0.0\n" \
    "  d = b - a\n" \
    "  if d == 0.0 then return x <= a ? 0.0 : x - a\n" \
    "  if x > b then return x - 0.5 * (b + a)\n" \
    "  t = (x - a) / d\n" \
    "  t2 = t * t\n" \
    "  t2 * t2 * ((t - 3.0) * t + 2.5) * d\n" \
    "}\n"

#endif
