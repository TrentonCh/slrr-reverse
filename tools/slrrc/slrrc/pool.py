"""pool.py - the constant pool of one class: an interning table.

A pool is a list of entries; an entry refers only to entries with a lower
index, and no entry exists twice (notes/foundation.md 3.2, rules P1 and P3).
Interning returns the index of the entry and appends it when it is new,
whatever it was first interned for.  The pool is a log: entries that nothing
refers to in the end stay in it.

    utf8(text)              text
    cls(name)               utf8 name, then the class entry
    nat(name, desc)         utf8 name, utf8 descriptor, then the nat entry
    ref(cls, name, desc)    the class (utf8, class), the nat (utf8, utf8, nat),
                            then the ref entry
    rid(pack, local_id)     utf8 pack file name, then the rid entry

The inner order is proven by the shipped pools (notes/declarations.md 6.5):
class before nat inside a ref (the other order fails 1,620 classes), name
before descriptor inside a nat (2,811).

The compiler appends to the pool in five steps: seed, types, names (decl.py),
resolve, emit (the writer, over the finished trees).

C++ (sc_pool.cpp): struct PoolEntry { EntryKind kind; uint32_t a, b;
std::string text; } and two hash maps.
"""

# the kind word of an entry in the CONS chunk
UTF8 = 0
RID = 3
CLASS = 4
REF = 5
NAT = 7

NONE = 0xFFFFFFFF


class PoolEntry(object):
    __slots__ = ('kind', 'a', 'b', 'text')

    def __init__(self, kind, a, b, text):
        self.kind = kind
        self.a = a          # CLASS: utf8 of the name   NAT: utf8 of the name
        #                     REF: class entry          RID: utf8 of the pack file name
        self.b = b          # NAT: utf8 of the descriptor   REF: nat entry   RID: local id
        self.text = text    # UTF8: the text (one character per byte)


class Pool(object):
    def __init__(self):
        self.entries = []
        self.utf8_index = {}        # text -> index
        self.index = {}             # (kind, a, b) -> index

    def utf8(self, text):
        i = self.utf8_index.get(text)
        if i is None:
            i = len(self.entries)
            self.utf8_index[text] = i
            self.entries.append(PoolEntry(UTF8, 0, 0, text))
        return i

    def _entry(self, kind, a, b):
        key = (kind, a, b)
        i = self.index.get(key)
        if i is None:
            i = len(self.entries)
            self.index[key] = i
            self.entries.append(PoolEntry(kind, a, b, ''))
        return i

    def cls(self, name):
        return self._entry(CLASS, self.utf8(name), 0)

    def nat(self, name, desc):
        n = self.utf8(name)
        d = self.utf8(desc)
        return self._entry(NAT, n, d)

    def ref(self, cls, name, desc):
        c = self.cls(cls)
        n = self.nat(name, desc)
        return self._entry(REF, c, n)

    def rid(self, pack, local_id):
        return self._entry(RID, self.utf8(pack), local_id)

    def find_utf8(self, text):
        """Index of a text that is in the pool, NONE when it is not."""
        return self.utf8_index.get(text, NONE)

    def to_cons(self):
        """The entries as the tuples lib/tufa.py reads and writes (tests)."""
        out = []
        for e in self.entries:
            if e.kind == UTF8:
                out.append(('utf8', e.text))
            elif e.kind == CLASS:
                out.append(('class', e.a))
            elif e.kind == NAT:
                out.append(('nat', e.a, e.b))
            elif e.kind == REF:
                out.append(('ref', e.a, e.b))
            else:
                out.append(('rid', e.a, e.b))
        return out
