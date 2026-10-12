#!/usr/bin/env python3
"""oracle.py - the corpus index and the helpers every analyst calls.

The corpus is a read-only copy of the Build 940 install (default D:\\SLRR-test,
override with the environment variable SLRR_CORPUS or the --corpus option).
Sources are <dir>/src/<Name>.java (the directory is spelled src, Src or SRC),
classes are <dir>/<Name>.class.  Directories named tree_rpk_scan_boot* are
ignored.

Build the index (writes notes/pairs.json and notes/corpus.md):

    python -I oracle.py build

Use it from a script:

    import oracle
    for e in oracle.iter_oracle_pairs():          # Build 940, consistent pairs
        blobs = oracle.load_class(e)              # list of tufa.Blob
        text = oracle.load_source(e)              # str
        ...
    e = oracle.entry('game/Marker')               # one entry by name
    d = oracle.compare_blobs(mine, theirs)        # first differing chunk/offset
    if not d.equal: print(d)

    oracle.iter_pairs(build, consistent)          # any selection of pairs
    oracle.iter_entries(category)                 # sources without class, ...
    oracle.iter_blobs(build)                      # every blob of the corpus
    oracle.compare_files(bytes_a, bytes_b)        # blob by blob

Entry fields (one per source file, plus one per class file without a source):

    category   'pair_940'     source + class, every blob at build 0x24F9D
               'pair_other'   source + class, some blob at another build
               'source_only'  source without a class
               'class_only'   class without a source
    dir        directory of the class file, relative to the corpus, '/' separated
    name       base name without extension (case as on disk; source wins)
    source     relative path of the .java file, or None
    cls        relative path of the .class file, or None
    package    package named by the source, or None
    declared   top-level types of the source in order:
               [{'kind': 'class'|'interface', 'name', 'extends', 'implements',
                 'modifiers', 'line'}]
    blobs      per blob of the class file in order:
               [{'name', 'super', 'build', 'offset', 'size', 'interface'}]
    builds     sorted distinct build numbers of the class file
    names_match  True when the fully qualified declared names equal the blob
               names in the same order (None unless it is a pair)
    consistent   True when the class passes paircheck.py against the source
               (token census and line check); False = the source was edited
               after the class was compiled; None unless it is a pair
    stale      the reasons when consistent is False (list of str)
"""
import json
import os
import re
import sys
from collections import namedtuple, Counter

_HERE = os.path.dirname(os.path.abspath(__file__))
if _HERE not in sys.path:
    sys.path.insert(0, _HERE)

import paircheck  # noqa: E402
import srclex     # noqa: E402
import tufa       # noqa: E402

DEFAULT_CORPUS = r'D:\SLRR-test'
NOTES_DIR = os.path.normpath(os.path.join(_HERE, '..', 'notes'))
PAIRS_JSON = os.path.join(NOTES_DIR, 'pairs.json')
CORPUS_MD = os.path.join(NOTES_DIR, 'corpus.md')
BUILD_940 = tufa.BUILD_940
SKIP_DIR_PREFIX = 'tree_rpk_scan_boot'

CATEGORIES = ('pair_940', 'pair_other', 'source_only', 'class_only')


def corpus_root():
    return os.environ.get('SLRR_CORPUS') or DEFAULT_CORPUS


# ---------------------------------------------------------------------------
# file enumeration
# ---------------------------------------------------------------------------

def _walk(root):
    for dp, dn, fn in os.walk(root):
        dn[:] = sorted(d for d in dn if not d.lower().startswith(SKIP_DIR_PREFIX))
        yield dp, dn, sorted(fn)


def _rel(root, path):
    return os.path.relpath(path, root).replace('\\', '/')


def find_class_files(root=None):
    """Sorted relative paths of every .class file of the corpus."""
    root = root or corpus_root()
    out = []
    for dp, dn, fn in _walk(root):
        for f in fn:
            if f.lower().endswith('.class'):
                out.append(_rel(root, os.path.join(dp, f)))
    out.sort(key=str.lower)
    return out


def find_source_files(root=None):
    """Sorted relative paths of every .java file of the corpus (all of them
    sit in a directory named src in some letter case; anything else raises)."""
    root = root or corpus_root()
    out = []
    for dp, dn, fn in _walk(root):
        for f in fn:
            if f.lower().endswith('.java'):
                if os.path.basename(dp).lower() != 'src':
                    raise RuntimeError('source outside a src directory: %s'
                                       % os.path.join(dp, f))
                out.append(_rel(root, os.path.join(dp, f)))
    out.sort(key=str.lower)
    return out


# ---------------------------------------------------------------------------
# sources
# ---------------------------------------------------------------------------

def source_encoding(data):
    """Classify the bytes of a source file.  Returns (encoding, details) where
    encoding is 'ascii' for every file of the corpus (3,454 of 3,454); the
    other answers exist so that a foreign file is reported, not mangled:
    'utf-8-sig' (BOM), 'utf-16' (BOM), 'utf-8' (high bytes that decode) and
    'latin-1' (high bytes that do not)."""
    det = {
        'bom': None,
        'high_bytes': sum(1 for x in data if x >= 0x80),
        'nul': data.count(b'\x00'),
        'crlf': data.count(b'\r\n'),
        'tabs': data.count(b'\t'),
    }
    det['bare_lf'] = data.count(b'\n') - det['crlf']
    det['bare_cr'] = data.count(b'\r') - det['crlf']
    if data.startswith(b'\xef\xbb\xbf'):
        det['bom'] = 'utf-8'
        return 'utf-8-sig', det
    if data.startswith(b'\xff\xfe') or data.startswith(b'\xfe\xff'):
        det['bom'] = 'utf-16'
        return 'utf-16', det
    if det['high_bytes'] == 0:
        return 'ascii', det
    try:
        data.decode('utf-8')
        return 'utf-8', det
    except UnicodeDecodeError:
        return 'latin-1', det


def load_source_bytes(entry_or_path, root=None):
    with open(_abs(entry_or_path, 'source', root), 'rb') as f:
        return f.read()


def load_source(entry_or_path, root=None, keep_eol=True):
    """Text of a source file.  Every source of the corpus is 7-bit ASCII with
    CRLF line ends, so the text is decoded as latin-1 (one character per byte,
    identical to ASCII here and loss-free for any other file): character
    offsets are byte offsets and the text compares directly with the latin-1
    strings of tufa.py.  keep_eol=False turns CRLF into LF."""
    data = load_source_bytes(entry_or_path, root)
    text = data.decode('latin-1')
    if not keep_eol:
        text = text.replace('\r\n', '\n')
    return text


_TOKEN = re.compile(r'''
      //[^\n]*                      # line comment
    | /\*.*?\*/                     # block comment
    | "(?:\\.|[^"\\\n])*"           # string literal
    | '(?:\\.|[^'\\\n])*'           # char literal
''', re.S | re.X)


def strip_comments_and_strings(text):
    """Replace comments, string and char literals by spaces of the same length
    (newlines kept), so offsets and line numbers stay valid."""
    def blank(m):
        return re.sub(r'[^\n\r]', ' ', m.group(0))
    return _TOKEN.sub(blank, text)


_PACKAGE = re.compile(r'\bpackage\s+([A-Za-z_][\w.]*)\s*;')
_IMPORT = re.compile(r'\bimport\s+([A-Za-z_][\w.]*(?:\.\*)?)\s*;')
_DECL = re.compile(
    r'((?:\b(?:public|private|protected|abstract|final|static|native)\s+)*)'
    r'\b(class|interface)\s+([A-Za-z_]\w*)'
    r'(?:\s+extends\s+([A-Za-z_][\w.]*(?:\s*,\s*[A-Za-z_][\w.]*)*))?'
    r'(?:\s+implements\s+([A-Za-z_][\w.]*(?:\s*,\s*[A-Za-z_][\w.]*)*))?'
    r'\s*\{')


def scan_source(text):
    """Light scan: package, imports and the top-level type declarations in
    source order.  Returns {'package', 'imports', 'declared'}."""
    clean = strip_comments_and_strings(text)
    m = _PACKAGE.search(clean)
    package = m.group(1) if m else None
    imports = _IMPORT.findall(clean)
    # brace depth before each character, to keep only depth-0 declarations
    declared = []
    depth = 0
    pos = 0
    events = [(mm.start(), mm) for mm in _DECL.finditer(clean)]
    ei = 0
    n = len(clean)
    while pos < n and ei < len(events):
        start, mm = events[ei]
        # advance depth up to the start of this declaration
        seg = clean[pos:start]
        depth += seg.count('{') - seg.count('}')
        pos = start
        if depth == 0:
            split = lambda s: [x.strip() for x in s.split(',')] if s else []
            ext = split(mm.group(4))
            declared.append({
                'kind': mm.group(2),
                'name': mm.group(3),
                'extends': (ext[0] if len(ext) == 1 else ext) if ext else None,
                'implements': split(mm.group(5)),
                'modifiers': mm.group(1).split(),
                'line': clean.count('\n', 0, mm.start(2)) + 1,
            })
        ei += 1
    return {'package': package, 'imports': imports, 'declared': declared}


# ---------------------------------------------------------------------------
# classes
# ---------------------------------------------------------------------------

def load_class_bytes(entry_or_path, root=None):
    with open(_abs(entry_or_path, 'cls', root), 'rb') as f:
        return f.read()


def load_class(entry_or_path, root=None):
    """Parse a class file of the corpus (or any path) into [tufa.Blob]."""
    p = _abs(entry_or_path, 'cls', root)
    with open(p, 'rb') as f:
        return tufa.parse(f.read(), p)


def _abs(entry_or_path, attr, root=None):
    if isinstance(entry_or_path, Entry):
        rel = getattr(entry_or_path, attr)
        if rel is None:
            raise ValueError('%s has no %s file' % (entry_or_path.key, attr))
        return os.path.join(root or corpus_root(), rel.replace('/', os.sep))
    p = str(entry_or_path)
    if os.path.isabs(p):
        return p
    return os.path.join(root or corpus_root(), p.replace('/', os.sep))


# ---------------------------------------------------------------------------
# the index
# ---------------------------------------------------------------------------

def survey_sources(root=None):
    """Byte-level and token-level census of every source file: what encodings,
    line ends and literal forms occur.  Returns a dict of counters."""
    root = root or corpus_root()
    enc = Counter()
    lex = Counter()
    esc = Counter()
    for rel in find_source_files(root):
        data = load_source_bytes(rel, root)
        e, det = source_encoding(data)
        enc['files'] += 1
        enc['encoding ' + e] += 1
        if det['bom']:
            enc['with a BOM'] += 1
        if det['nul']:
            enc['with NUL bytes'] += 1
        if det['crlf'] and not det['bare_lf'] and not det['bare_cr']:
            enc['line ends CRLF only'] += 1
        elif det['bare_lf'] or det['bare_cr']:
            enc['with a bare LF or CR'] += 1
        if det['tabs']:
            enc['with TAB characters'] += 1
        if data and not data.endswith(b'\n'):
            enc['last line without a line end'] += 1
        if b'\x1a' in data:
            enc['with Ctrl-Z'] += 1
        if any(x < 32 and x not in (9, 10, 13) for x in data):
            enc['with other control characters'] += 1
        try:
            toks = srclex.tokens(data.decode('latin-1'))
        except srclex.LexError:
            lex['files that do not tokenize'] += 1
            continue
        lex['files tokenized'] += 1
        for t in toks:
            lex['tokens'] += 1
            lex['kind ' + t.kind] += 1
            x = t.text
            if t.kind == 'float':
                if x[-1] in 'fFdD':
                    lex['float with suffix ' + x[-1]] += 1
                if 'e' in x.lower().rstrip('fd'):
                    lex['float with exponent'] += 1
                if x.startswith('.'):
                    lex['float starting with "."'] += 1
                if x.rstrip('fFdD').endswith('.'):
                    lex['float ending with "."'] += 1
            elif t.kind == 'int':
                if x[:2] in ('0x', '0X'):
                    lex['int hexadecimal'] += 1
                elif len(x) > 1 and x[0] == '0':
                    lex['int with a leading zero'] += 1
                else:
                    lex['int decimal'] += 1
            elif t.kind == 'str':
                if '\t' in x:
                    lex['string holding a raw TAB'] += 1
                i = 0
                while i < len(x):
                    if x[i] == '\\' and i + 1 < len(x):
                        esc['\\' + x[i + 1]] += 1
                        i += 2
                    else:
                        i += 1
            elif t.kind == 'kw':
                lex['keyword ' + x] += 1
    return {'bytes': dict(enc), 'tokens': dict(lex), 'string_escapes': dict(esc)}


_ENTRY_FIELDS = ('category', 'dir', 'name', 'source', 'cls', 'package',
                 'declared', 'blobs', 'builds', 'names_match', 'source_size',
                 'class_size', 'consistent', 'stale')


class Entry(namedtuple('Entry', _ENTRY_FIELDS)):
    __slots__ = ()

    @property
    def key(self):
        return '%s/%s' % (self.dir, self.name)

    @property
    def is_pair(self):
        return self.source is not None and self.cls is not None

    @property
    def declared_names(self):
        """Fully qualified names of the declared top-level types, in order."""
        if self.declared is None:
            return None
        pre = (self.package + '.') if self.package else ''
        return [pre + d['name'] for d in self.declared]

    @property
    def blob_names(self):
        return None if self.blobs is None else [b['name'] for b in self.blobs]

    def to_dict(self):
        d = self._asdict()
        d['class'] = d.pop('cls')
        return dict(d)

    @classmethod
    def from_dict(cls, d):
        d = dict(d)
        d['cls'] = d.pop('class')
        return cls(**{k: d.get(k) for k in _ENTRY_FIELDS})


def build_index(root=None, progress=None):
    """Scan the corpus and return (entries, problems)."""
    root = root or corpus_root()
    sources = find_source_files(root)
    classes = find_class_files(root)
    problems = []
    by_key = {}
    for rel in classes:
        d, f = os.path.split(rel)
        k = (d.lower(), f[:-6].lower())
        if k in by_key:
            problems.append('two class files map to %s/%s' % k)
        by_key[k] = rel
    used = set()
    entries = []
    for n, rel in enumerate(sources):
        if progress and n % 500 == 0:
            progress('sources %d/%d' % (n, len(sources)))
        sdir, f = os.path.split(rel)
        d = os.path.dirname(sdir)            # strip the src component
        name = f[:-5]
        data = load_source_bytes(rel, root)
        enc, det = source_encoding(data)
        if enc != 'ascii':
            problems.append('%s: encoding %s %r' % (rel, enc, det))
        scan = scan_source(data.decode('latin-1'))
        k = (d.lower(), name.lower())
        crel = by_key.get(k)
        blobs = builds = match = csize = consistent = stale = None
        if crel is not None:
            if k in used:
                problems.append('two sources map to %s' % crel)
            used.add(k)
            blobs, builds, csize, parsed = _class_summary(root, crel, problems)
            pre = (scan['package'] + '.') if scan['package'] else ''
            match = [pre + x['name'] for x in scan['declared']] == \
                    [b['name'] for b in blobs]
            cat = 'pair_940' if builds == [BUILD_940] else 'pair_other'
            # is the class still the compilation of this source?
            stale = []
            if not match:
                stale.append('declared types differ from the blob names')
            try:
                r = paircheck.check_pair(data.decode('latin-1'), parsed)
                for label, a, b in r['census']:
                    stale.append('token census: %s source %d class %d' % (label, a, b))
                for kind, cnt in sorted(r['lines'].items()):
                    stale.append('line check: %d %s node%s not on the source line'
                                 % (cnt, kind, '' if cnt == 1 else 's'))
                stale.extend('e.g. ' + x for x in r['examples'][:2])
                consistent = match and r['consistent']
            except srclex.LexError as x:
                stale.append('source does not tokenize: %s' % x)
                consistent = False
        else:
            cat = 'source_only'
        entries.append(Entry(cat, d, name, rel, crel, scan['package'],
                             scan['declared'], blobs, builds, match,
                             len(data), csize, consistent, stale))
    for rel in classes:
        d, f = os.path.split(rel)
        k = (d.lower(), f[:-6].lower())
        if k in used:
            continue
        blobs, builds, csize, _ = _class_summary(root, rel, problems)
        entries.append(Entry('class_only', d, f[:-6], None, rel, None, None,
                             blobs, builds, None, None, csize, None, None))
    entries.sort(key=lambda e: (e.dir.lower(), e.name.lower()))
    return entries, problems


def _class_summary(root, rel, problems):
    data = load_class_bytes(rel, root)
    try:
        bl = tufa.parse(data, rel)
    except tufa.TufaError as x:
        problems.append('%s: %s' % (rel, x))
        return [], [], len(data), []
    blobs = [{'name': b.name, 'super': b.super_name, 'build': b.build,
              'offset': b.offset, 'size': b.size,
              'interface': bool(b.is_interface)} for b in bl]
    return blobs, sorted(set(b.build for b in bl)), len(data), bl


_INDEX = None


def load_index(path=None, rebuild=False):
    """The list of Entry, read from notes/pairs.json (built when missing)."""
    global _INDEX
    if _INDEX is not None and not rebuild and path is None:
        return _INDEX
    path = path or PAIRS_JSON
    if rebuild or not os.path.exists(path):
        entries, problems = build_index()
        write_index(entries, problems, path)
    else:
        with open(path, 'r', encoding='utf-8') as f:
            doc = json.load(f)
        entries = [Entry.from_dict(d) for d in doc['entries']]
    if path == PAIRS_JSON:
        _INDEX = entries
    return entries


def iter_entries(category=None):
    """Entries of one category (a name or a tuple of names), or all."""
    cats = None if category is None else (
        (category,) if isinstance(category, str) else tuple(category))
    for e in load_index():
        if cats is None or e.category in cats:
            yield e


def iter_pairs(build=BUILD_940, consistent=None):
    """Source/class pairs.  build=BUILD_940 (default) yields the pairs whose
    every blob was written by the Build 940 compiler, build=None yields all
    2,819 pairs, any other number the pairs holding that build.

    consistent=None (default) yields every such pair; consistent=True only
    the pairs whose class passes paircheck against the shipped source (use
    this for byte-for-byte comparisons: a stale class cannot match);
    consistent=False only the stale ones (Entry.stale lists the reasons)."""
    for e in load_index():
        if not e.is_pair:
            continue
        if build is not None and e.builds != [build]:
            continue
        if consistent is not None and bool(e.consistent) != consistent:
            continue
        yield e


def iter_oracle_pairs():
    """The pairs a compiler must reproduce byte for byte: Build 940 classes
    that are consistent with their shipped source."""
    return iter_pairs(BUILD_940, consistent=True)


def entry(name):
    """The one Entry whose 'dir/name' ends with the given text (case is
    ignored): entry('game/Marker'), entry('Trigger').  Raises KeyError when
    none or more than one matches; an exact 'dir/name' always wins."""
    q = name.replace('\\', '/').lower()
    for suffix in ('.java', '.class'):
        if q.endswith(suffix):
            q = q[:-len(suffix)]
    q = q.replace('/src/', '/')
    hits = [e for e in load_index() if e.key.lower() == q]
    if not hits:
        hits = [e for e in load_index() if ('/' + e.key.lower()).endswith('/' + q)]
    if len(hits) != 1:
        raise KeyError('%r matches %d entries%s' % (name, len(hits), (
            ': ' + ', '.join(h.key for h in hits[:6])) if hits else ''))
    return hits[0]


def iter_class_files():
    """Every class file of the corpus: yields (relative path, [tufa.Blob])."""
    for rel in find_class_files():
        yield rel, load_class(rel)


def iter_blobs(build=None):
    """Every blob of the corpus: yields (relative path, blob index, Blob)."""
    for rel, blobs in iter_class_files():
        for i, b in enumerate(blobs):
            if build is None or b.build == build:
                yield rel, i, b


# ---------------------------------------------------------------------------
# comparing compiler output with the oracle
# ---------------------------------------------------------------------------

class BlobDiff(namedtuple('BlobDiff', 'equal chunk offset where a_len b_len')):
    """Result of compare_blobs.
    equal   True when the two blobs serialise to the same bytes
    chunk   tag of the first chunk that differs ('TUFA' for the build number,
            'LAYOUT' when the chunk lists differ), None when equal
    offset  byte offset of the first difference inside that chunk's payload
    where   a readable position: pool entry, row and column, tree and node
    a_len, b_len  payload sizes of that chunk
    """
    __slots__ = ()

    def __bool__(self):
        return self.equal

    def __str__(self):
        if self.equal:
            return 'equal'
        return 'first difference in %s at +0x%X (%s); sizes %s / %s' % (
            self.chunk, self.offset or 0, self.where, self.a_len, self.b_len)


def _first_diff(a, b):
    n = min(len(a), len(b))
    for i in range(n):
        if a[i] != b[i]:
            return i
    return n if len(a) != len(b) else None


def _as_blob(x):
    if isinstance(x, tufa.Blob):
        return x
    if isinstance(x, (bytes, bytearray)):
        bl = tufa.parse(bytes(x))
        if len(bl) != 1:
            raise ValueError('expected one blob, found %d' % len(bl))
        return bl[0]
    raise TypeError('compare_blobs takes tufa.Blob or bytes, not %r' % type(x))


def locate(blob, tag, offset):
    """Describe what sits at a byte offset of a chunk payload of a blob."""
    if tag == 'CONS':
        if offset < 4:
            return 'entry count'
        pos = 4
        for i, e in enumerate(blob.cons):
            size = 4 + (4 + len(e[1].encode('latin-1')) + 1 if e[0] == 'utf8'
                        else 4 if e[0] == 'class' else 8)
            if offset < pos + size:
                return 'pool entry %d (%s %s) +%d' % (
                    i, e[0], blob.describe(i), offset - pos)
            pos += size
        return 'past the last pool entry'
    if tag in ('FILD', 'MTHD'):
        if tag == 'FILD':
            vecs = (('static', blob.static_fields), ('instance', blob.instance_fields))
            rs, cols = 16, ('flags', 'name', 'type', 'init')
        else:
            vecs = (('static', blob.static_methods), ('instance', blob.instance_methods))
            rs, cols = 20, ('flags', 'name', 'desc', 'tree', 'locals')
        pos = 0
        for label, rows in vecs:
            if offset < pos + 4:
                return '%s count' % label
            pos += 4
            if offset < pos + rs * len(rows):
                r, c = divmod(offset - pos, rs)
                return '%s row %d (%s) column %s' % (
                    label, r, blob.describe(rows[r].name), cols[c // 4])
            pos += rs * len(rows)
        return 'past the last row'
    if tag == 'CLSS':
        cols = ('marker', 'interface flag', 'name', 'super', 'interface count')
        k = offset // 4
        return cols[k] if k < 5 else 'interface %d' % (k - 5)
    if tag == 'TREE':
        if offset < 4:
            return 'tree count'
        pos = 4
        own = blob.tree_owners()
        for t, nodes in enumerate(blob.trees):
            label = 'tree %d (%s)' % (t, '; '.join(own.get(t, ['unowned'])))
            if offset < pos + 4:
                return '%s node count' % label
            pos += 4
            for k, nd in enumerate(nodes):
                size = tufa.NODE_SIZE[nd.op]
                if offset < pos + size:
                    part = 'op' if offset == pos else (
                        'slot' if offset < pos + 3 else 'imm')
                    return '%s node %d %s (op 0x%02X)' % (label, k, part, nd.op)
                pos += size
        return 'past the last tree'
    return ''


def compare_blobs(a, b):
    """Compare two blobs chunk by chunk (TUFA, then the chunks in file order)
    and report the first differing chunk and the offset inside its payload.
    a and b are tufa.Blob objects or the bytes of single blobs.  The readable
    position is taken from a (the candidate) and, when a is shorter, from b."""
    a = _as_blob(a)
    b = _as_blob(b)
    if a.build != b.build:
        return BlobDiff(False, 'TUFA', 0,
                        'build 0x%X / 0x%X' % (a.build, b.build), 4, 4)
    if list(a.chunks) != list(b.chunks):
        return BlobDiff(False, 'LAYOUT', 0,
                        'chunks %s / %s' % (','.join(a.chunks), ','.join(b.chunks)),
                        len(a.chunks), len(b.chunks))
    for tag in a.chunks:
        pa = tufa.chunk_payload(a, tag)
        pb = tufa.chunk_payload(b, tag)
        if pa == pb:
            continue
        off = _first_diff(pa, pb)
        where = locate(a, tag, off) if off < len(pa) else \
            'a ends; b has ' + locate(b, tag, off)
        return BlobDiff(False, tag, off, where, len(pa), len(pb))
    return BlobDiff(True, None, None, None, None, None)


def compare_files(a, b):
    """Compare two class files (bytes or lists of Blob) blob by blob.  Returns
    a list of (blob index, BlobDiff); a count mismatch is reported as a final
    (None, BlobDiff) with chunk 'BLOBS'."""
    la = tufa.parse(a) if isinstance(a, (bytes, bytearray)) else list(a)
    lb = tufa.parse(b) if isinstance(b, (bytes, bytearray)) else list(b)
    out = [(i, compare_blobs(x, y)) for i, (x, y) in enumerate(zip(la, lb))]
    if len(la) != len(lb):
        out.append((None, BlobDiff(False, 'BLOBS', 0, 'blob count %d / %d'
                                   % (len(la), len(lb)), len(la), len(lb))))
    return out


def files_equal(a, b):
    return all(d.equal for _, d in compare_files(a, b))


# ---------------------------------------------------------------------------
# writing pairs.json and corpus.md
# ---------------------------------------------------------------------------

def write_index(entries, problems, path=None, survey=None):
    path = path or PAIRS_JSON
    os.makedirs(os.path.dirname(path), exist_ok=True)
    counts = Counter(e.category for e in entries)
    doc = {
        'corpus': corpus_root(),
        'source_survey': survey,
        'counts': {c: counts.get(c, 0) for c in CATEGORIES},
        'sources': sum(1 for e in entries if e.source),
        'class_files': sum(1 for e in entries if e.cls),
        'blobs': sum(len(e.blobs) for e in entries if e.blobs),
        'problems': problems,
        'entries': [e.to_dict() for e in entries],
    }
    with open(path, 'w', encoding='utf-8', newline='\n') as f:
        json.dump(doc, f, indent=1, sort_keys=False)
        f.write('\n')
    return doc


def _top_dir(e, depth=2):
    parts = e.dir.split('/')
    return '/'.join(parts[:depth]).lower()


def corpus_markdown(entries, problems, survey=None):
    L = []
    w = L.append
    counts = Counter(e.category for e in entries)
    n_src = sum(1 for e in entries if e.source)
    n_cls = sum(1 for e in entries if e.cls)
    n_blob = sum(len(e.blobs) for e in entries if e.blobs)
    w('# Corpus index')
    w('')
    w('Generated by `lib/oracle.py build` from `%s`. Do not edit by hand; '
      'the machine-readable twin is `pairs.json`.' % corpus_root())
    w('')
    w('## Totals')
    w('')
    w('| What | Count |')
    w('| --- | ---: |')
    w('| Source files (`<dir>/src/<Name>.java`) | %d |' % n_src)
    w('| Class files (`<dir>/<Name>.class`) | %d |' % n_cls)
    w('| Class blobs in those files | %d |' % n_blob)
    w('| Pairs, every blob at build 0x24F9D (`pair_940`) | %d |' % counts['pair_940'])
    w('| Pairs with another build (`pair_other`) | %d |' % counts['pair_other'])
    w('| Sources without a class (`source_only`) | %d |' % counts['source_only'])
    w('| Classes without a source (`class_only`) | %d |' % counts['class_only'])
    w('')
    # builds
    bf = Counter()
    bb = Counter()
    mixed = 0
    for e in entries:
        if not e.cls:
            continue
        if len(e.builds) > 1:
            mixed += 1
        for b in e.builds:
            bf[b] += 1
        for b in e.blobs:
            bb[b['build']] += 1
    w('## Build numbers')
    w('')
    w('| Build | Class files | Blobs |')
    w('| --- | ---: | ---: |')
    for b in sorted(bf, reverse=True):
        w('| 0x%X (%d) | %d | %d |' % (b, b, bf[b], bb[b]))
    w('')
    w('Class files holding blobs of more than one build: %d.' % mixed)
    w('')
    cb = Counter()
    for e in entries:
        if e.cls:
            cb[(e.category, tuple(e.builds))] += 1
    w('| Category | Builds in the file | Class files |')
    w('| --- | --- | ---: |')
    for (cat, bs), n in sorted(cb.items()):
        w('| %s | %s | %d |' % (cat, ', '.join('0x%X' % b for b in bs), n))
    w('')
    # multi-blob
    multi = [e for e in entries if e.blobs and len(e.blobs) > 1]
    hist = Counter(len(e.blobs) for e in entries if e.blobs is not None and e.cls)
    w('## Blobs per class file')
    w('')
    w('| Blobs | Class files |')
    w('| ---: | ---: |')
    for k in sorted(hist):
        w('| %d | %d |' % (k, hist[k]))
    w('')
    w('Files with more than one blob (%d):' % len(multi))
    w('')
    w('| Class file | Category | Blobs | Classes in file order |')
    w('| --- | --- | ---: | --- |')
    for e in multi:
        w('| %s | %s | %d | %s |' % (e.cls, e.category, len(e.blobs),
                                     ', '.join(b['name'].rsplit('.', 1)[-1]
                                               for b in e.blobs)))
    w('')
    # declared vs blobs
    pairs = [e for e in entries if e.is_pair]
    ok = sum(1 for e in pairs if e.names_match)
    w('## Declared types against blob names')
    w('')
    w('For a pair, the light scan lists the top-level `class` / `interface` '
      'declarations of the source in order and prefixes the package; the blob '
      'names come from the CLSS chunks in file order.')
    w('')
    w('| Category | Pairs | Same names, same order | Different |')
    w('| --- | ---: | ---: | ---: |')
    for cat in ('pair_940', 'pair_other'):
        ps = [e for e in pairs if e.category == cat]
        k = sum(1 for e in ps if e.names_match)
        w('| %s | %d | %d | %d |' % (cat, len(ps), k, len(ps) - k))
    w('')
    bad = [e for e in pairs if not e.names_match]
    if bad:
        w('Pairs whose names differ (%d):' % len(bad))
        w('')
        w('| Source | Category | Declared | Blobs |')
        w('| --- | --- | --- | --- |')
        for e in bad:
            w('| %s | %s | %s | %s |' % (e.source, e.category,
                                         ', '.join(e.declared_names) or '(none)',
                                         ', '.join(e.blob_names) or '(none)'))
        w('')
    nodecl = [e for e in entries if e.source and not e.declared]
    multi_src = [e for e in entries if e.source and e.declared and len(e.declared) > 1]
    w('Sources declaring more than one top-level type: %d. Sources in which '
      'the scan finds no top-level type: %d.' % (len(multi_src), len(nodecl)))
    w('')
    if nodecl:
        for e in nodecl:
            w('- %s (%s)' % (e.source, e.category))
        w('')
    kinds = Counter(d['kind'] for e in entries if e.declared for d in e.declared)
    w('Declared top-level types over all sources: %s.' % ', '.join(
        '%d %s' % (v, k) for k, v in sorted(kinds.items())))
    w('')
    # consistency
    w('## Is the class still the compilation of the shipped source?')
    w('')
    w('`lib/paircheck.py` runs two checks on every pair: the token census '
      '(per-file counts of %d token classes of the source against the node '
      'counts of the class) and the line check (the literal or member name a '
      'node carries must stand on the source line the node records). A pair '
      'that fails either one is stale: the source was edited after the class '
      'was compiled, so the class cannot be reproduced from it. '
      '`oracle.iter_oracle_pairs()` yields only the consistent Build 940 pairs.'
      % len(paircheck.CENSUS))
    w('')
    w('| Category | Pairs | Consistent | Stale |')
    w('| --- | ---: | ---: | ---: |')
    for cat in ('pair_940', 'pair_other'):
        ps = [e for e in pairs if e.category == cat]
        k = sum(1 for e in ps if e.consistent)
        w('| %s | %d | %d | %d |' % (cat, len(ps), k, len(ps) - k))
    w('')
    st = [e for e in pairs if not e.consistent]
    if st:
        w('Stale pairs (%d):' % len(st))
        w('')
        w('| Class file | Category | Evidence |')
        w('| --- | --- | --- |')
        for e in st:
            ev = '; '.join(x for x in (e.stale or []) if not x.startswith('e.g. '))
            eg = [x for x in (e.stale or []) if x.startswith('e.g. ')]
            if eg:
                ev += ' (%s)' % eg[0].replace('|', '\\|')
            w('| %s | %s | %s |' % (e.cls, e.category, ev))
        w('')
    # encodings
    w('## Source encoding and literal forms')
    w('')
    if survey:
        w('Byte-level census of the source files (`oracle.survey_sources`). '
          '`oracle.load_source` decodes as latin-1, which is the identity on '
          'these files and loss-free on any other.')
        w('')
        w('| Property | Files |')
        w('| --- | ---: |')
        for k, v in sorted(survey['bytes'].items(), key=lambda kv: (kv[0] != 'files', kv[0])):
            w('| %s | %d |' % (k, v))
        w('')
        w('Token census with `lib/srclex.py` (strict mode: a character no rule '
          'accepts raises). Forms that are absent do not appear in the table; '
          'in particular there is no character literal, no float with an '
          'exponent or without digits on one side of the point, no octal-looking '
          'int.')
        w('')
        w('| Token property | Count |')
        w('| --- | ---: |')
        for k, v in sorted(survey['tokens'].items()):
            if not k.startswith('keyword '):
                w('| %s | %d |' % (k, v))
        w('')
        w('Keywords: %s.' % ', '.join('`%s` %d' % (k[8:], v) for k, v in sorted(
            survey['tokens'].items()) if k.startswith('keyword ')))
        w('')
        w('Escape sequences inside string literals: %s.' % ', '.join(
            '`%s` %d' % (k, v) for k, v in sorted(survey['string_escapes'].items())))
        w('')
    else:
        w('Run `oracle.py build` to fill this section.')
        w('')
    # directories
    w('## By directory')
    w('')
    w('Directory = first two path components of the class directory.')
    w('')
    w('| Directory | Sources | Class files | pair_940 | pair_other | source_only | class_only |')
    w('| --- | ---: | ---: | ---: | ---: | ---: | ---: |')
    dirs = {}
    for e in entries:
        d = dirs.setdefault(_top_dir(e), Counter())
        d[e.category] += 1
        if e.source:
            d['src'] += 1
        if e.cls:
            d['cls'] += 1
    for k in sorted(dirs):
        d = dirs[k]
        w('| %s | %d | %d | %d | %d | %d | %d |' % (
            k, d['src'], d['cls'], d['pair_940'], d['pair_other'],
            d['source_only'], d['class_only']))
    tot = Counter()
    for d in dirs.values():
        tot.update(d)
    w('| **total** | %d | %d | %d | %d | %d | %d |' % (
        tot['src'], tot['cls'], tot['pair_940'], tot['pair_other'],
        tot['source_only'], tot['class_only']))
    w('')
    # packages
    w('## By package (sources)')
    w('')
    w('| Package | Sources | pair_940 | pair_other | source_only |')
    w('| --- | ---: | ---: | ---: | ---: |')
    pk = {}
    for e in entries:
        if e.source:
            pk.setdefault(e.package or '(none)', Counter())[e.category] += 1
    for k in sorted(pk):
        d = pk[k]
        w('| %s | %d | %d | %d | %d |' % (k, sum(d.values()), d['pair_940'],
                                          d['pair_other'], d['source_only']))
    w('')
    # class only
    co = [e for e in entries if e.category == 'class_only']
    w('## Classes without a source (%d)' % len(co))
    w('')
    w('| Class file | Builds | Classes |')
    w('| --- | --- | --- |')
    for e in co:
        w('| %s | %s | %s |' % (e.cls, ', '.join('0x%X' % b for b in e.builds),
                                ', '.join(b['name'] for b in e.blobs)))
    w('')
    # other-build pairs
    po = [e for e in entries if e.category == 'pair_other']
    w('## Pairs compiled by an earlier build (%d)' % len(po))
    w('')
    w('| Class file | Builds | Names match |')
    w('| --- | --- | --- |')
    for e in po:
        w('| %s | %s | %s |' % (e.cls, ', '.join('0x%X' % b for b in e.builds),
                                'yes' if e.names_match else 'no'))
    w('')
    if problems:
        w('## Problems reported by the scan (%d)' % len(problems))
        w('')
        for p in problems:
            w('- %s' % p)
        w('')
    else:
        w('## Problems reported by the scan')
        w('')
        w('None: every class file parses with the strict reader and every '
          'source is ASCII.')
        w('')
    return '\n'.join(L)


def main(argv):
    import argparse
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--corpus', help='corpus root (default %s)' % DEFAULT_CORPUS)
    sub = ap.add_subparsers(dest='cmd')
    sub.add_parser('build', help='scan the corpus, write pairs.json and corpus.md')
    sub.add_parser('summary', help='print the category counts of pairs.json')
    p = sub.add_parser('show', help='print the entry of a source or class')
    p.add_argument('name', help='substring of dir/name, case-insensitive')
    p = sub.add_parser('diff', help='compare two class files chunk by chunk')
    p.add_argument('a')
    p.add_argument('b')
    a = ap.parse_args(argv)
    if a.corpus:
        os.environ['SLRR_CORPUS'] = a.corpus
    if a.cmd == 'build':
        entries, problems = build_index(progress=lambda s: print(s, file=sys.stderr))
        survey = survey_sources()
        doc = write_index(entries, problems, survey=survey)
        with open(CORPUS_MD, 'w', encoding='utf-8', newline='\n') as f:
            f.write(corpus_markdown(entries, problems, survey))
            f.write('\n')
        print('sources %d  class files %d  blobs %d' % (
            doc['sources'], doc['class_files'], doc['blobs']))
        for c in CATEGORIES:
            print('  %-12s %d' % (c, doc['counts'][c]))
        print('problems %d' % len(problems))
        print('wrote %s' % PAIRS_JSON)
        print('wrote %s' % CORPUS_MD)
        return 0
    if a.cmd == 'summary':
        c = Counter(e.category for e in load_index())
        for k in CATEGORIES:
            print('%-12s %d' % (k, c.get(k, 0)))
        return 0
    if a.cmd == 'show':
        q = a.name.lower()
        for e in load_index():
            if q in e.key.lower():
                print(json.dumps(e.to_dict(), indent=1))
        return 0
    if a.cmd == 'diff':
        with open(a.a, 'rb') as f:
            da = f.read()
        with open(a.b, 'rb') as f:
            db = f.read()
        rc = 0
        for i, d in compare_files(da, db):
            print('blob %s: %s' % (i, d))
            if not d.equal:
                rc = 1
        return rc
    ap.print_help()
    return 2


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
