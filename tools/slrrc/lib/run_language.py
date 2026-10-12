#!/usr/bin/env python3
"""run_language.py - re-run the language proofs and regenerate notes/grammar.md.

    python -I run_language.py [--fast]

    1. check_parse.py         notes/parse_check.txt, .json     every source tokenizes and parses
    2. check_members.py       notes/members_check.txt, .json   declarations = class files
    3. check_shape.py         notes/shape_check.txt, .json     tree shapes = class files
    4. census.py              notes/grammar_census.txt, .json  how often each construct occurs
    5. study_class_dirs.py    notes/class_dirs.txt, .json      where named classes live
    6. check_sensitivity.py   notes/sensitivity.txt, .json     wrong grammars are noticed
    7. gen_grammar_md.py      notes/grammar.md

--fast skips step 6 (about 100 s) and reuses its last output.  Needs
notes/pairs.json (run_all.py or "oracle.py build").  Stops with a non-zero
status at the first step that fails.
"""
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
NOTES = os.path.normpath(os.path.join(HERE, '..', 'notes'))


def j(name):
    return os.path.join(NOTES, name)


STEPS = [
    ('check_parse.py', ['--json', j('parse_check.json')], 'parse_check.txt'),
    ('check_members.py', ['--json', j('members_check.json')], 'members_check.txt'),
    ('check_shape.py', ['--json', j('shape_check.json'), '--show', '100'], 'shape_check.txt'),
    ('census.py', ['--json', j('grammar_census.json')], 'grammar_census.txt'),
    ('study_class_dirs.py', ['--json', j('class_dirs.json')], 'class_dirs.txt'),
    ('check_sensitivity.py', ['--json', j('sensitivity.json')], 'sensitivity.txt'),
    ('gen_grammar_md.py', [], None),
]


def main(argv):
    fast = '--fast' in argv
    os.makedirs(NOTES, exist_ok=True)
    for script, args, out in STEPS:
        if fast and script == 'check_sensitivity.py' and os.path.exists(j('sensitivity.json')):
            print('%-24s skipped (--fast)' % script)
            continue
        t0 = time.time()
        cmd = [sys.executable, '-I', os.path.join(HERE, script)] + args
        r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        text = r.stdout.decode('utf-8', 'replace').replace('\r\n', '\n')
        if out:
            with open(j(out), 'w', encoding='utf-8', newline='\n') as f:
                f.write(text)
        last = [x for x in text.strip().split('\n') if x.strip()][-1:] or ['']
        print('%-24s exit %d  %6.1fs  %s' % (script, r.returncode, time.time() - t0,
                                             last[0][:100]))
        if r.returncode != 0:
            sys.stdout.write(text[-3000:])
            return r.returncode
    print('all steps passed')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
