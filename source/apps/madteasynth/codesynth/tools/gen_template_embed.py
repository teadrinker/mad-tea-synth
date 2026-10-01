#!/usr/bin/env python3
"""Generate export_template_embedded.h -- the project-export template, as C++ data.

An exported project must build with no include path outside itself, so the
exporter has to *carry* the renderer sources rather than point at them. This
script walks the `#include "..."` graph from a small manifest of roots, and
writes every file it reaches into one generated header as string data.

Run by CMake at configure time (see CMakeLists.txt) and by
build_codesynth_export.bat, from the repo root:

    python apps/madteasynth/codesynth/tools/gen_template_embed.py \
        --root . --manifest <manifest.txt> --out <dir>/export_template_embedded.h

Three things here are not obvious.

**The closure is walked, not listed.** A hand-maintained file list goes stale
the first time someone adds an `#include` to render_polygon.h, and the symptom
is an exported project that does not compile -- surfacing in WSL, hours from the
edit that caused it. Walking means the manifest only names entry points.

**Target masks are derived, not declared.** A file's mask is the union of the
masks of everything that reaches it, so `font/` and `common/` become
all-targets automatically once a second target's shim pulls them in, and no
existing manifest line has to be touched to make that happen.

**Line endings are normalised to LF.** Files are read in Python's universal-
newline mode, so a CRLF source is embedded (and exported) as LF -- 18 of the 30
files in the current closure are CRLF in the repo. This is deliberate rather
than incidental: an exported project is built by waf under WSL and by clang,
both of which prefer LF, and a template that mixed the two would produce trees
whose line endings depended on which machine last touched the repo. It does
mean an embedded file is not byte-identical to its source; check_template_dump.py
compares with endings normalised and reports the count separately.

**Chunking is mandatory, not tidiness.** MSVC's C2026 caps a single string
literal at 16380 bytes and several of these files are far past that
(render_lowspec.c is 62 KB). Each file is emitted as a null-terminated array of
raw-string chunks split at line boundaries; the reader concatenates.

**Binary files take the other path entirely.** A `binary` entry (the menu icon
PNG) is emitted as a byte array with an explicit length instead of as string
chunks, because a PNG contains NUL bytes and embedded newlines -- a
null-terminated string literal would truncate it at the first zero and the LF
normalisation above would corrupt every 0x0D 0x0A pair in it. `binary` implies
`nofollow` (there is no include graph in a PNG) and cannot be combined with
`subst`.
"""

import argparse
import os
import re
import sys

# Keep in step with ExportTemplate.h's ExportTarget bits.
#
# `wav` is one bit for BOTH wav targets (mono and stereo): they carry identical
# files and differ only in the channel mode the exporter hands the song
# generator, which is a setting rather than a file. It is kExportTargetWavStereo
# in the header, reached through the kExportTemplateWav alias.
TARGETS = {'pebble': 1, 'win32': 2, 'microw8': 4, 'wav': 8}

# Split chunks well under MSVC's 16380-byte literal cap. The margin is for the
# escaping the raw-string form does not need today but might if the delimiter
# ever has to change.
CHUNK_LIMIT = 12000

RAW_DELIM = 'TPL'

INCLUDE_RE = re.compile(r'^\s*#\s*include\s*"([^"]+)"', re.MULTILINE)


def die(msg):
    sys.stderr.write('gen_template_embed: %s\n' % msg)
    sys.exit(1)


def read_manifest(path):
    """Parse `dest | source | targets` lines. Blank lines and # comments skipped."""
    entries = []
    with open(path, 'r', encoding='utf-8') as f:
        for lineno, line in enumerate(f, 1):
            line = line.split('#', 1)[0].strip()
            if not line:
                continue
            parts = [p.strip() for p in line.split('|')]
            if len(parts) != 3:
                die('%s:%d: expected "dest | source | targets"' % (path, lineno))
            dest, src, tnames = parts
            mask = 0
            follow = True
            subst = False
            binary = False
            for t in tnames.replace(',', ' ').split():
                if t == 'subst':
                    # `{{NAME}}` placeholders, filled in by ExportTemplateWrite
                    # from values the export computes (song name, slug, screen
                    # size, sample rate). Opt-in per entry because `{{` is legal
                    # C -- a nested aggregate initialiser opens with it -- so a
                    # blanket substitution pass over the closure would be a
                    # silent rewrite of source files. Only the READMEs ask.
                    subst = True
                    continue
                if t == 'binary':
                    # Carried byte for byte: read in binary mode, no include
                    # rewrite, no LF normalisation, emitted as a byte array.
                    # Implies nofollow -- there is nothing in a PNG to walk.
                    binary = True
                    follow = False
                    continue
                if t == 'nofollow':
                    # An entry-point file whose OWN includes must not be walked.
                    # The template mains live outside the tree they are written
                    # for, so their `#include "svf.h"` cannot resolve from here
                    # -- and need not: every header they name is already reached
                    # through another manifest entry.
                    follow = False
                    continue
                if t == 'all':
                    mask = sum(TARGETS.values())
                elif t in TARGETS:
                    mask |= TARGETS[t]
                else:
                    die('%s:%d: unknown target %r' % (path, lineno, t))
            if not mask:
                die('%s:%d: no targets' % (path, lineno))
            if binary and subst:
                die('%s:%d: `binary` and `subst` are mutually exclusive' % (path, lineno))
            entries.append((dest, src, mask, follow, subst, binary))
    return entries


# Directories whose contents must never be selected by include resolution.
#
# teatime/src/c/common holds one-line SHIMS (`#include "../../../common/x.h"`)
# that exist only because teatime's waf build globs src/c and needs a TU there.
# An include of "common/math_fixedp.h" from teatime/src/c therefore resolves to
# the shim, not to the real header -- correct for teatime, wrong for us: an
# exported project's src/c/common IS the real thing, copied from the repo root.
# Skipping these makes such an include fall through to common/math_fixedp.h,
# which is the file the export must carry.
DEFAULT_EXCLUDES = [
    'teatime/src/c/common/',
    'teatime/src/c/vm/',
    'teatime/src/c/meld/',
    'teatime/src/c/parser/',
]


def excluded(relpath, excludes):
    rel = relpath.replace('\\', '/')
    return any(rel.startswith(e) for e in excludes)


def resolve_include(inc, from_src, root, excludes):
    """Resolve one `#include "inc"` the way the compiler will: the including
    file's own directory first, then the repo root (which every consumer of
    these sources puts on the include path) -- skipping the shim dirs above."""
    for cand in (os.path.join(os.path.dirname(from_src), inc), inc):
        cand = os.path.normpath(cand).replace('\\', '/')
        if excluded(cand, excludes):
            continue
        if os.path.isfile(os.path.join(root, cand)):
            return cand
    return None


def rewrite_text(text, src, root, excludes):
    """Rewrite only the `../`-bearing includes.

    teatime's shim TUs reach out of the project with `../../../font/x.h`, which
    cannot resolve in an exported tree. Everything else is already either
    same-directory or repo-root-relative, and both of those work unchanged
    under the export layout (src/c is on the include path, and font/ and
    common/ keep their directory names) -- so they are left exactly as they
    are. Rewriting them too would work, but it would make every embedded file
    differ from its repo original for no reason, and the phase-2 check is a
    diff of the dumped tree against the repo.
    """
    def sub(m):
        inc = m.group(1)
        if '../' not in inc.replace('\\', '/'):
            return m.group(0)
        target = resolve_include(inc, src, root, excludes)
        if target is None:
            die('%s: cannot resolve #include "%s"' % (src, inc))
        return m.group(0).replace('"%s"' % inc, '"%s"' % target)
    return INCLUDE_RE.sub(sub, text)


def derive_roots(entries):
    """Learn {source-dir-prefix: dest-dir-prefix} from the manifest.

    A manifest line pairs a repo path with an export path, and the part they do
    not share is exactly the relocation being asked for -- e.g.
    `teatime/src/c/svf.h` -> `src/c/svf.h` teaches `teatime/src/c/` -> `src/c/`.
    Discovered files are then relocated by the same rule as the entry point that
    reached them, with no second place to state it.

    Anything not under a learned prefix (font/, common/) keeps its repo-relative
    layout under src/c, which is what lets its `#include "font/..."` lines stay
    exactly as written.
    """
    roots = {}
    for dest, src, _, follow, _subst, _binary in entries:
        # Learn ONLY from entries whose includes are walked. A `nofollow` entry
        # is a leaf -- nothing is ever discovered through it -- so its mapping
        # can never be needed, while teaching from it actively breaks things:
        # templates/pebble/wscript -> wscript says the prefix maps to the
        # project ROOT, and templates/pebble/pebble_tsys.c -> src/c/... says it
        # maps to src/c. One dict key, two answers, last one wins, and a
        # discovered header silently lands in the wrong place.
        if not follow:
            continue
        d, s = dest.replace('\\', '/').split('/'), src.replace('\\', '/').split('/')
        n = 0
        while n < len(d) and n < len(s) and d[len(d) - 1 - n] == s[len(s) - 1 - n]:
            n += 1
        sp = '/'.join(s[:len(s) - n])
        dp = '/'.join(d[:len(d) - n])
        if sp:
            roots['' if not sp else sp + '/'] = dp + '/' if dp else ''
    return roots


def relocate(child, roots):
    """Export path for a discovered file."""
    best = ''
    for sp in roots:
        if child.startswith(sp) and len(sp) > len(best):
            best = sp
    if best:
        return roots[best] + child[len(best):]
    return 'src/c/' + child


def walk(entries, root, excludes):
    """Breadth-first over the include graph. Returns {src: (dest, mask, data)}
    with masks unioned over every path that reaches a file.

    `data` is `str` for a text file and `bytes` for a `binary` one; callers
    (this script's emitter, and check_template_dump.py) tell them apart with
    isinstance rather than by a second return value, so there is one fact to
    keep in step instead of two."""
    files = {}
    queue = []
    roots = derive_roots(entries)

    nofollow = set()
    # Keyed on the source path, and never set on a DISCOVERED file: substitution
    # is a property of a document the manifest names on purpose, not something a
    # file can inherit by being #included from one.
    substitute = set()
    # Never set on a discovered file either, for the same reason: a binary
    # entry is a leaf the manifest names on purpose.
    binaries = set()
    for dest, src, mask, follow, subst, binary in entries:
        full = os.path.join(root, src)
        if not os.path.isfile(full):
            die('manifest names a missing file: %s' % src)
        src = src.replace('\\', '/')
        if not follow:
            nofollow.add(src)
        if subst:
            substitute.add(src)
        if binary:
            binaries.add(src)
        queue.append((dest, src, mask))

    while queue:
        dest, src, mask = queue.pop(0)
        if src in files:
            prev_dest, prev_mask, text = files[src]
            if prev_dest != dest:
                die('%s wants two destinations: %s and %s' % (src, prev_dest, dest))
            if prev_mask | mask == prev_mask:
                continue  # already covered by this mask; nothing to propagate
            files[src] = (prev_dest, prev_mask | mask, text)
            mask = prev_mask | mask
        elif src in binaries:
            # No universal-newline read, no include rewrite, no raw-string
            # check: none of them mean anything for a byte stream, and the
            # first two would silently corrupt it.
            with open(os.path.join(root, src), 'rb') as f:
                data = f.read()
            # A zero-length array is not valid C++, and the shape emit_bytes
            # would have to fall back to (one 0x00) would export a 1-byte file
            # while claiming to have carried the original. Neither is a thing
            # to guess at, and an empty resource is a mistake upstream anyway.
            if not data:
                die('%s is a `binary` entry but is empty' % src)
            files[src] = (dest, mask, data)
        else:
            with open(os.path.join(root, src), 'r', encoding='utf-8', errors='strict') as f:
                text = f.read()
            text = text if src in nofollow else rewrite_text(text, src, root, excludes)
            if (')' + RAW_DELIM + '"') in text:
                die('%s contains the raw-string terminator )%s" -- change RAW_DELIM'
                    % (src, RAW_DELIM))
            files[src] = (dest, mask, text)

        if src in substitute and '{{' not in files[src][2]:
            die('%s is marked `subst` but contains no {{placeholder}}' % src)

        if src in nofollow:
            continue

        # Follow the includes of the file as it will be EXPORTED (post-rewrite),
        # so a rewritten `../` path is resolved from its new spelling.
        for inc in INCLUDE_RE.findall(files[src][2]):
            child = resolve_include(inc, src, root, excludes)
            if child is None:
                continue  # a system-ish or generated header (e.g. "song.h"); not ours
            if child.startswith('..'):
                die('%s: include escapes the repo root: %s' % (src, inc))
            queue.append((relocate(child, roots), child, mask))

    return files, substitute


def chunk_text(text):
    """Split at line boundaries into pieces under CHUNK_LIMIT bytes."""
    chunks, cur, cur_len = [], [], 0
    for line in text.splitlines(keepends=True):
        enc = len(line.encode('utf-8'))
        if cur and cur_len + enc > CHUNK_LIMIT:
            chunks.append(''.join(cur))
            cur, cur_len = [], 0
        cur.append(line)
        cur_len += enc
    if cur:
        chunks.append(''.join(cur))
    return chunks or ['']


def ident(path, binary=False):
    # Keyed on the SOURCE path, not the destination: two targets legitimately
    # write different files to the same place (pebble's main.c and win32's are
    # both src/c/main.c), so destinations are not unique and sources are.
    return ('kTplB_' if binary else 'kTplF_') + re.sub(r'[^A-Za-z0-9]', '_', path)


def emit_bytes(data):
    """A byte array's initialiser lines, 16 bytes to a line. Never empty --
    walk() rejects a zero-length binary entry, which is the only input that
    could produce one."""
    return ['  ' + ' '.join('0x%02x,' % b for b in data[i:i + 16])
            for i in range(0, len(data), 16)]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', required=True, help='repo root')
    ap.add_argument('--manifest', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--deps', default=None,
                    help='also write a newline-separated list of every source '
                         'the closure reached, for CMAKE_CONFIGURE_DEPENDS')
    ap.add_argument('--exclude', action='append', default=None,
                    help='path prefix include resolution must skip (repeatable)')
    args = ap.parse_args()

    root = os.path.abspath(args.root)
    excludes = args.exclude if args.exclude is not None else DEFAULT_EXCLUDES
    files, substitute = walk(read_manifest(args.manifest), root, excludes)

    # Two sources MAY share a destination -- each target writes its own main.c
    # to src/c/main.c -- but only if no single export would want both. Masks
    # that overlap mean one file silently overwriting the other in the exported
    # tree, with whichever came last winning.
    by_dest = {}
    for src in sorted(files):
        dest, mask, _ = files[src]
        for other_src, other_mask in by_dest.get(dest, []):
            if mask & other_mask:
                die('%s: %s and %s both write it for target mask %d'
                    % (dest, other_src, src, mask & other_mask))
        by_dest.setdefault(dest, []).append((src, mask))

    out = []
    w = out.append
    w('// GENERATED by apps/madteasynth/codesynth/tools/gen_template_embed.py.')
    w('// DO NOT EDIT -- regenerated whenever any embedded source changes.')
    w('//')
    w('// The project-export template: every file an exported project needs that')
    w('// is not generated per song. See ExportTemplate.h for how it is read.')
    w('#pragma once')
    w('')

    total = 0
    for src in sorted(files):
        dest, mask, data = files[src]
        if isinstance(data, bytes):
            # An explicit length, not a terminator: the whole reason this file
            # is not string chunks is that its bytes include zeros.
            total += len(data)
            w('// %s  (%d bytes, binary)' % (src, len(data)))
            w('static const unsigned char %s[] = {' % ident(src, True))
            for line in emit_bytes(data):
                w(line)
            w('};')
            w('')
            continue
        total += len(data.encode('utf-8'))
        w('// %s  (%d bytes)' % (src, len(data.encode('utf-8'))))
        w('static const char* const %s[] = {' % ident(src))
        for chunk in chunk_text(data):
            w('R"%s(%s)%s",' % (RAW_DELIM, chunk, RAW_DELIM))
        w('0 };')
        w('')

    w('static const ExportTemplateFile kExportTemplateFiles[] = {')
    for src in sorted(files):
        dest, mask, data = files[src]
        if isinstance(data, bytes):
            w('  { "%s", 0, %d, false, %s, sizeof(%s) },'
              % (dest, mask, ident(src, True), ident(src, True)))
        else:
            w('  { "%s", %s, %d, %s, 0, 0 },'
              % (dest, ident(src), mask, 'true' if src in substitute else 'false'))
    w('};')
    w('')
    w('// %d files, %d bytes of source.' % (len(files), total))

    text = '\n'.join(out) + '\n'

    # The closure is DISCOVERED, so CMake cannot know what to re-configure on
    # without being told -- an edit to render_polygon.h has to regenerate this
    # header, and nothing in CMakeLists.txt names that file. Written before the
    # unchanged early-out below, so a no-op run still refreshes the list.
    if args.deps:
        deps = '\n'.join(os.path.join(root, s).replace('\\', '/') for s in sorted(files))
        os.makedirs(os.path.dirname(os.path.abspath(args.deps)), exist_ok=True)
        with open(args.deps, 'w', encoding='utf-8', newline='\n') as f:
            f.write(deps + '\n')

    # Only rewrite on change: this runs at configure time, and touching the
    # header every run would rebuild every TU that includes it.
    try:
        with open(args.out, 'r', encoding='utf-8') as f:
            if f.read() == text:
                print('gen_template_embed: %d files, %d bytes (unchanged)' % (len(files), total))
                return
    except (IOError, OSError):
        pass

    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    with open(args.out, 'w', encoding='utf-8', newline='\n') as f:
        f.write(text)
    print('gen_template_embed: %d files, %d bytes -> %s' % (len(files), total, args.out))


if __name__ == '__main__':
    main()
