#!/usr/bin/env python3
"""check_roundtrip.py - proof 1: tufa.py reads and re-writes every class file
of the corpus byte for byte.

    python -I check_roundtrip.py [--corpus DIR] [--json OUT]

For every .class file: parse (strict), serialise, compare with the file bytes;
also run the JSON form through to_dict / from_dict and serialise again, and run
tufa.validate on every blob.  Prints the counts and every failure with its
reason; exit status 0 only when everything is identical.
"""
import json
import os
import sys
from collections import Counter

_HERE = os.path.dirname(os.path.abspath(__file__))
if _HERE not in sys.path:
    sys.path.insert(0, _HERE)

import oracle  # noqa: E402
import tufa    # noqa: E402


def main(argv):
    out_json = None
    if '--corpus' in argv:
        os.environ['SLRR_CORPUS'] = argv[argv.index('--corpus') + 1]
    if '--json' in argv:
        out_json = argv[argv.index('--json') + 1]
    root = oracle.corpus_root()
    files = oracle.find_class_files(root)
    st = Counter()
    fails = []
    layouts = Counter()
    builds_files = Counter()
    builds_blobs = Counter()
    blobs_per_file = Counter()
    total_bytes = 0
    for rel in files:
        st['files'] += 1
        data = oracle.load_class_bytes(rel, root)
        total_bytes += len(data)
        try:
            blobs = tufa.parse(data, rel)
        except tufa.TufaError as x:
            st['parse_error'] += 1
            fails.append((rel, 'parse: %s' % x))
            continue
        st['parsed'] += 1
        st['blobs'] += len(blobs)
        blobs_per_file[len(blobs)] += 1
        for b in set(x.build for x in blobs):
            builds_files[b] += 1
        # the blobs must tile the file exactly
        pos = 0
        for b in blobs:
            if b.offset != pos:
                fails.append((rel, 'blob at 0x%X, expected 0x%X' % (b.offset, pos)))
            pos += b.size
            builds_blobs[b.build] += 1
            layouts[' '.join(b.chunks)] += 1
            if list(b.chunks) != tufa.canonical_chunks(b):
                st['noncanonical_layout'] += 1
                fails.append((rel, 'chunk list %s is not canonical %s' % (
                    b.chunks, tufa.canonical_chunks(b))))
            probs = tufa.validate(b)
            if probs:
                st['blobs_with_validate_problems'] += 1
                fails.append((rel, 'validate: ' + '; '.join(probs[:5])))
        if pos != len(data):
            fails.append((rel, 'blobs cover %d of %d bytes' % (pos, len(data))))
        again = tufa.serialise(blobs)
        if again == data:
            st['identical'] += 1
        else:
            st['different'] += 1
            n = min(len(again), len(data))
            off = next((i for i in range(n) if again[i] != data[i]), n)
            fails.append((rel, 'serialise differs at 0x%X (sizes %d / %d)'
                          % (off, len(again), len(data))))
        # JSON form
        via = [tufa.Blob.from_dict(json.loads(json.dumps(b.to_dict()))) for b in blobs]
        if tufa.serialise(via) == data:
            st['identical_via_json'] += 1
        else:
            st['different_via_json'] += 1
            fails.append((rel, 'JSON round trip differs'))
        # canonical chunk list must regenerate the same bytes too
        if b''.join(tufa.serialise_blob(b, tufa.canonical_chunks(b)) for b in blobs) == data:
            st['identical_canonical'] += 1
    print('corpus            %s' % root)
    print('class files       %d  (%d bytes)' % (st['files'], total_bytes))
    print('parsed strictly   %d' % st['parsed'])
    print('parse errors      %d' % st['parse_error'])
    print('blobs             %d' % st['blobs'])
    print('byte-identical    %d of %d' % (st['identical'], st['files']))
    print('  via JSON dict   %d of %d' % (st['identical_via_json'], st['files']))
    print('  canonical list  %d of %d' % (st['identical_canonical'], st['files']))
    print('validate problems %d blobs' % st['blobs_with_validate_problems'])
    print('chunk layouts (per blob):')
    for k, v in layouts.most_common():
        print('  %-28s %d' % (k, v))
    print('builds (files / blobs):')
    for b in sorted(builds_files, reverse=True):
        print('  0x%X  %d / %d' % (b, builds_files[b], builds_blobs[b]))
    print('blobs per file:')
    for k in sorted(blobs_per_file):
        print('  %d  %d' % (k, blobs_per_file[k]))
    print('failures          %d' % len(fails))
    for rel, why in fails:
        print('  %s: %s' % (rel, why))
    if out_json:
        with open(out_json, 'w', encoding='utf-8', newline='\n') as f:
            json.dump({'counts': dict(st), 'layouts': dict(layouts),
                       'builds_files': {('0x%X' % k): v for k, v in builds_files.items()},
                       'builds_blobs': {('0x%X' % k): v for k, v in builds_blobs.items()},
                       'failures': fails, 'bytes': total_bytes}, f, indent=1)
    return 0 if not fails and st['identical'] == st['files'] else 1


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
