#!/usr/bin/env python3
"""seed_baseline.py - write the parts of tests/baseline that no test run
produces: the list of pairs that cannot be reproduced, and the seeds of the
later milestones, copied from the composition spike.

    python -I tests/seed_baseline.py

    expected_fail.tsv   the 15 stale pairs (from the corpus index: the source
                        was edited after the class was compiled) and the 2
                        pairs compiled against another version of a class
                        they call (notes/compiler_spec.md 1.4 item 3)
    m5.pass             spec/out/spike_pass_oracle.txt       2,649 class files
    m5_older.pass       spec/out/spike_pass_older.txt          153 class files
    m5_float.tsv        spec/out/spike_float_modes.tsv       the float setting each file needs
    m6.pass             spec/out/spike_pass_source_only.txt    635 sources
    m6.sha256           spec/out/spike_source_only.sha256    hash of each of the 635 outputs

m1.pass, m2.pass, m3.pass, m3_model.pass and extra.pass are written by
run_tests.py --update.  The script overwrites what it wrote before and
touches nothing else.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import harness  # noqa: E402

SPIKE_OUT = os.path.join(harness.ROOT, 'spec', 'out')
COPIES = [
    ('spike_pass_oracle.txt', 'm5.pass', 2649),
    ('spike_pass_older.txt', 'm5_older.pass', 153),
    ('spike_float_modes.tsv', 'm5_float.tsv', 2802),
    ('spike_pass_source_only.txt', 'm6.pass', 635),
    ('spike_source_only.sha256', 'm6.sha256', 635),
]
# found only by compiling (spec/spike_check.py): the class was compiled
# against a version of another class that the install no longer has
UNREPRODUCIBLE = [
    ('parts/scripts/enginepart/slidingenginepart/reciprocatingenginepart/differential/'
     'Center_Differential.class',
     'holds a ref to Chassis.getBlock:()Ljava.game.parts.Part; the shipped Chassis '
     'declares getBlock:()Ljava.game.parts.enginepart.Block;'),
    ('sl/Scripts/game/Garage.class',
     'holds a ref to NetworkEngine.httpRequestAsync, which the shipped NetworkEngine '
     'no longer declares'),
]


def main():
    os.makedirs(harness.BASELINE_DIR, exist_ok=True)
    rows = []
    for e in harness.stale_pairs():
        reason = '; '.join(x for x in (e.stale or []) if not x.startswith('e.g.'))
        rows.append((e.cls, 'stale', '0x%X' % e.builds[0], reason))
    known = set(e.cls for e in harness.oracle_pairs())
    for cls, reason in UNREPRODUCIBLE:
        if cls not in known:
            raise SystemExit('%s is not an oracle pair' % cls)
        rows.append((cls, 'other class version', '0x24F9D', reason))
    rows.sort()
    path = os.path.join(harness.BASELINE_DIR, 'expected_fail.tsv')
    with open(path, 'w', encoding='utf-8', newline='\n') as f:
        f.write('# class file\twhy it cannot be reproduced\tbuild\tevidence\n')
        for r in rows:
            f.write('\t'.join(r) + '\n')
    print('expected_fail.tsv  %d pairs (%d stale, %d compiled against another class version)' % (
        len(rows), len(rows) - len(UNREPRODUCIBLE), len(UNREPRODUCIBLE)))
    for src, dst, want in COPIES:
        with open(os.path.join(SPIKE_OUT, src), 'r', encoding='utf-8') as f:
            lines = [x for x in f.read().split('\n') if x]
        items = [x for x in lines if not x.startswith('#')]
        if len(items) != want:
            raise SystemExit('%s holds %d items, expected %d' % (src, len(items), want))
        with open(os.path.join(harness.BASELINE_DIR, dst), 'w', encoding='utf-8',
                  newline='\n') as f:
            f.write('\n'.join(lines) + '\n')
        print('%-18s %d items from spec/out/%s' % (dst, len(items), src))
    return 0


if __name__ == '__main__':
    sys.exit(main())
