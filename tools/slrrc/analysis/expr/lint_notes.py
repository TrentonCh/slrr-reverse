#!/usr/bin/env python3
"""lint_notes.py - mechanical checks of notes/expressions.md after generation:
every table row has as many cells as its header, no <placeholder> stands
outside a code span (a renderer would take it for HTML), code spans are
closed, and code fences are balanced.

    python -I lint_notes.py
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
NOTES = os.path.normpath(os.path.join(HERE, '..', '..', 'notes', 'expressions.md'))


def main(argv):
    with open(NOTES, encoding='utf-8') as f:
        lines = f.read().split('\n')
    in_code = False
    hdr = None
    bad = 0
    for i, line in enumerate(lines, 1):
        if line.startswith('```'):
            in_code = not in_code
            continue
        if in_code:
            continue
        if line.startswith('|'):
            cells = len(re.findall(r'(?<!\\)\|', line)) - 1
            if hdr is None:
                hdr = cells
            elif cells != hdr:
                bad += 1
                print('line %d: %d cells, header has %d: %s' % (i, cells, hdr, line[:80]))
        else:
            hdr = None
        segs = line.split('`')
        if len(segs) % 2 == 0:
            bad += 1
            print('line %d: unclosed code span: %s' % (i, line[:100]))
        for j in range(0, len(segs), 2):
            if re.search(r'<\w+>', segs[j]):
                bad += 1
                print('line %d: placeholder outside a code span: %s' % (i, line[:100]))
    if in_code:
        bad += 1
        print('unbalanced code fence')
    print('%s: %d lines, %d problems' % (NOTES, len(lines), bad))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
