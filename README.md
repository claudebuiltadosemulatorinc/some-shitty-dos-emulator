# dosemu.c — a throwaway DOS machine for unpacking Disk eXPress images

> [!NOTE]
> This was all originally undocumented. I asked Claude to heavily comment the source for my own
> personal interest and as a learning experience.

`DE584631.EXE` and `DE584632.EXE` only know how to do one thing: write a floppy
image to a real diskette drive. There is no "extract to folder" mode. To get the
contents without a 1998 PC and two blank floppies, you need something that looks
enough like DOS *and* a floppy controller to satisfy them.

This is that something: a single-file 16-bit x86 interpreter with a small DOS
personality, pointed at a virtual 1.44 MB drive whose sectors are dumped to a
`.img` file instead of to magnetic media.

It is not a general-purpose emulator. It was written to run these two programs
and nothing else, and it is kept here because it is the tool that produced the
extracted disks.

## Building and running

```
gcc -O2 -fno-strict-aliasing -o dosemu dosemu.c

./dosemu DE584631.EXE disk1.img "/s a:"
./dosemu DE584632.EXE disk2.img "/s a:"

python3 fat12_extract.py disk1.img disk1/
```

`/s` suppresses the "about to overwrite" prompt; `a:` is the target drive. Add
`-v` as a fourth argument for a per-track trace of the disk writes.

The included `dosemu` binary is a **statically linked Linux x86-64 executable**,
not a Windows one — no Windows cross-compiler was available in the sandbox where
it was built. On Windows it will run under WSL, or just rebuild from source with
any C compiler.

## What is emulated

**CPU** — 8086/80186 in real mode, plus the handful of 286/386 instructions a
1994 Microsoft C compiler might emit (`PUSHA`/`POPA`, `ENTER`/`LEAVE`, immediate
`IMUL`, shift-by-immediate, `MOVZX`/`MOVSX`, `SETcc`, near `Jcc`). One megabyte
of flat memory, no protected mode, no FPU (`ESC` opcodes are decoded and
skipped).

**DOS** — enough of `INT 21h` to load and run an `.EXE`: file open/read/seek/
close, console output, the memory manager, get/set interrupt vector, date and
time, and the generic IOCTL calls. The interrupt vector table is real: every
vector points at a stub in the BIOS area whose first byte is an illegal opcode
the interpreter traps on, so `INT n`, far calls through a vector, and
`AH=35h`/`AH=25h` all behave the way a program expects.

**The floppy** — `INT 13h`, `INT 25h`/`INT 26h`, and generic IOCTL
`INT 21h/440Dh` minor 41h and 61h (write/read track on a logical drive). These
extractors use the last of those. The virtual drive reports itself as an
80-cylinder, 2-head, 18-sector 1.44 MB unit and starts out freshly FAT12
formatted, because the extractor refuses to write to an unformatted disk.

## The bug worth knowing about

The first working version produced disks that looked plausible — real driver
text, a valid boot sector, two matching FAT copies — but had no root directory
anywhere on them, and the same 55,266 bytes of content repeating forever.

The cause was not the CPU. It was `INT 21h AH=4Ah`, resize memory block. The
extractor allocates a ~9 KB buffer per track and then grows it in 8 KB steps up
to 64 KB. The first implementation let every resize succeed without actually
reserving the space, so the next allocation was handed a segment inside the
block that had just "grown" — every track's buffer overlapped its neighbour, and
the decompressor spent the run overwriting its own output.

Replacing it with a real first-fit allocator with block splitting, coalescing on
free, and honest failure (carry set, `BX` = largest block available) fixed it
outright. Both disks then extracted with the extractors' own CRC-32 checks
passing: `2F182F0A` for disk 1, `21A95CEA` for disk 2.

The lesson generalises: when an emulated program produces subtly wrong output,
suspect the operating system you wrote before you suspect the processor.

## Reading the source

`dosemu.c` is heavily commented for someone who programs but does not write C.
Alongside what each piece does, the comments explain the C idioms in play
(pointer casts, `static`, out-parameters, bit twiddling) and the x86 and DOS
concepts they implement — segment:offset addressing, the ModR/M byte, the flags
register, the interrupt vector table, the MZ header and the PSP. Start at the
top and read down; the header comment sets out the whole design in a page.

## Files

| File | |
|---|---|
| `dosemu.c` | The interpreter, ~1,730 lines including commentary, no dependencies |
| `dosemu` | Static Linux x86-64 build of the above |
| `fat12_extract.py` | Lists and unpacks a FAT12 image |
