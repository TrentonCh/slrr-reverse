#!/usr/bin/env python3
"""pack_oracle.py - the pack spellings of the shipped pools, as a test oracle.

    TABLE_940           the one spelling each of the 126 packs has in the
                        pools of the 2,993 shipped classes (7,242 rid entries)
    TablePackNames      a PackNames that answers from such a table

Until the exe was read this table was what the compiler answered from.  It is
not an input any more: the spelling of a pack is the spelling of its first
registration in the pack table of the session (slrrc/packs.py,
notes/exe_reads.md 2), and the package replays that table.  The table stays
here for two things:

  - as the oracle the replayed table is compared with: every spelling of the
    table has to be the name of a record after the sources were compiled
    (tests/t4_trees.py, tests/t9_exe.py);
  - as a PackNames without state for the synthetic sources of the tests
    (TablePackNames(TABLE_940, None)).

tests/gen_pack_table.py prints the table from the pools; tests/t4_trees.py
checks that it still equals what the pools hold.  Three entries differ from
the directory listing of the install (cars\\traffic\\Civilvan_2.rpk,
Civilvan_3.rpk, maps\\ROC_track\\smallmap.rpk: nothing scans those
directories, so the spelling of the sources was registered first), and one
names a pack that has no file any more (old_stdpack_231).
"""
import os
import sys

sys.path.insert(0, os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                                 '..')))

from slrrc import packs     # noqa: E402

lower = packs.lower


class TablePackNames(packs.PackNames):
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
