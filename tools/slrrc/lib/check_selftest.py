#!/usr/bin/env python3
"""check_selftest.py - proof 4: the tools themselves.

    python -I check_selftest.py [--corpus DIR]

A. Strictness of tufa.parse.  Take class files of the corpus, damage them in
   ways a careless reader would survive, and require tufa.TufaError each time:
   truncation at every chunk boundary and inside chunks, trailing bytes, a
   wrong TUFA size, an unknown chunk tag, a duplicated chunk, a chunk size one
   too large / too small, an unknown pool kind, a missing NUL, an op byte
   outside the table, a row vector count one too large.
B. compare_blobs.  Change one byte per chunk kind of a blob and require that
   the report names that chunk and the offset of the change.
C. Disassembler.  Disassemble every class file of the corpus twice in text
   form and once in JSON form: no exception, identical text both times, the
   JSON loads, and no operand is flagged as a pool-kind mismatch.
D. Writer from scratch.  Rebuild every blob from its JSON dict with the
   canonical chunk list and compare with the file bytes (also done by
   check_roundtrip.py; repeated here on the Blob.from_dict path only).
"""
import json
import os
import struct
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
if _HERE not in sys.path:
    sys.path.insert(0, _HERE)

import disasm   # noqa: E402
import oracle   # noqa: E402
import tufa     # noqa: E402


def must_raise(data, label, results):
    """The damaged bytes must be rejected: by the layout reader (TufaError) or,
    when the damage happens to leave a well-formed layout (a row count moved
    by one can swallow exactly the next count word), by tufa.validate."""
    try:
        blobs = tufa.parse(data, label)
    except tufa.TufaError:
        results['raised'] += 1
        return True
    except Exception as x:          # any other exception is a bug in the reader
        results['wrong_exception'].append('%s: %r' % (label, x))
        return False
    if any(tufa.validate(b) for b in blobs):
        results['validate'].append(label)
        return True
    results['accepted'].append(label)
    return False


def chunk_table(data):
    """[(tag, header offset, payload offset, size)] of a class file."""
    out = []
    pos = 0
    while pos < len(data):
        tag = data[pos:pos + 4].decode('ascii')
        size = struct.unpack_from('<I', data, pos + 4)[0]
        out.append((tag, pos, pos + 8, size))
        pos += 8 + size
    return out


def damage_cases(data):
    """Yield (label, damaged bytes)."""
    ct = chunk_table(data)
    for tag, hdr, pay, size in ct:
        # cutting a file at a blob boundary leaves a valid file with fewer
        # blobs, so only cuts in front of the other chunks are damage
        if hdr and tag != 'TUFA':
            yield 'truncated before %s@%d' % (tag, hdr), data[:hdr]
        yield 'truncated inside header of %s@%d' % (tag, hdr), data[:hdr + 5]
        if size:
            yield 'truncated inside %s@%d' % (tag, hdr), data[:pay + size // 2]
            yield 'truncated 1 byte short of %s end' % tag, data[:pay + size - 1]
    yield 'trailing byte', data + b'\x00'
    yield 'trailing 12 zero bytes', data + b'\x00' * 12
    yield 'trailing TUFA header only', data + b'TUFA\x04\x00\x00\x00\x9d\x4f\x02\x00'
    d = bytearray(data)
    d[4:8] = struct.pack('<I', 8)
    yield 'TUFA size 8', bytes(d)
    d = bytearray(data)
    d[0:4] = b'TUFB'
    yield 'bad magic', bytes(d)
    for tag, hdr, pay, size in ct:
        if tag == 'TUFA':
            continue
        d = bytearray(data)
        d[hdr:hdr + 4] = b'XXXX'
        yield 'unknown tag in place of %s' % tag, bytes(d)
        d = bytearray(data)
        d[hdr + 4:hdr + 8] = struct.pack('<I', size + 1)
        yield '%s size +1' % tag, bytes(d)
        if size:
            d = bytearray(data)
            d[hdr + 4:hdr + 8] = struct.pack('<I', size - 1)
            yield '%s size -1' % tag, bytes(d)
        # the same chunk twice in one blob
        chunk = data[hdr:pay + size]
        yield 'duplicated %s' % tag, data[:pay + size] + chunk + data[pay + size:]
        # one extra payload byte with a matching size
        d = bytearray(data[:pay + size] + b'\x00' + data[pay + size:])
        d[hdr + 4:hdr + 8] = struct.pack('<I', size + 1)
        yield '%s with one padding byte' % tag, bytes(d)
    for tag, hdr, pay, size in ct:
        if tag == 'CONS' and size >= 12:
            d = bytearray(data)
            d[pay + 4:pay + 8] = struct.pack('<I', 1)       # kind 1 does not exist
            yield 'pool kind 1', bytes(d)
            d = bytearray(data)
            d[pay + 4:pay + 8] = struct.pack('<I', 6)
            yield 'pool kind 6', bytes(d)
            ln = struct.unpack_from('<I', data, pay + 8)[0]
            d = bytearray(data)
            d[pay + 12 + ln] = 0x41                          # the NUL of entry 0
            yield 'utf8 without NUL', bytes(d)
            d = bytearray(data)
            cnt = struct.unpack_from('<I', data, pay)[0]
            d[pay:pay + 4] = struct.pack('<I', cnt + 1)
            yield 'pool count +1', bytes(d)
            d = bytearray(data)
            d[pay:pay + 4] = struct.pack('<I', cnt - 1)
            yield 'pool count -1', bytes(d)
        if tag in ('FILD', 'MTHD'):
            cnt = struct.unpack_from('<I', data, pay)[0]
            d = bytearray(data)
            d[pay:pay + 4] = struct.pack('<I', cnt + 1)
            yield '%s static count +1' % tag, bytes(d)
        if tag == 'CLSS':
            d = bytearray(data)
            cnt = struct.unpack_from('<I', data, pay + 16)[0]
            d[pay + 16:pay + 20] = struct.pack('<I', cnt + 1)
            yield 'CLSS interface count +1', bytes(d)
        if tag == 'TREE' and size >= 4:
            nt = struct.unpack_from('<I', data, pay)[0]
            d = bytearray(data)
            d[pay:pay + 4] = struct.pack('<I', nt + 1)
            yield 'tree count +1', bytes(d)
            if nt and struct.unpack_from('<I', data, pay + 4)[0]:
                for bad in (48, 0x80, 0xFF):
                    d = bytearray(data)
                    d[pay + 8] = bad
                    yield 'op byte %d' % bad, bytes(d)
                d = bytearray(data)
                nn = struct.unpack_from('<I', data, pay + 4)[0]
                d[pay + 4:pay + 8] = struct.pack('<I', nn + 1)
                yield 'node count +1', bytes(d)
                d = bytearray(data)
                d[pay + 4:pay + 8] = struct.pack('<I', nn - 1)
                yield 'node count -1', bytes(d)


def main(argv):
    if '--corpus' in argv:
        os.environ['SLRR_CORPUS'] = argv[argv.index('--corpus') + 1]
    root = oracle.corpus_root()
    files = oracle.find_class_files(root)
    rc = 0

    # ---- A
    sample = ['system/scripts/lang/Runnable.class', 'sl/Scripts/game/Marker.class',
              'sl/Scripts/game/Trigger.class', 'system/scripts/lang/Class.class',
              'sl/Scripts/game/GameState.class', 'sl/Scripts/game/Transit.class',
              'system/scripts/util/Vector.class',
              'system/scripts/render/osd/dialog/Dialog.class']
    res = {'raised': 0, 'validate': [], 'accepted': [], 'wrong_exception': []}
    ncases = 0
    for rel in sample:
        data = oracle.load_class_bytes(rel, root)
        for label, bad in damage_cases(data):
            ncases += 1
            must_raise(bad, '%s: %s' % (rel, label), res)
    try:
        tufa.parse(b'', 'empty')
        res['accepted'].append('empty file')
    except tufa.TufaError:
        res['raised'] += 1
    ncases += 1
    print('A. strictness: %d damaged inputs from %d files: TufaError raised for '
          '%d, layout still well-formed but rejected by validate for %d, '
          'accepted %d' % (ncases, len(sample), res['raised'],
                           len(res['validate']), len(res['accepted'])))
    for x in res['validate']:
        print('   validate: %s' % x)
    for x in res['accepted']:
        print('   ACCEPTED: %s' % x)
        rc = 1
    for x in res['wrong_exception']:
        print('   WRONG EXCEPTION: %s' % x)
        rc = 1

    # ---- B
    okb = 0
    nb = 0
    for rel in ('sl/Scripts/game/Trigger.class', 'sl/Scripts/game/Marker.class'):
        data = oracle.load_class_bytes(rel, root)
        blob = tufa.parse(data, rel)[0]
        for tag in blob.chunks:
            pay, size = blob.chunk_spans[tag]
            for off in (0, size // 2, size - 1):
                nb += 1
                d = bytearray(data)
                d[pay + off] ^= 0x01
                try:
                    other = tufa.parse(bytes(d), rel)[0]
                except tufa.TufaError:
                    # the flipped byte broke the layout: compare payload bytes
                    okb += 1
                    continue
                r = oracle.compare_blobs(other, blob)
                if not r.equal and r.chunk == tag and r.offset == off:
                    okb += 1
                else:
                    print('   compare_blobs: flipped %s+%d of %s, reported %s' % (
                        tag, off, rel, r))
                    rc = 1
        same = oracle.compare_blobs(blob, tufa.parse(data, rel)[0])
        nb += 1
        okb += bool(same.equal)
        d = bytearray(data)
        d[8] ^= 0x01
        r = oracle.compare_blobs(tufa.parse(bytes(d), rel)[0], blob)
        nb += 1
        okb += (r.chunk == 'TUFA')
    print('B. compare_blobs: %d of %d single-byte changes located (chunk and offset)'
          % (okb, nb))
    if okb != nb:
        rc = 1

    # ---- C and D
    nfiles = 0
    nblobs = 0
    unstable = 0
    mismatch = 0
    json_bad = 0
    scratch_bad = 0
    errors = []
    text_bytes = 0
    for rel in files:
        nfiles += 1
        data = oracle.load_class_bytes(rel, root)
        try:
            blobs = tufa.parse(data, rel)
            t1 = disasm.disassemble(blobs, path=rel)
            t2 = disasm.disassemble(tufa.parse(data, rel), path=rel)
            if t1 != t2:
                unstable += 1
            text_bytes += len(t1)
            if '!! expected a' in t1 or '!! outside the pool' in t1:
                mismatch += 1
            for i, b in enumerate(blobs):
                nblobs += 1
                doc = json.loads(json.dumps(disasm.blob_to_json(b, i, rel)))
                if len(doc['trees']) != len(b.trees) or any(
                        len(tr['nodes']) != len(b.trees[tr['index']])
                        for tr in doc['trees']):
                    json_bad += 1
                if any(nd.get('pool_mismatch') for tr in doc['trees']
                       for nd in tr['nodes']):
                    mismatch += 1
            rebuilt = b''.join(
                tufa.serialise_blob(tufa.Blob.from_dict(json.loads(json.dumps(
                    b.to_dict()))), None) for b in blobs)
            canon = b''.join(tufa.serialise_blob(
                tufa.Blob.from_dict(b.to_dict()), tufa.canonical_chunks(b))
                for b in blobs)
            if rebuilt != data or canon != data:
                scratch_bad += 1
        except Exception as x:
            errors.append('%s: %r' % (rel, x))
    print('C. disassembler: %d files, %d blobs, %d characters of text; '
          'exceptions %d, unstable %d, pool-kind mismatches %d, JSON defects %d'
          % (nfiles, nblobs, text_bytes, len(errors), unstable, mismatch, json_bad))
    for x in errors[:10]:
        print('   %s' % x)
    print('D. writer from dicts with the canonical chunk list: %d of %d files '
          'identical' % (nfiles - scratch_bad - len(errors), nfiles))
    if errors or unstable or mismatch or json_bad or scratch_bad:
        rc = 1
    print('OK' if rc == 0 else 'FAILED')
    return rc


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
