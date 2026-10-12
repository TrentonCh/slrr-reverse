#!/usr/bin/env python3
"""run_all.py - regenerate every note and re-run every proof, in order.

    python -I run_all.py

    1. oracle.py build            notes/pairs.json, notes/corpus.md
    2. check_roundtrip.py         notes/roundtrip.txt, .json
    3. check_layout.py            notes/layout.txt, .json
    4. check_opcodes.py           notes/opcode_checks.txt, .json
    5. gen_opcodes_md.py          notes/opcodes.md
    6. check_selftest.py          notes/selftest.txt
    7. study_float_literals.py    notes/float_literals.txt, .json

Stops with a non-zero status at the first step that fails.  notes/foundation.md
is written by hand and quotes the numbers of these outputs.
"""
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
NOTES = os.path.normpath(os.path.join(HERE, '..', 'notes'))

STEPS = [
    ('oracle.py', ['build'], None),
    ('check_roundtrip.py', ['--json', os.path.join(NOTES, 'roundtrip.json')], 'roundtrip.txt'),
    ('check_layout.py', ['--json', os.path.join(NOTES, 'layout.json')], 'layout.txt'),
    ('check_opcodes.py', ['--json', os.path.join(NOTES, 'opcode_checks.json')], 'opcode_checks.txt'),
    ('gen_opcodes_md.py', [], None),
    ('check_selftest.py', [], 'selftest.txt'),
    ('study_float_literals.py', ['--json', os.path.join(NOTES, 'float_literals.json')],
     'float_literals.txt'),
]


def main():
    os.makedirs(NOTES, exist_ok=True)
    for script, args, out in STEPS:
        t0 = time.time()
        cmd = [sys.executable, '-I', os.path.join(HERE, script)] + args
        r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        text = r.stdout.decode('utf-8', 'replace').replace('\r\n', '\n')
        if out:
            with open(os.path.join(NOTES, out), 'w', encoding='utf-8', newline='\n') as f:
                f.write(text)
        last = [x for x in text.strip().split('\n') if x.strip()][-1:] or ['']
        print('%-26s exit %d  %5.1fs  %s' % (script, r.returncode, time.time() - t0,
                                             last[0][:90]))
        if r.returncode != 0:
            sys.stdout.write(text[-3000:])
            return r.returncode
    print('all steps passed')
    return 0


if __name__ == '__main__':
    sys.exit(main())
