           Raspberry Pi Precision Clock Discipline without GPS
                      (c) 2020 Andreas Steinmetz

--------------------------------------------------------------------------


  Raspberry Pi Precision Clock Discipline using the DS3231 chip.
===================================================================

If you are in need for precise timing on a Raspberry Pi but can't
rely on GPS reception or NTP via (W)LAN, you can use a DS3231 breakout
as a sufficiently precise replacement.

If you can spare some time you can tune the chip precision to a
few seconds a year which should be fine enough for most use cases.

The system clock then does have the same precision if you
discipline it by the DS3231 PPS output.

Please see the file HOWTO.txt on how to use this stuff.

As an Add-On the archive contains an access library for the 24Cxx
eproms contained on some breakout boards.
