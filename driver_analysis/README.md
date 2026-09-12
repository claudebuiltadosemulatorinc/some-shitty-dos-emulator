# DD58463.exe — identification and extraction

## Short answer

It is not a game. It is a **Dell driver diskette set**: the **Yamaha OPL3SA2 audio
drivers for Windows 95, revision A05**, for **Dell Dimension XPS D-series** desktops,
created **19 January 1998**.

The floppy was almost certainly labelled "Files for game" because on a 1997–98 PC the
OPL3-SA2 chip *was* the game sound — FM synthesis, MPU-401 MIDI and the joystick port
all live on it. Section 11 of the driver README is literally titled *"Using software
wavetable in a virtual DOS box application"* and explains how to point a DOS game at
MPU-401 on I/O 330 for better music.

## What the files are

| File | What it is |
|---|---|
| `DE584631.EXE` (177 KB) | Compressed image of **diskette 1 of 2** |
| `DE584632.EXE` (841 KB) | Compressed image of **diskette 2 of 2** |
| `MAKEDISK.BAT` | Driver script: prompts for two blank floppies and runs each `.EXE` in turn |
| `MAKEDISK.PIF` | Windows shortcut so `MAKEDISK.BAT` can be launched from Explorer |

The two `.EXE` files are **not** ordinary self-extracting archives — they do not unpack
into files. Each one is a whole 1.44 MB floppy stored as a compressed **sector image**,
wrapped in a self-extracting stub built with *Disk eXPress Self-Extracting Diskette
Image (OS/2 and DOS) V2.34, © 1991–94 Albert J. Shan*. Running one writes the image
straight onto a blank formatted diskette; it cannot write to a hard disk. This is why
`MAKEDISK.BAT` insists on "2 formatted diskette(s)".

Each stub carries a plaintext description block, readable directly in the binary:

```
Audio Drivers (Yamaha OPL3SA2)
Dimension XPS Dxxx
Windows 95
vA05 - diskette 1 of 2
Dell Computer Corporation - www.dell.com
```

`DD58463` is Dell's own download ID from the era; the two payload files inherit the
number (`DE58463` + disk number). `MAKEDISK.PIF` is dated **14 Nov 1997**, and both
`.EXE` files and `MAKEDISK.BAT` **23 June 1998** — the PIF was reused from an earlier package.

## What is on the disks

Both are FAT12 1.44 MB floppies, volume labels `YAMAHA 1` and `YAMAHA 2`.

**Disk 1 — `YAMAHA 1`**

| File | Size | Role |
|---|---|---|
| `OPL3SA.DRV` | 179,584 | 16-bit Windows audio driver (NE) |
| `VOPL3SA.VXD` | 76,447 | Virtual device driver (LE) |
| `SAUNINST.EXE` | 67,584 | Uninstaller (Win32 PE) |
| `SASOUND.INF` | 7,847 | Main setup INF |
| `SACOM.INF`, `SAIDE.INF`, `SARESERV.INF` | 2,398 / 905 / 485 | Serial, IDE-CD and resource-reservation INFs |
| `VERSION.TXT` | 146 | Version stamp |
| `README\{ENGLISH,FRENCH,GERMAN,ITAILAN,JAPANESE,SPANISH}\README.TXT` | ~10–12 KB each | Release notes (`ITAILAN` is Dell's typo) |

**Disk 2 — `YAMAHA 2`**

| File | Size | Role |
|---|---|---|
| `VSGM.VXD` | 1,333,886 | Yamaha software-wavetable MIDI synthesiser — the reason a second disk exists |
| `VYMIXD.VXD` | 13,358 | Mixer driver |
| `SASOUND.INF`, `SACOM.INF`, `SAIDE.INF`, `SARESERV.INF`, `VERSION.TXT` | — | Byte-identical copies of the disk-1 INFs, so setup can read them from either disk |

`VERSION.TXT` on both disks reads:

```
   Title: Yamaha OPL3SA2 Windows 95 Drivers
 Version: A05
      OS: Windows 95
Language: International
```

The English README's revision history runs from **version 1.09 (27 Aug 1996)** to
**4.03.2324 (6 Jun 1997)** — so Dell's "A05" wraps Yamaha driver build 4.03.2324.
Supported hardware is listed as **OPL3-SA, OPL3-SA2, OPL3-SA3 and OPL4-ML**
(hardware wavetable).

The INF declares the Plug and Play IDs the drivers bind to: `YMH0002` (Sound Blaster Pro
compatible), `YMH0003` (Windows Sound System), `YMH0004`/`PNPB020` (FM synth),
`YMH0005`/`PNPB006` (MPU-401), `YMH0021` (OPL3-SA2 sound system) and
`YMH0022`/`PNPB02F` (joystick).

## "Files for game" — what the label probably meant

There is no game anywhere on these disks, and nothing on them names one. But the
label is not a mistake, and there are two readings, both supported by the contents.

**The game *port*.** The OPL3-SA2 is not only the sound chip — the joystick
connector hangs off it too, and this driver set is what makes it work. `SASOUND.INF`
installs a device it calls `"YAMAHA OPL3-SAx GamePort"` (PnP IDs `YMH0022` /
`PNPB02F`) along with a `"Gameport Joystick"` driver, `msjstick.drv`, registered
under `Drivers\joystick`. The README has dedicated steps for it — *"the gameport
will also be detected and installed"* — and tells you to look under **Sound, video
and game controllers** in Device Manager. `VOPL3SA.VXD` even carries the error
string *"Unable to access to Gameport - Gameport is in use by another
application."* If someone's joystick stopped working, this disk set is exactly
what they would have needed, and "files for game" is a reasonable shorthand for it.

**Sound for games generally.** The other reading is just as plausible: on a 1998
PC this chip *was* the game audio. OPL3 FM synthesis and MPU-401 MIDI were the two
things DOS games asked for by name, and section 11 of the README is titled *"Using
software wavetable in a virtual DOS box application"* — set MPU-401 OUT to Soft GM,
point the DOS game at MPU-401 on I/O 330, and the music improves over plain FM.
A rescue floppy made so that games would have sound after a Windows reinstall fits
the label just as well.

**Which game?** The disks cannot answer that. They are a stock Dell driver
package, byte-identical to what anyone with a Dimension XPS D-series could
download; nothing on them is specific to a title, and nothing was added by hand.
The only things that could narrow it further are outside the data — more writing
on the physical floppy label, or other disks that came out of the same box.

> **See also:** `DD58463_games_research.md` for the full research write-up —
> what the chip satisfied in each game sound menu, the handful of era titles
> actually documented against it, and what Dell's own manual says the machine
> contained.

### One correction from Dell's own documentation

The package is *named* for the OPL3-SA2, but Dell's *Dimension XPS D Series
Reference and Troubleshooting Manual* describes the integrated audio as the
"Yamaha **OPL3-SA3** 3D-enhanced audio controller with wave-table **software**".
Yamaha shipped one unified driver family covering OPL3-SA, SA2 and SA3 — the
same four files are documented elsewhere as the OPL3-SA3 driver — so the "SA2"
in the filename is packaging, not silicon.

Two further details from that manual and from period advertising: integrated
sound was a **build-time option** on these machines (configurations without it
shipped a Sound Blaster AWE64 or Turtle Beach Montego card instead), and Dell's
"Integrated Yamaha 32 Wavetable Sound" branding refers to the *software*
wavetable — `VSGM.VXD` on disk 2 — not to a wavetable chip.


## How this was extracted

The extractors only write to a physical floppy drive via DOS generic IOCTL
(`INT 21h/440Dh`, minor 41h "write track"), so no DOS machine, no extraction. No DOS
emulator was installable in this sandbox — the package registries and source hosts were
all blocked by egress policy — so I wrote a small 16-bit x86 interpreter with just
enough DOS and BIOS to run the stubs against a virtual 1.44 MB drive.

Both extractions ran to completion and the extractors' own integrity check passed:

| | Stored CRC-32 | Computed |
|---|---|---|
| Disk 1 | `2F182F0A` | `2F182F0A` |
| Disk 2 | `21A95CEA` | `21A95CEA` |

All extracted executables have intact headers (`NE`, `PE`, `LE`) and file lengths match
their directory entries, so the contents are complete and undamaged after 28 years.

## Files produced

- `DE584631_disk1_YAMAHA1.img`, `DE584632_disk2_YAMAHA2.img` — raw 1.44 MB floppy
  images, mountable or writable to real media
- `extracted/` — the file trees from both disks
