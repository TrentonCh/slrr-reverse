"""packs.py - the pool spelling of the pack a resource id names.

A resource id literal  cars.racers.duhen:0x0000002Ar  becomes the pool entry
{utf8 pack file name, local id}.  The file name is the written name with the
dots turned into backslashes and ".rpk" appended, but not in the letter case
of the source: the pool of a shipped class holds "cars\\racers\\Duhen.rpk".

WHERE THE GAME TAKES THE SPELLING FROM IS AN OPEN QUESTION
(notes/compiler_spec.md 1.4 item 2).  What the shipped classes show:

  - every pack has exactly one spelling over all 2,993 shipped pools (126
    packs).  That is what an interning pack table of the resource system
    gives: the first spelling it met in that session;
  - the spelling is the one of the source for 8,037 of the 9,058 resource id
    nodes of the oracle pairs and the one of the pack file on disk for 9,046;
    three packs are spelled as neither the directory listing nor every source
    spells them;
  - a pack that has no file compiles to "system.rpk" with local id 0
    (observed once: the 2 literals of TimeAttackTest).

So the compiler does not decide: it asks its host through the interface
PackNames.  TablePackNames is the implementation for a host that has nothing
better than this evidence: the one spelling each pack has in the shipped
pools (TABLE_940), then the spelling of the pack file as the host lists it,
then the name as written.

    packs = TablePackNames(TABLE_940, files)      # files: the pack files of the install
    packs.canonical('cars\\racers\\duhen.rpk')    # 'cars\\racers\\Duhen.rpk', or None

Nothing here reads a directory: the host passes the list of files.

C++ (script_compiler.hpp): struct PackNames { virtual bool canonical(
std::string_view written_path, std::string* file_name) = 0; }; false is the
None of the reference.
"""

NO_FILE_PACK = 'system.rpk'     # what a pack without a file compiles to,
NO_FILE_ID = 0                  # with this local id


class PackNames(object):
    """The canonical file name of a pack.

    THE CONTRACT.  canonical() has two answers and they are not alike:

      a name    the spelling for the pool.  A host that cannot tell whether
                the pack exists answers with a name too: the spelling it
                knows, or written_path itself.
      None      the INSTALL HAS NO FILE for this pack.  The compiler then
                writes system.rpk id 0 in place of the resource id, the one
                form a shipped class shows for such a pack, and warns
                (W_PACK) at the literal: the class will name another
                resource than its source does.

    So None is a statement about the files of the install, and has to be
    answered from them (a listing of its *.rpk files), not from the packs
    the host happens to have opened: a pack that is not open yet, asked for
    while a script compiles, would turn every resource id of that pack into
    system.rpk:0x00000000.  The answer for a path does not change while a
    compilation runs; the compiler asks once per pack (CachedPackNames)."""

    def canonical(self, written_path):
        """written_path: the pack as the source names it, in path form
        (pack_path).  Returns the spelling for the pool, or None when the
        install has no file for the pack (never for "not known")."""
        return written_path


class CachedPackNames(PackNames):
    """A PackNames that asks another one once per written path: what one
    compilation puts between the emitter and the PackNames of its host."""

    def __init__(self, inner):
        self.inner = inner
        self.answers = {}           # written path -> the answer (a name or None)

    def canonical(self, written_path):
        if written_path in self.answers:
            return self.answers[written_path]
        name = self.inner.canonical(written_path)
        self.answers[written_path] = name
        return name


def pack_path(written):
    """'cars.racers.Axis' -> 'cars\\racers\\Axis.rpk': the file a written
    pack name stands for, relative to the install, in the letter case of the
    source."""
    return written.replace('.', '\\') + '.rpk'


def lower(text):
    """ASCII lower case (pack names are compared without regard to case)."""
    out = []
    for ch in text:
        if 'A' <= ch <= 'Z':
            out.append(chr(ord(ch) + 32))
        else:
            out.append(ch)
    return ''.join(out)


class TablePackNames(PackNames):
    """table: spellings that win over everything else (TABLE_940).
    files: the pack files the host has, as relative paths with backslashes
    in the spelling of its directory listing; None when the host cannot say,
    in which case every pack is taken to have a file.

        pack has no file            None
        pack is in the table        the spelling of the table
        otherwise                   the spelling of the file
        files is None               the table, else the name as written"""

    def __init__(self, table, files):
        self.table = {}
        for p in table:
            self.table[lower(p)] = p
        self.files = None
        if files is not None:
            self.files = {}
            for p in files:
                self.files[lower(p)] = p

    def canonical(self, written_path):
        key = lower(written_path)
        if self.files is None:
            return self.table.get(key, written_path)
        on_disk = self.files.get(key)
        if on_disk is None:
            return None
        return self.table.get(key, on_disk)


# The one spelling each pack has in the pools of the shipped classes (Build
# 940 install, 2,993 classes, 7,242 rid entries).  Generated by
# tests/gen_pack_table.py; tests/t4_trees.py checks it against the pools.
# Three entries differ from the directory listing of the install
# (cars\traffic\Civilvan_2.rpk, Civilvan_3.rpk, maps\ROC_track\smallmap.rpk),
# and one names a pack that has no file any more (old_stdpack_231).
#
# TABLE_940_VERSION names this table in a cache manifest: an output with a
# resource id depends on it.  Change it with every change of the table.
TABLE_940_VERSION = '940-shipped-pools-1'
TABLE_940 = (
    r'cars.rpk',
    r'cars\misc\Police.rpk',
    r'cars\racers.rpk',
    r'cars\racers\Axis.rpk',
    r'cars\racers\Badge.rpk',
    r'cars\racers\Baiern.rpk',
    r'cars\racers\Codrac.rpk',
    r'cars\racers\Coyot.rpk',
    r'cars\racers\Duhen.rpk',
    r'cars\racers\Einvagen.rpk',
    r'cars\racers\Enula.rpk',
    r'cars\racers\Focer.rpk',
    r'cars\racers\Furrano.rpk',
    r'cars\racers\Kurumma.rpk',
    r'cars\racers\MC.rpk',
    r'cars\racers\Naxas.rpk',
    r'cars\racers\Ninja.rpk',
    r'cars\racers\Nonus.rpk',
    r'cars\racers\Prime.rpk',
    r'cars\racers\Remo.rpk',
    r'cars\racers\ST9.rpk',
    r'cars\racers\Stallion.rpk',
    r'cars\racers\Sunset.rpk',
    r'cars\racers\SuperDuty.rpk',
    r'cars\racers\Teg.rpk',
    r'cars\racers\Whisper.rpk',
    r'cars\racers\Yotta.rpk',
    r'cars\racers\Zed.rpk',
    r'cars\traffic\Ambulance.rpk',
    r'cars\traffic\ArmoredVan.rpk',
    r'cars\traffic\CivilVan.rpk',
    r'cars\traffic\CivilVan_2.rpk',
    r'cars\traffic\CivilVan_3.rpk',
    r'cars\traffic\Coach.rpk',
    r'cars\traffic\Erbilac.rpk',
    r'cars\traffic\Erbilac_2.rpk',
    r'cars\traffic\Erbilac_3.rpk',
    r'cars\traffic\FireEngine.rpk',
    r'cars\traffic\Peterbild.rpk',
    r'cars\traffic\Peterbild_2.rpk',
    r'cars\traffic\Peterbild_3.rpk',
    r'cars\traffic\Pickup.rpk',
    r'cars\traffic\Pickup_2.rpk',
    r'cars\traffic\Pickup_3.rpk',
    r'cars\traffic\Schoolbus.rpk',
    r'cars\traffic\Taxi.rpk',
    r'cars\traffic\Wagon.rpk',
    r'frontend.rpk',
    r'humans.rpk',
    r'maps\city.rpk',
    r'maps\city\smallmap.rpk',
    r'maps\ROC_track.rpk',
    r'maps\ROC_Track\smallmap.rpk',
    r'maps\skydome.rpk',
    r'maps\test_track.rpk',
    r'maps\test_track\smallmap.rpk',
    r'misc\carlot.rpk',
    r'misc\catalog.rpk',
    r'misc\dealer.rpk',
    r'misc\dealer2.rpk',
    r'misc\dealer3.rpk',
    r'misc\garage.rpk',
    r'misc\garage_objects.rpk',
    r'misc\paintbooth.rpk',
    r'multibot\maps\A1Ring.rpk',
    r'multibot\maps\A1Ring\t_data.rpk',
    r'multibot\maps\DerbyArena.rpk',
    r'multibot\maps\DerbyArena\t_data.rpk',
    r'multibot\maps\DragStrip.rpk',
    r'multibot\maps\DragStrip\t_data.rpk',
    r'multibot\maps\GatewayRoad.rpk',
    r'multibot\maps\GatewayRoad\t_data.rpk',
    r'multibot\maps\HockenheimGP.rpk',
    r'multibot\maps\HockenheimGP\t_data.rpk',
    r'multibot\maps\LagunaSeca.rpk',
    r'multibot\maps\LagunaSeca\t_data.rpk',
    r'multibot\maps\NationalDirtRaceway.rpk',
    r'multibot\maps\NationalDirtRaceway\t_data.rpk',
    r'multibot\maps\Norisring.rpk',
    r'multibot\maps\Norisring\t_data.rpk',
    r'multibot\maps\Nurburgring.rpk',
    r'multibot\maps\Nurburgring\t_data.rpk',
    r'multibot\maps\OranPark.rpk',
    r'multibot\maps\OranPark\t_data.rpk',
    r'multibot\maps\OultonPark.rpk',
    r'multibot\maps\OultonPark\t_data.rpk',
    r'multibot\maps\ROCTrack\t_data.rpk',
    r'multibot\maps\Sandown.rpk',
    r'multibot\maps\Sandown\t_data.rpk',
    r'multibot\maps\SPAFrankoohapsGPCircuit.rpk',
    r'multibot\maps\SPAFrankoohapsGPCircuit\t_data.rpk',
    r'multibot\maps\TestTrack\t_data.rpk',
    r'multibot\maps\UKRallyStage1.rpk',
    r'multibot\maps\UKRallyStage1\t_data.rpk',
    r'multibot\maps\UKRallyStage2.rpk',
    r'multibot\maps\UKRallyStage2\t_data.rpk',
    r'multibot\maps\UKRallyStage3.rpk',
    r'multibot\maps\UKRallyStage3\t_data.rpk',
    r'multibot\maps\Vallelunga.rpk',
    r'multibot\maps\Vallelunga\t_data.rpk',
    r'multibot\maps\ValoCity\t_data.rpk',
    r'multibot\maps\Zandvoort.rpk',
    r'multibot\maps\Zandvoort\t_data.rpk',
    r'multibot\scripts.rpk',
    r'multibot\sounds.rpk',
    r'objects.rpk',
    r'particles.rpk',
    r'parts.rpk',
    r'parts\engines\Baiern_Emer.rpk',
    r'parts\engines\Callaway_Cadillac_Bugatti_V16.rpk',
    r'parts\engines\Dodge_BMW_Racing_V10.rpk',
    r'parts\engines\Einvagen_Duhen_Ishima_Focer.rpk',
    r'parts\engines\GMC_Nissan_Honda_Hyundai_Opel.rpk',
    r'parts\engines\Lama_Performance_V12.rpk',
    r'parts\engines\MC_Prime_SuperDuty.rpk',
    r'parts\interior.rpk',
    r'parts\mufflers.rpk',
    r'parts\pedals.rpk',
    r'parts\wheels.rpk',
    r'parts\wings.rpk',
    r'sl.rpk',
    r'sl\Scripts\game\CareerEvents\old_stdpack_231.rpk',
    r'sl\Scripts\game\CareerEvents\stdpack_231.rpk',
    r'sl\Scripts\game\Gamemodes\stdpack_231.rpk',
    r'sound.rpk',
    r'system.rpk',
)
