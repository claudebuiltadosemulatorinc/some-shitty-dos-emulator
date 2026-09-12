/* ===========================================================================
 * dosemu.c — a 16-bit x86 interpreter with just enough DOS to run one program
 * ===========================================================================
 *
 * WHAT THIS IS
 *
 * DE584631.EXE and DE584632.EXE are 1998 Dell driver diskettes stored as
 * compressed *sector images*. Each one is a DOS program whose only trick is to
 * decompress itself onto a real floppy disk. There is no "extract to a folder"
 * mode. So to read them without a 1998 PC, you have to give them something
 * that looks enough like DOS and a floppy drive that they are willing to run.
 *
 * That is all this file does. It reads the .EXE, pretends to be an 8086 CPU
 * executing it, answers the DOS and BIOS calls the program makes, and when the
 * program says "write these 18 sectors to track 5 of drive A:", it copies them
 * into an array instead of onto magnetic media. At the end the array is dumped
 * to a .img file.
 *
 * HOW AN EMULATOR LIKE THIS WORKS
 *
 * A CPU is a loop. Read the byte at the current instruction pointer, decide
 * what instruction it is, do it, advance the pointer, repeat. That is literally
 * the whole design: see step() near the bottom, and the while() loop in main()
 * that calls it a few hundred million times. Everything else in this file is
 * detail about what the individual instructions do and what happens when the
 * program asks the "operating system" for something.
 *
 * THE SAME IDEA, STATED AS A STATE MACHINE
 *
 * If "pretending to be a CPU" sounds vague, here is a more precise framing.
 * The complete state of the emulated machine is a small, fully enumerable set
 * of variables: one megabyte of memory, eight general-purpose registers, six
 * segment registers, an instruction pointer, nine one-bit flags. That is all of
 * it. Nothing the emulated program can observe lives anywhere else.
 *
 * One call to step() is one state transition. It reads the current state and
 * writes a new state, and nothing outside that state influences the outcome:
 * there is no clock in this emulator, no real device that can interrupt, no
 * thread scheduling, no random number source, no dependence on the host's
 * speed or memory layout. In the vocabulary of a modeller, step() is a pure
 * function from state to state, and the whole program is the iterated map
 *
 *      state_{n+1} = step(state_n)
 *
 * run until the state says "halted".
 *
 * The practical payoff is determinism, and it is the reason this approach is
 * tractable at all. The same input .EXE produces the same 1,474,560-byte image,
 * byte for byte, on every run and on any host machine. A bug does not appear
 * intermittently; it appears at the same instruction number every time, and you
 * can stop the machine at instruction 39,214,776 and inspect every bit of the
 * state. Debugging the original program on original hardware would have meant
 * reproducing a fault in real time on a real disk drive. Here the entire run is
 * a reproducible computation you can re-evaluate as often as you like, which is
 * how the one serious bug in this file (see THE MEMORY MANAGER) was found.
 *
 * NOTES FOR SOMEONE WHO PROGRAMS BUT NOT IN C
 *
 *  - `static` at file scope does NOT mean what it means in Java or C#. It means
 *    "private to this file". A `static` variable inside a function is different
 *    again: it keeps its value between calls, like a closure variable.
 *
 *  - WHERE VARIABLES LIVE. C gives a variable one of three lifetimes, and the
 *    distinction matters below. (1) Static/global storage: `static uint8_t
 *    mem[0x100000]` exists for the whole run, at a fixed place, and is zeroed
 *    before main() starts; it costs nothing to "create". (2) Automatic storage,
 *    i.e. local variables: these live on the *stack*, a region that grows and
 *    shrinks as functions call and return, so a local disappears the moment its
 *    function returns. (3) Dynamic storage, i.e. the *heap*: memory you request
 *    at run time with malloc() and hand back with free(), which lives until you
 *    free it. The big arrays here are deliberately static rather than local,
 *    because a one-megabyte local variable would overrun a typical stack. The
 *    stack is explained in detail at THE STACK, the heap at THE MEMORY MANAGER
 *    — and one nice property of this file is that it contains both a *use* of
 *    the host's allocator and a complete *implementation* of one for the
 *    emulated machine, so you can see the mechanism from both sides.
 *
 *  - `uint8_t`, `uint16_t`, `uint32_t` are unsigned integers of exactly 8, 16
 *    and 32 bits. `int8_t`/`int16_t` are the signed versions. C's plain `int`
 *    has no guaranteed width — it is "whatever is natural on this machine" —
 *    so anything modelling hardware uses the explicit widths everywhere.
 *
 *    The width is not a performance tweak here; it is part of the specification
 *    being implemented. A 16-bit register on this CPU holds values 0..65535 and
 *    that is a fact about the machine, in the same way that a column typed as
 *    int16 in a dataframe is a fact about the column. If you modelled AX as a
 *    64-bit integer, arithmetic that is supposed to wrap around would instead
 *    keep counting, and the emulated program would take different branches.
 *
 *  - UNSIGNED OVERFLOW WRAPS, DELIBERATELY. In C, arithmetic on an unsigned
 *    type is defined to be modular: for a uint16_t, every result is taken
 *    modulo 65536. So 65535 + 1 == 0, and 3 - 5 == 65534. This is not an error
 *    condition and there is no exception; it is the specified behaviour, and it
 *    is exactly what the real hardware does, because a 16-bit adder physically
 *    has nowhere to put a 17th bit. Code below relies on this in several
 *    places — address computations that wrap inside a 64 KB segment, a
 *    "subtract" implemented as adding a negative number, the borrow detection
 *    in op_sub(). Signed overflow is a different story: in C it is undefined
 *    behaviour, which is why signed interpretations here are always produced by
 *    an explicit cast at the point of use rather than by storing signed types.
 *
 *  - Arrays are raw memory with no bounds checking. `uint8_t mem[0x100000]` is
 *    one megabyte of bytes; reading mem[2000000] is not an error, it is a bug
 *    that silently reads someone else's data. Hence the masking you will see.
 *
 *  - POINTERS, in one paragraph, with the details where they first matter. A
 *    pointer is an integer that happens to be a memory address. `&x` produces
 *    the address of x; `*p` reads (or writes) whatever is at address p; the
 *    *type* of the pointer says how many bytes to touch and how to interpret
 *    them. An array name used as a value is the address of its first element,
 *    which is why `mem` and `mem+37` are both perfectly good pointers. See the
 *    REGISTER NAME SHORTHAND section for the machine-level picture.
 *
 *  - `#define NAME value` is a text substitution done before compilation, not a
 *    variable. It has no type and costs nothing at runtime.
 *
 *  - BIT OPERATIONS ARE FIELD EXTRACTION. They are load-bearing here rather
 *    than decorative:
 *        x & 0xFF     keep the low 8 bits (like x % 256)
 *        x >> 4       shift right 4 bits (like x / 16)
 *        x << 4       shift left 4 bits  (like x * 16)
 *        x | y        bitwise OR, used to glue pieces together
 *        x ^ y        bitwise XOR
 *        ~x           flip every bit
 *
 *    The useful way to read them is not "bit twiddling" but "unpacking several
 *    small fields that were packed into one integer". An 8086 instruction byte
 *    is a record with three columns in it; a directory entry on a FAT disk
 *    packs a date into 16 bits as year/month/day. `(v >> shift) & mask` is the
 *    SELECT for one of those columns. This idiom appears on nearly every page
 *    below, and once you read it that way the code stops looking cryptic.
 *
 *  - `switch` cases fall through to the next case unless you `break` or
 *    `return`. Several places below deliberately let two opcodes share a body.
 *
 *  - There is no exception handling. Errors are reported by return value, or by
 *    printing and calling exit().
 *
 * WHAT IS *NOT* HERE
 *
 * No protected mode, no floating point, no video hardware, no sound, no
 * timers, no multitasking, no real filesystem. This is not a general-purpose
 * emulator and it would not boot DOS. It implements exactly the subset that
 * these two programs turned out to touch, which is a much smaller thing.
 */

#include <stdio.h>      /* printf, fopen, fread, ... */
#include <stdlib.h>     /* malloc, exit, getenv */
#include <string.h>     /* memcpy, memset, strlen */
#include <stdint.h>     /* the uint8_t / uint16_t / uint32_t types */

/* ---------------------------------------------------------------------------
 * THE MACHINE'S STATE
 *
 * Everything the emulated CPU can see is in these few variables. If you saved
 * all of them to disk you would have a complete snapshot of the machine.
 * ------------------------------------------------------------------------ */

/* Physical memory: one flat megabyte. That is the entire address space a real
 * 8086 could reach (20 address lines, 2^20 = 1048576 = 0x100000 bytes).
 *
 * WHAT "MEMORY" ACTUALLY IS, since everything below depends on it. Main memory
 * is a single enormous array of bytes, numbered from 0. An *address* is nothing
 * more exotic than an index into that array — an ordinary non-negative integer.
 * There are no types in memory, no objects, no names: the byte at address 74219
 * is just a number 0..255, and whether it means part of a machine instruction,
 * a character of text, or half of a disk sector depends entirely on what reads
 * it. On the real chip, an address travels out of the processor on 20 physical
 * wires (the address bus) as a 20-bit number, and the memory chips return the
 * byte stored there.
 *
 * So emulating memory needs no cleverness at all: an array of bytes on the host
 * is an exact model of an array of bytes on the guest, and the emulated
 * machine's address is literally the index we use. That equivalence — address
 * is index, memory is array — is worth internalising early, because it is what
 * makes pointers, segmentation, disk buffers and the memory allocator below all
 * the same kind of thing: arithmetic on integers that name array positions.
 *
 * `static` here also means this array is in the host program's static storage:
 * one megabyte reserved at program startup, not on the stack (a 1 MB local
 * variable would blow a typical 8 MB stack in a few nested calls) and not on
 * the heap (no allocation call needed, and the C runtime zero-fills it for us,
 * which conveniently matches a machine powering on with cleared memory). */
static uint8_t mem[0x100000];

/* WHAT A REGISTER IS
 *
 * A register is a storage slot physically inside the processor: a small bank of
 * flip-flops wired directly into the arithmetic circuitry. It is not memory
 * with a fast cache in front of it — it is a different kind of thing in a
 * different physical place, and it is addressed differently. Memory is
 * addressed by number, and there are a million of those. Registers are named
 * inside the instruction itself, by a 3-bit field, and on this CPU there are
 * eight of them, 16 bits each: 128 bits of general-purpose state for the entire
 * machine. Everything else the program is working with is out in memory.
 *
 * REGISTERS VS MEMORY IS *THE* PERFORMANCE DISTINCTION IN CODE OF THIS ERA.
 * An 8086 has no cache. An operand that lives in memory has to be fetched over
 * the external bus, and the address for it has to be computed first; an operand
 * that lives in a register is already inside the ALU. The same logical
 * operation therefore costs several times more when one side of it is in
 * memory, and the multiplier is large enough that the arrangement of a hot loop
 * around the eight available registers was the main thing that decided whether
 * code was fast or slow. This is why assembly of this period reads the way it
 * does: values are hauled into registers, worked on there for as long as
 * possible, and written back once. It is also why compiler writers care so much
 * about "register allocation" — choosing which of a function's many values get
 * to occupy the few real slots at each moment, which is a graph-colouring
 * problem, and why a 1998 decompressor inner loop like the one this emulator
 * spends its time on touches memory as rarely as it can manage.
 *
 * The registers are listed here in the order the x86 instruction encoding
 * numbers them — that ordering is not arbitrary, it lets us turn a 3-bit field
 * straight out of an instruction into an index into this array.
 *
 *   index 0 1 2 3 4 5 6 7
 *   name  AX CX DX BX SP BP SI DI
 *
 * "General-purpose" is an overstatement on this architecture: each register has
 * conventional and sometimes mandatory roles baked into the instruction set.
 * AX is the accumulator (multiply and divide use it implicitly), CX is the
 * count register (the loop and string-repeat instructions decrement it), DX
 * holds the high half of a 32-bit product or dividend, BX can serve as a memory
 * base address, SI and DI are the source and destination pointers for the block
 * instructions, and:
 *
 *   SP  the STACK POINTER — the address of the top of the stack
 *   BP  the BASE POINTER, conventionally the FRAME POINTER
 *
 * Those last two need the stack explained, which is done in full at THE STACK
 * further down. The short version, because BP shows up in the address decoder
 * before then: the stack is a region of memory used as a last-in-first-out
 * scratch area. Every function call pushes its return address there, and a
 * function's local variables usually live there too. SP always points at the
 * current top. BP is set once on entry to a function to point at a fixed spot
 * in that function's own patch of stack (its "frame"), so that arguments and
 * locals can be reached at constant offsets from BP even while SP keeps moving
 * as the function pushes and pops. Hence "frame pointer".
 *
 * They are declared 32 bits wide even though this is a 16-bit CPU, so that the
 * handful of 386-era instructions that use the wide forms have somewhere to
 * put their results. The 16-bit code only ever looks at the low half. */
static uint32_t R[8];

/* Segment registers. See the note on segmentation above lin() below. */
static uint16_t S[6];            /* ES CS SS DS FS GS */

/* An `enum` is just a set of named integer constants: rES is 0, rCS is 1, and
 * so on. Writing S[rCS] instead of S[1] is purely for readability. */
enum { rES=0, rCS, rSS, rDS, rFS, rGS };

/* The instruction pointer — the offset within the code segment of the next
 * instruction to execute. Named IPr rather than IP only to avoid colliding
 * with anything else called IP.
 *
 * This one variable is the machine's entire notion of "where I am in the
 * program". There is no call stack maintained by the hardware, no frame
 * objects, no program counter stack: a jump is an assignment to IPr, a function
 * call is an assignment to IPr after saving the old value into memory, and a
 * return is reading that saved value back. Control flow, at this level, is
 * mutation of a single integer. */
static uint32_t IPr;

/* The flags. A real CPU packs these as bits inside one 16-bit FLAGS register;
 * keeping them as separate ints is slower but far easier to get right, and
 * getflags()/setflags() below convert to and from the packed form when the
 * program pushes or pops them.
 *
 *   CF  carry      — an add overflowed out of the top bit, or a subtract borrowed
 *   PF  parity     — low 8 bits of the result have an even number of 1s
 *   AF  auxiliary  — carry out of bit 3; only used by the BCD instructions
 *   ZF  zero       — the result was zero
 *   SF  sign       — the top bit of the result was set (i.e. negative)
 *   TFf trap       — single-step mode (present but unused here)
 *   IFf interrupt  — interrupts enabled (present but unused here)
 *   DFf direction  — string instructions count downwards instead of upwards
 *   OF  overflow   — signed arithmetic overflowed
 *
 * The three with an `f` suffix (TFf, IFf, DFf) are spelled that way because
 * TF/IF/DF would clash with other common names. */
static int CF,PF,AF,ZF,SF,TFf,IFf,DFf,OF;

static int halted = 0, exitcode = -1;   /* set when the program terminates */
static long long icount = 0;            /* instructions executed, for reporting */
static int verbose = 0;                 /* -v on the command line */

/* ---------------------------------------------------------------------------
 * THE VIRTUAL FLOPPY DRIVE
 *
 * WHAT A BLOCK DEVICE IS. Underneath every filesystem there is a much simpler
 * abstraction: a *block device* is an addressable array of fixed-size blocks.
 * That is the whole interface. You can read block number n, or write block
 * number n, and the block is always the same size — 512 bytes here, as it was
 * for essentially all PC-era disks. The device has no idea what a file is, no
 * idea what a directory is, and no idea which blocks are in use. It is, in
 * data terms, an array of 2880 rows of 512 bytes, and nothing more.
 *
 * Everything else is a data structure written *into* that array by software. A
 * filesystem (FAT12, in this case) is exactly that: a layout convention that
 * says "block 0 describes the disk, blocks 1-9 hold an allocation table, blocks
 * 19-32 hold directory entries, the rest is file contents". See format_floppy()
 * near the bottom of the file for the full layout, and for why the allocation
 * table is a genuinely nice piece of data-structure design.
 *
 * A DEVICE DRIVER is the code that sits between the two: it turns a general
 * request ("write these 512 bytes as block 91") into whatever specific commands
 * the actual hardware needs (position the head over cylinder 2, select head 1,
 * wait for sector 19 to come round, clock the bytes out). Because that
 * translation is the only thing the driver does, replacing a real disk with a
 * simulated one is easy: implement the same request interface over an array.
 * That is precisely what this emulator does, in int13() and in the IOCTL
 * handler inside int21(), and it is why the output is a *sector image* — a
 * byte-for-byte copy of the block array — rather than a folder of files.
 *
 * A 1.44 MB floppy is 80 cylinders x 2 heads x 18 sectors x 512 bytes. A
 * "cylinder" is a ring position of the head arm, and there are two heads
 * because the disk has two sides, so 80*2 = 160 tracks of 18 sectors each.
 *
 * The program addresses sectors as (cylinder, head, sector), the way real
 * hardware did — three numbers describing a physical position. We flatten that
 * to a single index — an LBA, logical block address, the row number in the
 * array — and store the whole disk as one big array. floppy_lba() below is the
 * flattening function, and it is the same index arithmetic you would use to
 * address a 3-dimensional array stored contiguously.
 * ------------------------------------------------------------------------ */
#define FLOP_CYL 80
#define FLOP_HD  2
#define FLOP_SPT 18                                  /* sectors per track */
#define FLOP_SIZE (FLOP_CYL*FLOP_HD*FLOP_SPT*512)    /* = 1,474,560 bytes */
static uint8_t floppy[FLOP_SIZE];

/* One flag per sector, so we can report how much of the disk was actually
 * touched. Purely diagnostic; the emulation does not depend on it. */
static uint8_t written[FLOP_CYL*FLOP_HD*FLOP_SPT];

/* ---------------------------------------------------------------------------
 * HOST FILE ACCESS
 *
 * The extractor opens its own .EXE and reads the compressed image out of the
 * back of it. So we need real file I/O — but only for that one file, which
 * keeps things simple: every open() the program performs is answered with a
 * fresh handle onto the same real file on the host disk.
 *
 * WHAT A FILE HANDLE IS. When a program opens a file, the OS does the real work
 * — locating it, checking permissions, setting up buffers, remembering how far
 * through it you have read — and hands back a small integer. That integer is a
 * *handle*: an index into a table the OS keeps on the program's behalf. The
 * program never sees the underlying object and cannot corrupt it; it can only
 * name it. Every subsequent read, write, seek and close passes the index back.
 *
 * This is the same indirection as any opaque identifier in a higher-level API,
 * and it buys the same things: the OS can change its internal representation
 * freely, it can validate every use (an out-of-range handle is rejected rather
 * than dereferenced), and the numbering is per-program, so your handle 5 and
 * another program's handle 5 are unrelated. The arrays below are precisely such
 * a table — handles[] maps the emulated program's handle number to a real host
 * FILE*, and the emulated program can no more see a FILE* than a DOS program
 * could see DOS's internals. Unix file descriptors are the same idea with the
 * same convention for the first few slots.
 * ------------------------------------------------------------------------ */
static const char *selfpath = NULL;  /* path to the .EXE we are running */
static FILE *handles[64];            /* DOS file handle -> host FILE* */
static long  hpos[64];               /* bookkeeping: current offset per handle */

/* ---------------------------------------------------------------------------
 * REGISTER NAME SHORTHAND
 *
 * These let the rest of the file say `AX` where it means "the low 16 bits of
 * R[0]", including on the left-hand side of an assignment (`AX = 5;`).
 *
 * POINTERS, AT THE MACHINE LEVEL. This is the right place to make the earlier
 * one-paragraph summary concrete, because these eight lines are pointers in
 * their purest form.
 *
 * Memory is an array of bytes indexed by an integer (see the note on mem[]
 * above). A pointer is that integer. When C says `uint16_t *p`, the value of p
 * at run time is an address — on a 64-bit host, a 64-bit unsigned number that
 * indexes the process's address space. Nothing else is stored with it: no
 * length, no bounds, no ownership, no null-checking. It is a row number.
 *
 * Three operations, and that is the whole of it:
 *
 *   &x    "address of" — asks where x lives, producing the integer that names
 *         its first byte.
 *   *p    "dereference" — go to that address and read (or, on the left of an
 *         assignment, write) the value there. How many bytes it touches, and
 *         how it interprets them, comes from the pointer's *type*: reading
 *         through a uint16_t* fetches two bytes and assembles them into a
 *         number; reading through a uint8_t* fetches one.
 *   p+n   pointer arithmetic — scaled by the size of the pointed-at type. If p
 *         is a uint32_t*, p+1 is four bytes further along, not one. The
 *         compiler is doing `address + n * sizeof(element)` for you, which is
 *         the same index-to-offset multiplication an array subscript does.
 *         Indeed `p[n]` is *defined* as `*(p + n)`; arrays and pointers are two
 *         spellings of one mechanism.
 *
 * A pointer being merely an address is why the memory allocator further down
 * cannot move a block once it has handed it out (see THE MEMORY MANAGER): the
 * program holds bare integers into that block, the allocator has no record of
 * where they are, and there is nothing to update. Languages with a compacting
 * garbage collector *can* move objects precisely because their runtime knows
 * where every reference is.
 *
 * The mechanism of the macros below: &R[0] is the address of that array slot;
 * (uint16_t*) tells the compiler to treat that address as pointing at a 16-bit
 * value instead of a 32-bit one; the leading * dereferences it. On a
 * little-endian machine (see the endianness note below rd16) the low 16 bits of
 * a 32-bit value live at the *same starting address* as the 32-bit value, so
 * reading two bytes from there gives exactly the low half of the register. The
 * result is an alias: `AX = 5` writes the bottom half of R[0] and leaves the
 * top half alone, which is precisely how a real AX relates to a real EAX.
 *
 * This is a type pun — deliberately viewing the same bytes through two
 * different types. It is common in emulators, it is not strictly portable (the
 * C standard's "strict aliasing" rules say the compiler may assume two pointers
 * of different types never refer to the same storage, and optimise on that
 * assumption), and it is why the build line uses -fno-strict-aliasing, which
 * switches that assumption off. A stricter-but-uglier alternative would be a
 * getter and setter function for every access.
 * ------------------------------------------------------------------------ */
#define AX (*(uint16_t*)&R[0])
#define CX (*(uint16_t*)&R[1])
#define DX (*(uint16_t*)&R[2])
#define BX (*(uint16_t*)&R[3])
#define SP (*(uint16_t*)&R[4])
#define BP (*(uint16_t*)&R[5])
#define SI (*(uint16_t*)&R[6])
#define DI (*(uint16_t*)&R[7])

/* ---------------------------------------------------------------------------
 * MEMORY ACCESS, AND THE SEGMENT:OFFSET SCHEME
 *
 * The 8086 has 16-bit registers but a 20-bit address bus, so no single register
 * can name a byte of memory. The fix was segmentation: every address is a pair,
 * a 16-bit *segment* and a 16-bit *offset*, combined as
 *
 *      physical address = segment * 16 + offset
 *
 * Written "1234:5678", meaning 0x12340 + 0x5678 = 0x179B8. A segment therefore
 * starts every 16 bytes (a "paragraph"), and a single segment can address 64 KB
 * before the offset wraps. Note that the mapping is many-to-one: 0000:0010 and
 * 0001:0000 are the same byte. This is why DOS-era code sizes are quoted in
 * paragraphs, and why 64 KB shows up everywhere as a natural limit.
 *
 * `static inline` asks the compiler to paste the function body in at each call
 * site rather than making a real call. These run tens of millions of times.
 * ------------------------------------------------------------------------ */
static inline uint32_t lin(uint16_t seg, uint16_t off){ return ((uint32_t)seg<<4)+off; }

/* rd8/wr8 are the only two functions that touch `mem` directly. The `& 0xFFFFF`
 * wraps any address back inside one megabyte, which both prevents a stray
 * access from corrupting the emulator's own memory and mimics the real chip:
 * with only 20 address lines, address 0x100001 genuinely did wrap to 0x000001. */
static inline uint8_t  rd8(uint32_t a){ return mem[a & 0xFFFFF]; }
static inline void     wr8(uint32_t a, uint8_t v){ mem[a & 0xFFFFF]=v; }

/* ENDIANNESS, and why multi-byte values are assembled a byte at a time.
 *
 * Memory holds bytes. A 16-bit number does not fit in a byte, so it occupies
 * two adjacent addresses — and something has to decide which end goes first.
 * x86 is LITTLE-ENDIAN: the least significant byte is stored at the lower
 * address. The value 0x1234 written to address a lands as
 *
 *      address a     : 0x34      (the "little" end, worth 1s)
 *      address a + 1 : 0x12      (worth 256s)
 *
 * Big-endian machines (older Motorola and SPARC parts, IBM mainframes, and the
 * byte order used in most internet protocol headers, which is why it is often
 * called network byte order) do the opposite. Neither is better; they are
 * conventions, and the only real cost is that data crossing between them must
 * be swapped. Little-endian has one small elegance visible in this file: the
 * low half of a wider value starts at the same address as the whole value,
 * which is what makes the AX-aliases-EAX trick in the macros above work.
 *
 * Endianness is not only a memory question — it is a *file format* question,
 * and everything this emulator parses is little-endian on disk too: the MZ
 * executable header in main(), the BPB in the boot sector, the FAT entries.
 * That is why you keep seeing the shape `lo | (hi << 8)`.
 *
 * These four helpers build 16- and 32-bit accesses out of byte accesses, low
 * byte first. The obvious alternative — casting a pointer into the mem[] array
 * and dereferencing it — would be faster, but it would silently bake in the
 * *host's* byte order (so the emulator would produce different results on a
 * big-endian machine), it can fault or run slowly on CPUs that require aligned
 * accesses, and it would mishandle an access that straddles the 1 MB wrap.
 * Composing from rd8/wr8 gives correct, host-independent behaviour in every
 * case, and the compiler generates decent code for it anyway. */
static inline uint16_t rd16(uint32_t a){ return rd8(a) | (rd8(a+1)<<8); }
static inline void     wr16(uint32_t a, uint16_t v){ wr8(a,v&0xFF); wr8(a+1,v>>8); }
static inline uint32_t rd32(uint32_t a){ return rd16(a) | ((uint32_t)rd16(a+2)<<16); }
static inline void     wr32(uint32_t a, uint32_t v){ wr16(a,v&0xFFFF); wr16(a+2,v>>16); }

/* ---------------------------------------------------------------------------
 * REGISTER ACCESS BY NUMBER
 *
 * BIT FIELDS, READ AND WRITTEN. The two functions below are the first worked
 * example of the packed-field idiom that runs through the whole file, so it is
 * worth spelling out the general pattern once:
 *
 *      read  a field:   (word >> shift) & mask
 *      write a field:   word = (word & ~(mask << shift)) | (value << shift)
 *
 * Reading shifts the field down to the bottom and then discards everything
 * above it. Writing punches a hole of zeros where the field goes (AND with the
 * inverted mask) and ORs the new bits into the hole. If it helps to have a
 * familiar picture: an integer used this way is a fixed-width record with
 * several narrow columns packed side by side, exactly as a columnar file format
 * packs several small values into one machine word to save space. Reading a
 * field is projecting one column out; writing one is an update that must leave
 * the neighbouring columns untouched. That "must leave the neighbours alone"
 * requirement is why writes are read-modify-write rather than plain stores.
 *
 * Instructions identify registers with a 3-bit field, so the decoder needs to
 * go from a number to a register. For 16-bit operands the numbering matches the
 * R[] array directly. For 8-bit operands it does not:
 *
 *      0 1 2 3 4 5 6 7
 *      AL CL DL BL AH CH DH BH
 *
 * The first four are the *low* bytes of AX/CX/DX/BX and the second four are the
 * *high* bytes of the same four registers. So register 5 (CH) is bits 8..15 of
 * R[1]. That is what the `i<4` test and the `i-4` are doing. AH and AL really
 * are two halves of one register on this architecture — writing AL changes the
 * bottom of AX — which is why these are read-modify-write operations rather
 * than plain assignments.
 *
 * The overlap is not an accident of the encoding; it is used constantly. A DOS
 * call puts a function number in AH and a parameter in AL and thereby passes
 * two arguments in one register, and int21() below reads them back out
 * separately. Half of the "packed fields" theme on this machine is just this:
 * scarce storage, so everything shares.
 * ------------------------------------------------------------------------ */
static uint8_t getreg8(int i){ return (i<4)? (R[i]&0xFF) : ((R[i-4]>>8)&0xFF); }
static void setreg8(int i, uint8_t v){
    /* Clear the byte we are replacing (the & with an inverted mask), then OR
     * the new value into the hole. */
    if(i<4) R[i]=(R[i]&0xFFFFFF00u)|v; else R[i-4]=(R[i-4]&0xFFFF00FFu)|((uint32_t)v<<8);
}

/* Width-generic accessors. Throughout this file, `w` is an operand size in
 * BYTES: 1, 2 or 4. Passing the width around like this means one piece of code
 * can implement, say, ADD for all three sizes instead of three near-copies.
 *
 * `cond ? a : b` is C's if-expression, and they chain: the below reads as
 * "if w is 1 do this, else if w is 2 do that, else the third thing". */
static uint32_t getreg(int i,int w){ return w==1?getreg8(i): w==2?(R[i]&0xFFFF):R[i]; }
static void setreg(int i,int w,uint32_t v){
    if(w==1) setreg8(i,v); else if(w==2) R[i]=(R[i]&0xFFFF0000u)|(v&0xFFFF); else R[i]=v;
}

/* ---------------------------------------------------------------------------
 * PACKING AND UNPACKING THE FLAGS REGISTER
 *
 * We keep flags as separate variables, but the program can push the whole
 * FLAGS register onto the stack (PUSHF) or pop it back (POPF), and an interrupt
 * pushes it automatically. These two convert between the two representations.
 *
 * This is the packed-fields idiom again, now with nine one-bit columns in a
 * single 16-bit record — a bit vector. getflags() is the pack (shift each flag
 * into its column and OR them together) and setflags() the unpack (shift down,
 * mask to one bit). The emulator keeps the unpacked form because branching on
 * `if(ZF)` is clearer and faster than testing a bit, and pays the conversion
 * cost only at the boundary where the emulated program can observe the packed
 * layout. Choosing an internal representation that is convenient and converting
 * at the edges is the same trade you make when you decode a compact wire format
 * into working columns and re-encode on the way out.
 *
 * The bit positions are fixed by the hardware: carry is bit 0, parity bit 2,
 * auxiliary bit 4, zero bit 6, sign bit 7, trap bit 8, interrupt bit 9,
 * direction bit 10, overflow bit 11. Bit 1 is unused but always reads as 1 on
 * real silicon, hence the 0x0002 seed.
 *
 * `CF?1:0` normalises: our CF might hold any non-zero value, and we want
 * exactly 1 in the packed form.
 * ------------------------------------------------------------------------ */
static uint16_t getflags(void){
    return 0x0002 | (CF?1:0) | (PF?4:0) | (AF?0x10:0) | (ZF?0x40:0) | (SF?0x80:0)
         | (TFf?0x100:0) | (IFf?0x200:0) | (DFf?0x400:0) | (OF?0x800:0);
}
static void setflags(uint16_t f){
    CF=f&1; PF=(f>>2)&1; AF=(f>>4)&1; ZF=(f>>6)&1; SF=(f>>7)&1;
    TFf=(f>>8)&1; IFf=(f>>9)&1; DFf=(f>>10)&1; OF=(f>>11)&1;
}

/* ---------------------------------------------------------------------------
 * FLAG CALCULATION HELPERS
 * ------------------------------------------------------------------------ */

/* Parity: 1 if the low byte contains an even number of set bits. The folding
 * trick XORs the value against shifted copies of itself so that bit 0 ends up
 * holding the XOR of all eight original bits; that is the odd-parity answer, so
 * we invert it. (The x86 parity flag only ever looks at the low 8 bits, even
 * for 16- and 32-bit operations — a quirk inherited from the 8080.) */
static int parity(uint8_t v){ v^=v>>4; v^=v>>2; v^=v>>1; return (~v)&1; }

/* Value masks and top-bit positions for a given operand width. */
static uint32_t msk(int w){ return w==1?0xFF: w==2?0xFFFF:0xFFFFFFFFu; }
static int msb(int w){ return w*8-1; }        /* index of the most significant bit */

/* The flag update shared by the bitwise operations (AND, OR, XOR, TEST). They
 * all clear carry and overflow and then set zero/sign/parity from the result. */
static void setlogic(uint32_t r,int w){
    r&=msk(w); CF=0;OF=0;AF=0; ZF=(r==0); SF=(r>>msb(w))&1; PF=parity(r&0xFF);
}

/* ---------------------------------------------------------------------------
 * INSTRUCTION DECODING
 *
 * WHAT A PROGRAM IS, PHYSICALLY. Machine code is not a list of objects; it is a
 * byte string. The bytes 8B 46 FC sitting in memory *are* the instruction "load
 * AX from the local variable at BP-4". There is no separator between one
 * instruction and the next, no length prefix, no index. The only way to know
 * where the next instruction starts is to decode this one and see how long it
 * turned out to be. That is what fetch8() and decode_modrm() below are for, and
 * it is why "start executing at address X" is meaningful only if X really is an
 * instruction boundary: start one byte late and you decode a completely
 * different, usually nonsensical, program.
 *
 * WHY VARIABLE LENGTH. An x86 instruction is a variable-length byte sequence,
 * roughly:
 *
 *      [prefixes] opcode [ModR/M] [SIB] [displacement] [immediate]
 *
 * Everything after the opcode is optional and its presence depends on which
 * opcode it was. Instructions on this architecture run from 1 to about 6 bytes
 * in the 16-bit forms used here. Two forces produced that design. First,
 * density: when a whole machine had tens or hundreds of kilobytes of RAM, an
 * encoding that spends one byte on the common `PUSH AX` and five on a rare
 * `MOV [1234], 5678` fits noticeably more program into memory than a scheme
 * that charges a flat rate. Second, accretion: the instruction set has been
 * extended repeatedly since 1978, and each extension had to fit into whatever
 * encodings were still free, which is why (for example) the 386 additions all
 * hide behind a 0x0F escape byte at the bottom of step().
 *
 * The contrast is a fixed-width RISC encoding, where every instruction is
 * exactly four bytes and the fields sit at fixed bit positions. Decoding one is
 * a handful of shifts and masks with no sequencing at all, and a processor can
 * decode several at once because it knows where they all start without looking.
 * Variable-length decoding is inherently serial at the front end, which is a
 * real cost — and also the reason the x86 decoder is the most intricate part of
 * this file.
 *
 * WHAT THE DECODER IS REALLY DOING is parsing. It reads a byte stream and pulls
 * out typed fields: which operation, which registers, what constant, what
 * address. If you have ever written a parser for a binary format with
 * variable-length records — a tag byte that tells you which optional fields
 * follow, then those fields — this is the same job, and the code has the same
 * shape: read the tag, switch on it, consume exactly the bytes that tag implies.
 *
 * These three variables hold the state of the instruction currently being
 * decoded. They are reset at the top of step() for every instruction.
 * ------------------------------------------------------------------------ */

/* Segment override prefix. Normally a memory operand uses a default segment
 * (DS for most things, SS for anything based on BP or SP, because that is where
 * the stack lives). A prefix byte overrides that for one instruction. -1 means
 * "no override, use the default". */
static int segovr;

/* Operand and address size for this instruction, in bytes. On a 16-bit CPU both
 * are 2; the 0x66 and 0x67 prefix bytes flip them to 4. These programs are
 * 16-bit, so adsz is essentially always 2 and the 32-bit addressing path below
 * is there for completeness rather than because it gets used. */
static int opsz, adsz;

/* Repeat prefix for the string instructions: 0 none, 1 = REPNE, 2 = REP/REPE. */
static int repf;

/* Read the next instruction byte and advance IP. The `& 0xFFFF` is the 16-bit
 * wrap: IP is a 16-bit register, so incrementing past 0xFFFF returns to 0
 * *within the same code segment* rather than crossing into the next one.
 *
 * Note this is the only thing that moves IP forward during decoding, which is
 * how the emulator naturally handles variable-length instructions: each part of
 * the decoder consumes exactly the bytes it needs. */
static uint32_t fetch8(void){ uint8_t v=rd8(lin(S[rCS],IPr)); IPr=(IPr+1)&0xFFFF; return v; }
static uint32_t fetch16(void){ uint32_t v=fetch8(); v|=fetch8()<<8; return v; }
static uint32_t fetch32(void){ uint32_t v=fetch16(); v|=fetch16()<<16; return v; }
static uint32_t fetchw(int w){ return w==1?fetch8(): w==2?fetch16():fetch32(); }

/* Apply the segment override if one was present, otherwise use the default. */
static int seg_for(int def){ return segovr>=0? segovr : def; }

/* ---------------------------------------------------------------------------
 * THE ModR/M BYTE
 *
 * Most instructions that take two operands encode them in one byte after the
 * opcode, split into three fields:
 *
 *      bits 7-6   mod   how the r/m operand is addressed
 *      bits 5-3   reg   a register number (or, for some opcodes, an extension
 *                       of the opcode itself — "which of these 8 instructions")
 *      bits 2-0   r/m   the other operand: a register, or a memory address
 *
 * That is the field-extraction idiom once more, and here it is doing real work:
 * one byte carries three columns, and `m>>6`, `(m>>3)&7`, `m&7` project them
 * out. Packing operands this way is what keeps a two-operand instruction down
 * to two bytes.
 *
 * mod == 3 means "r/m is a register". Otherwise r/m names a way of computing a
 * memory address, optionally plus a displacement:
 *
 *      mod == 0   no displacement (except the special case r/m == 6)
 *      mod == 1   one signed byte of displacement follows
 *      mod == 2   two bytes of displacement follow
 *
 * So `MOV AX, [BX+SI+4]` is an opcode byte, then a ModR/M with mod=1, reg=0
 * (AX), r/m=0 (BX+SI), then the byte 0x04.
 *
 * The 16-bit addressing modes are a fixed menu of only eight combinations —
 * you cannot form an arbitrary base+index pair. That restriction is why so much
 * DOS-era assembly juggles values through BX and SI.
 *
 * decode_modrm() leaves its results in these file-level variables rather than
 * returning a struct. It is not elegant, but it means the instruction handlers
 * can just call decode_modrm() and then read what they need.
 * ------------------------------------------------------------------------ */
static int mod_, reg_, rm_, ismem_;
static uint32_t addr_;      /* resolved physical address, if ismem_ */
static uint16_t off_;       /* the offset part alone — LEA needs this */

static void decode_modrm(void){
    uint8_t m = fetch8();
    mod_ = m>>6; reg_ = (m>>3)&7; rm_ = m&7;

    if(mod_==3){ ismem_=0; return; }     /* r/m is a register; nothing to compute */
    ismem_=1;

    int defseg = rDS; uint16_t base=0;
    if(adsz==2){
        /* The eight 16-bit addressing modes. Note which ones default to the
         * stack segment: anything involving BP. BP is the frame pointer — the
         * register a function points at its own patch of stack so it can reach
         * its arguments and local variables at constant offsets (see THE STACK
         * below) — so an address built from BP is almost certainly a local
         * variable, and locals live in the stack segment. The hardware bakes
         * that assumption in: mention BP in an address and the default segment
         * silently changes from DS to SS. Mention BX instead and it stays DS.
         * Two registers that look interchangeable are not. */
        switch(rm_){
        case 0: base=BX+SI; break;
        case 1: base=BX+DI; break;
        case 2: base=BP+SI; defseg=rSS; break;
        case 3: base=BP+DI; defseg=rSS; break;
        case 4: base=SI; break;
        case 5: base=DI; break;
        /* The special case: r/m==6 would mean "[BP]" with no displacement, but
         * that encoding is stolen to mean "a bare 16-bit address". You can
         * still write [BP] — the assembler emits mod=1 with a displacement of
         * zero. */
        case 6: if(mod_==0){ base=fetch16(); } else { base=BP; defseg=rSS; } break;
        case 7: base=BX; break;
        }
        /* Add the displacement — the constant offset written in the
         * instruction, as in [BX+4] or [BP-6]. The (int8_t) cast is what makes
         * the one-byte form signed: the byte 0xFF is 255 read as unsigned and
         * -1 read as a signed 8-bit two's-complement number, and the cast picks
         * the second reading, so you can address [BX-1] in a single byte.
         *
         * `base += (int8_t)fetch8()` with base a uint16_t is a good example of
         * relying on modular arithmetic on purpose. The -1 is widened to the
         * unsigned type as 65535, the addition is taken modulo 65536, and
         * base + 65535 == base - 1. Wrapping is not being tolerated here, it is
         * being *used*, and it also happens to reproduce the hardware exactly:
         * a 16-bit offset that runs off the end of a segment wraps back to the
         * start of the same segment rather than spilling into the next one. */
        if(mod_==1) base += (int8_t)fetch8();
        else if(mod_==2) base += (uint16_t)fetch16();
        off_ = base;
        addr_ = lin(S[seg_for(defseg)], base);
    } else {
        /* 32-bit addressing, present for completeness. Here the encoding is far
         * more flexible (any base register, any index register, a scale of
         * 1/2/4/8) at the cost of a second byte, the SIB, when r/m == 4. */
        uint32_t b=0; int sindex=-1, scale=0;
        if(rm_==4){ uint8_t sib=fetch8(); scale=sib>>6; sindex=(sib>>3)&7; int bs=sib&7;
            if(bs==5 && mod_==0) b=fetch32(); else { b=R[bs]; if(bs==4||bs==5) defseg=rSS; }
            if(sindex!=4) b += R[sindex]<<scale;   /* index 4 encodes "no index" */
        } else if(rm_==5 && mod_==0){ b=fetch32(); }
        else { b=R[rm_]; if(rm_==5) defseg=rSS; }
        if(mod_==1) b += (int8_t)fetch8();
        else if(mod_==2) b += fetch32();
        off_ = (uint16_t)b;
        addr_ = lin(S[seg_for(defseg)], (uint16_t)b);
    }
}

/* Read or write "the r/m operand", whichever kind it turned out to be. Having
 * this pair means every instruction handler can ignore the register-versus-
 * memory distinction entirely. */
static uint32_t readrm(int w){
    if(ismem_) return w==1?rd8(addr_): w==2?rd16(addr_):rd32(addr_);
    return getreg(rm_,w);
}
static void writerm(int w, uint32_t v){
    if(ismem_){ if(w==1) wr8(addr_,v); else if(w==2) wr16(addr_,v); else wr32(addr_,v); }
    else setreg(rm_,w,v);
}

/* ===========================================================================
 * THE ARITHMETIC UNIT
 *
 * Getting the *results* right is easy. Getting the *flags* right is the actual
 * work, and it matters enormously: a decompressor is mostly conditional jumps,
 * so one wrong flag turns the output to noise. This is the part of an emulator
 * where subtle bugs hide.
 *
 * WHAT THE FLAGS ARE FOR. This CPU has no boolean type and no comparison
 * expression. Arithmetic instructions leave a few bits of commentary about
 * their own result in the flags — was it zero, was it negative, did it carry
 * out of the top — and the conditional jump instructions then branch on those
 * bits. So `if (a < b)` compiles to two instructions that communicate through
 * hidden global state: a CMP that sets the flags, and a jump that reads them.
 * Every arithmetic and logical operation writes that state, whether the program
 * intends to use it or not, which is why every function below sets flags as a
 * side effect and why an emulator has to get all of it exactly right rather
 * than only the parts that look load-bearing.
 *
 * A note on types before you read the code: every value in here is unsigned,
 * and signed interpretations are applied by explicit casts at the moment they
 * are needed. That is deliberate. Unsigned arithmetic in C wraps in a fully
 * defined way (results are modulo 2^width), which is exactly what the hardware
 * does; signed overflow in C is undefined behaviour, meaning the compiler is
 * entitled to assume it never happens and optimise accordingly. Modelling a
 * machine whose arithmetic is *supposed* to overflow therefore has to be done
 * in unsigned types.
 * ======================================================================== */

/* Addition, shared by ADD (carry=0) and ADC, add-with-carry (carry=1).
 *
 * The trick in the first two lines: do the arithmetic in a uint64_t, which is
 * wider than any operand, so nothing is lost. Then the bits above the operand
 * width tell us about carry, and masking down to the width gives the result the
 * emulated CPU would see. */
static uint32_t op_add(uint32_t a,uint32_t b,int w,int carry){
    uint32_t c = carry?(CF?1:0):0;
    uint64_t r = (uint64_t)a + b + c;
    uint32_t rr = (uint32_t)r & msk(w);

    /* Carry: did the sum need one more bit than the operand width? */
    CF = ((r >> (w*8)) & 1) != 0;

    /* Auxiliary carry: was there a carry out of bit 3, i.e. between the two
     * nibbles of the low byte? Only the packed-decimal instructions use it.
     * XORing the inputs with the result isolates the bits where a carry must
     * have happened. */
    AF = (((a ^ b ^ rr) >> 4) & 1) != 0;

    /* Signed overflow: true when both inputs had the same sign and the result
     * has the opposite one — the only way a signed add can be wrong.
     *   ~(a^b)   bits where a and b agree
     *   (a^rr)   bits where the result differs from a
     * AND them and look at the sign bit. */
    OF = (((~(a^b)) & (a^rr)) >> msb(w)) & 1;

    ZF = rr==0; SF=(rr>>msb(w))&1; PF=parity(rr&0xFF);
    return rr;
}

/* Subtraction, shared by SUB, SBB (subtract-with-borrow), CMP and NEG.
 *
 * Same structure. The unsigned arithmetic underflows deliberately: 3 - 5 in
 * 64-bit unsigned is a huge number whose bit above the operand width is set,
 * which is exactly the borrow we want to detect. For subtraction the overflow
 * test flips: a signed subtract is wrong when the inputs had *different* signs
 * and the result took the sign of the second one. */
static uint32_t op_sub(uint32_t a,uint32_t b,int w,int borrow){
    uint32_t c = borrow?(CF?1:0):0;
    uint64_t r = (uint64_t)a - b - c;
    uint32_t rr = (uint32_t)r & msk(w);
    CF = ((r >> (w*8)) & 1) != 0;
    AF = (((a ^ b ^ rr) >> 4) & 1) != 0;
    OF = (((a^b) & (a^rr)) >> msb(w)) & 1;
    ZF = rr==0; SF=(rr>>msb(w))&1; PF=parity(rr&0xFF);
    return rr;
}

/* The eight basic ALU operations, in the order the instruction set numbers
 * them. That ordering is the reason a single block of code in step() can handle
 * 32 different opcodes: the opcode byte itself contains this number.
 *
 *   0 ADD   1 OR   2 ADC   3 SBB   4 AND   5 SUB   6 XOR   7 CMP
 *
 * CMP is just SUB that throws its result away — it exists to set flags. Callers
 * check for op==7 and skip the write-back. */
static uint32_t alu(int op,uint32_t a,uint32_t b,int w){
    uint32_t r;
    switch(op){
    case 0: r=op_add(a,b,w,0); break;
    case 1: r=(a|b)&msk(w); setlogic(r,w); break;
    case 2: r=op_add(a,b,w,1); break;
    case 3: r=op_sub(a,b,w,1); break;
    case 4: r=(a&b)&msk(w); setlogic(r,w); break;
    case 5: r=op_sub(a,b,w,0); break;
    case 6: r=(a^b)&msk(w); setlogic(r,w); break;
    default: r=op_sub(a,b,w,0); break; /* CMP: result discarded by the caller */
    }
    return r;
}

/* INC and DEC are add/subtract 1 with one deliberate exception: they do NOT
 * touch the carry flag. That is not a quirk, it is the point — it lets a loop
 * increment a counter without disturbing a carry it is accumulating across
 * iterations, e.g. in multi-word arithmetic. Hence saving and restoring CF. */
static uint32_t op_inc(uint32_t a,int w){ int cf=CF; uint32_t r=op_add(a,1,w,0); CF=cf; return r; }
static uint32_t op_dec(uint32_t a,int w){ int cf=CF; uint32_t r=op_sub(a,1,w,0); CF=cf; return r; }

/* ---------------------------------------------------------------------------
 * SHIFTS AND ROTATES
 *
 * Shifts move every bit of a value left or right by some number of positions.
 * As arithmetic, a left shift by n multiplies by 2^n and an unsigned right
 * shift divides by 2^n, discarding the remainder — which is why shifts were the
 * standard way to multiply and divide by powers of two on a chip where a real
 * multiply cost dozens of cycles. As field manipulation, a shift is what moves
 * a packed field to or from the bottom of a word, as in the note above
 * getreg8(). Rotates are the same motion made circular: bits that fall off one
 * end reappear at the other, so nothing is lost.
 *
 * Eight operations sharing one encoding, again numbered by a field in the
 * instruction:
 *
 *   0 ROL  rotate left           bits that fall off the left re-enter on the right
 *   1 ROR  rotate right
 *   2 RCL  rotate left through carry    a 9/17/33-bit rotate: CF is the extra bit
 *   3 RCR  rotate right through carry
 *   4 SHL  shift left            zeros enter on the right
 *   5 SHR  shift right, unsigned zeros enter on the left
 *   6 —    an undocumented alias of SHL, hence "case 4: case 6:"
 *   7 SAR  shift right, signed   the sign bit is replicated, so -8 >> 1 is -4
 *
 * This function matters more than it looks. A compressor's bit reader is built
 * out of exactly these, usually SHL followed by RCL to pull one bit at a time
 * into a register — in this program, shift-by-one was the single most executed
 * instruction, about 8.6 million times.
 *
 * `cnt &= 31` masks the shift count to 5 bits, which is what a 286 and later do
 * (the original 8086 did not mask, so a count of 200 really did take 200 clock
 * cycles). The mask also protects the C code: shifting a value by more than its
 * own width is undefined behaviour in C, so the explicit `cnt<bits` guards
 * below are not paranoia, they are required for correctness.
 * ------------------------------------------------------------------------ */
static uint32_t do_shift(int op,uint32_t v,int cnt,int w){
    int bits=w*8; uint32_t m=msk(w);
    cnt &= 31;
    if(cnt==0) return v;         /* a zero-count shift changes nothing, not even flags */
    uint32_t r=v&m;
    switch(op){
    case 0: /* ROL */ { int c=cnt%bits; if(c) r=((r<<c)|(r>>(bits-c)))&m; CF=r&1; OF=((r>>(bits-1))&1)^CF; } break;
    case 1: /* ROR */ { int c=cnt%bits; if(c) r=((r>>c)|(r<<(bits-c)))&m; CF=(r>>(bits-1))&1; OF=((r>>(bits-1))&1)^((r>>(bits-2))&1);} break;
    case 2: /* RCL */ { for(int i=0;i<cnt;i++){ int nc=(r>>(bits-1))&1; r=((r<<1)|(CF?1:0))&m; CF=nc; } OF=((r>>(bits-1))&1)^(CF?1:0); } break;
    case 3: /* RCR */ { for(int i=0;i<cnt;i++){ int nc=r&1; r=(r>>1)|((CF?1u:0u)<<(bits-1)); CF=nc; } OF=((r>>(bits-1))&1)^((r>>(bits-2))&1); } break;
    case 4: case 6: /* SHL */ { if(cnt<=bits){ uint64_t t=(uint64_t)r<<cnt; CF=(t>>bits)&1; r=(uint32_t)t&m; } else { CF=0; r=0; }
                                OF=((r>>(bits-1))&1)^(CF?1:0); ZF=r==0; SF=(r>>(bits-1))&1; PF=parity(r&0xFF); } break;
    case 5: /* SHR */ { CF = cnt<=bits ? ((r>>(cnt-1))&1) : 0; OF=(v>>(bits-1))&1; r = cnt<bits ? (r>>cnt) : 0;
                        ZF=r==0; SF=(r>>(bits-1))&1; PF=parity(r&0xFF); } break;
    case 7: /* SAR */ { int32_t sv; if(w==1) sv=(int8_t)v; else if(w==2) sv=(int16_t)v; else sv=(int32_t)v;
                        int c = cnt>=bits? bits-1: cnt;
                        CF=(sv>>(c-1))&1; sv >>= c; r=(uint32_t)sv & m; OF=0;
                        ZF=r==0; SF=(r>>(bits-1))&1; PF=parity(r&0xFF); } break;
    }
    return r;
}

/* ---------------------------------------------------------------------------
 * THE STACK
 *
 * WHAT THE STACK IS. It is not a special kind of storage. It is an ordinary
 * region of ordinary memory, plus one register (SP) holding the address of its
 * current top, plus a discipline: last in, first out. Two instructions maintain
 * it. PUSH moves SP to make room and writes a value there; POP reads the value
 * at SP and moves SP back. That is the entire mechanism, and everything that
 * follows — function calls, local variables, recursion, stack traces — is built
 * out of it by convention rather than by hardware.
 *
 * WHY IT GROWS DOWNWARD. Two reasons, one architectural and one practical. The
 * architectural one is simply that the instruction set says so: PUSH decrements
 * SP, and there is no way to ask for the other direction. The practical one is
 * the classic address-space layout. A program's code and its static data sit at
 * low addresses, and any heap it allocates grows upward from just above them.
 * If the stack starts at the *highest* address and grows down, then the two
 * dynamic regions grow toward each other through the same pool of free space,
 * and neither has to have its maximum size decided in advance. They collide
 * only when memory is genuinely exhausted. Had both grown upward, you would
 * have to pick a boundary between them ahead of time and would waste whichever
 * side you overestimated.
 *
 * WHAT "PUSHING A RETURN ADDRESS" MEANS. A CALL instruction does two things:
 * it pushes the address of the instruction *after* the call, and then sets IP
 * to the target. The pushed value is the return address — a note saying "when
 * you are done, control should resume here". RET pops that note back into IP.
 * Nothing else marks the call: there is no separate call stack, no activation
 * record object, no bookkeeping the hardware maintains on your behalf. Because
 * pushes and pops nest exactly the way calls and returns nest, one LIFO region
 * naturally holds the return addresses of an arbitrarily deep chain of calls,
 * and recursion works with no extra machinery at all. It also means that
 * corrupting the stack corrupts control flow itself, which is the mechanism
 * behind the entire family of stack-smashing attacks.
 *
 * LOCAL VARIABLES AND STACK FRAMES. A function's locals normally live on the
 * stack too, because that gives them exactly the lifetime you want for free:
 * they come into existence when the function subtracts their total size from
 * SP, and they cease to exist when SP is restored on return. No allocation, no
 * freeing, no fragmentation — just moving one register. This is the real
 * difference between "the stack" and "the heap": stack storage is reclaimed in
 * strict reverse order of creation, automatically, at zero cost; heap storage
 * (see THE MEMORY MANAGER) can be released in any order and therefore needs a
 * real bookkeeping algorithm.
 *
 * The region belonging to one active call is its STACK FRAME. A typical frame
 * on this machine, drawn with high addresses at the top, looks like:
 *
 *      ...caller's frame...
 *      [ argument 2      ]  BP+6    pushed by the caller before the CALL
 *      [ argument 1      ]  BP+4
 *      [ return address  ]  BP+2    pushed by the CALL instruction
 *      [ saved BP        ]  BP+0    <- BP points here, set on entry
 *      [ local variable  ]  BP-2
 *      [ local variable  ]  BP-4
 *      [ ...temporaries  ]          <- SP, moves as the function pushes/pops
 *      lower addresses
 *
 * BP is the FRAME POINTER: fixed for the duration of the call, so every
 * argument and local has a constant offset from it — which is exactly why
 * decode_modrm() above treats an address built on BP as belonging to the stack
 * segment. SP meanwhile wanders as the function pushes intermediate values, so
 * it is a poor thing to measure from. The prologue that establishes this
 * (push BP; mov BP,SP; sub SP,locals) and the epilogue that unwinds it are
 * common enough to have dedicated instructions: see ENTER and LEAVE at opcodes
 * 0xC8 and 0xC9 in step().
 *
 * A CALLING CONVENTION is the agreement that makes all of this interoperate:
 * which arguments go in which registers or in what order on the stack, who
 * removes them afterwards, which registers a function may clobber and which it
 * must preserve, and where the return value appears. None of it is enforced by
 * the hardware — it is a protocol two pieces of code must both follow, and code
 * compiled under two different conventions will corrupt the stack when one
 * calls the other. DOS-era compilers shipped several (the C convention, where
 * the caller removes the arguments and so variadic functions like printf are
 * possible; the Pascal convention, where the callee removes them with the RET n
 * instruction at 0xC2 below, saving a couple of bytes per call site).
 *
 * SS:SP points at the top of the stack, and the stack grows *downwards* — push
 * decrements SP then writes, pop reads then increments. Everything is 2 bytes
 * wide because this is a 16-bit machine.
 *
 * There is no stack bounds checking, in the emulator or on the real hardware. A
 * program that pushes too much simply overwrites whatever is below its stack,
 * and the failure surfaces later as corrupted data or a wild jump, with nothing
 * pointing back at the cause. The guard pages and stack-overflow exceptions you
 * get on a modern system are provided by the memory-management hardware and the
 * operating system, neither of which exists here.
 * ------------------------------------------------------------------------ */
static void push16(uint16_t v){ SP-=2; wr16(lin(S[rSS],SP), v); }
static uint16_t pop16(void){ uint16_t v=rd16(lin(S[rSS],SP)); SP+=2; return v; }


/* ===========================================================================
 * THE MEMORY MANAGER  (a complete, working heap allocator, in 100 lines)
 *
 * WHAT A MEMORY ALLOCATOR IS FOR
 *
 * A program starts life owning one contiguous region of memory and a stack. The
 * stack solves the easy case: storage whose lifetime nests, created and
 * destroyed in strict reverse order by moving one register (see THE STACK). The
 * hard case is everything else — storage whose size is not known until run time
 * and whose lifetime does not nest, like a buffer whose length depends on the
 * file you just opened, or a list of records that outlives the function that
 * built it. That is what a *heap* is: one large region, plus a piece of
 * bookkeeping software that carves it into non-overlapping pieces on request
 * and takes them back afterwards, in any order.
 *
 * The interface is small — "give me n bytes" and "here, have these back" — and
 * that smallness hides the whole problem. The allocator must, using only the
 * memory it is managing, answer arbitrary requests in arbitrary order, and
 * arrange that no two live requests ever overlap. Everything below is that
 * problem and nothing else.
 *
 * THE FOUR IDEAS, all of which appear in the code that follows:
 *
 *  1. THE FREE LIST. The allocator keeps a record of which parts of the region
 *     are in use and which are not. Real DOS keeps it as a linked list of
 *     Memory Control Blocks, each sitting in the paragraph just before the
 *     block it describes — the bookkeeping is stored inside the memory being
 *     managed, which is why a program that overruns a buffer by a few bytes on
 *     such a system corrupts the allocator itself. This emulator uses a plain
 *     array, blk[], kept sorted by address; the semantics are the same and the
 *     debugging is far easier.
 *
 *  2. FIRST FIT. When a request arrives, walk the list and take the first free
 *     region large enough. The obvious alternative, best fit, scans them all
 *     and takes the smallest adequate one, hoping to leave larger holes intact;
 *     it costs a full scan and in practice is not reliably better. Worst fit
 *     takes the largest, on the theory that the leftover is more likely to be
 *     useful. There is no universally best policy — the right answer depends on
 *     the request pattern, and allocator design is largely empirical.
 *
 *  3. SPLITTING. The region you found is usually bigger than the request, so
 *     the allocator carves off the front and records the remainder as a new,
 *     smaller free region. One entry in the list becomes two. mm_alloc() does
 *     this, and mm_reserve() does the three-way version.
 *
 *  4. COALESCING, and the problem it fights: FRAGMENTATION. Free a block that
 *     sits between two other free blocks and you now have three adjacent free
 *     regions where you should have one. Nothing is lost in total, but a later
 *     request for a large block will fail even though the free bytes exist,
 *     because they are not contiguous. That is *external fragmentation*: memory
 *     that is free and yet unusable. mm_coalesce() prevents the worst of it by
 *     merging neighbouring free entries after every release — which is exactly
 *     why blk[] is kept sorted by address, since "neighbour in the address
 *     space" must be the same as "neighbour in the list" for the merge test to
 *     be a single comparison.
 *
 * WHY BLOCKS CANNOT SIMPLY BE MOVED. The obvious fix for fragmentation is
 * compaction: slide the live blocks together and the free space becomes one
 * piece again. An allocator at this level cannot do it. The program holds bare
 * addresses into the blocks it owns (see the pointer note in the REGISTER NAME
 * SHORTHAND section: a pointer is just an integer), the allocator has no record
 * of where those addresses are stored, and there is nothing to update. A
 * language with a compacting garbage collector *can* move objects precisely
 * because its runtime does know where every reference lives. This constraint is
 * also why mm_resize() below can only grow a block into the free space that
 * immediately follows it, and must otherwise fail.
 *
 * GRANULARITY. DOS hands out memory in units of one paragraph — 16 bytes, the
 * spacing of segment starts (see the segmentation note above lin()). Sizes and
 * addresses in this file are therefore in paragraphs, not bytes, which is what
 * lets a 16-bit number describe any position in a one-megabyte address space:
 * 0x10000 paragraphs * 16 = 0x100000 bytes. Rounding every request up to a
 * whole paragraph wastes a few bytes per allocation — that waste is *internal
 * fragmentation*, the other half of the pair — in exchange for much simpler
 * arithmetic. Every allocator makes some version of this trade.
 *
 * THE BUG THAT LIVED HERE, which is the reason this section is worth reading.
 * The first version of this file let every "grow this block" request succeed
 * without actually reserving the space, on the theory that nothing would
 * notice. The extractor allocates a ~9 KB buffer per track and then grows it in
 * 8 KB steps up to 64 KB — so the next allocation was handed a segment *inside*
 * the block that had just grown, every track's buffer overlapped its neighbour,
 * and the decompressor spent the run overwriting its own output. The images
 * that came out looked plausible (valid boot sector, two matching copies of the
 * FAT, real driver text) but had no root directory anywhere and repeated the
 * same 55,266 bytes forever.
 *
 * Note what kind of failure that is. The CPU emulation was perfect; every
 * instruction did exactly the right thing to exactly the bytes it was told to.
 * The lie was in the *allocator's invariant* — "no two live blocks overlap" —
 * and once that is broken, correct code operating on correct data produces
 * garbage, with the corruption appearing arbitrarily far from its cause. This
 * is the characteristic signature of a memory-management bug in any language
 * that has one, and it is why the invariant is worth stating out loud before
 * reading the code.
 *
 * The lesson: when an emulated program produces subtly wrong output, suspect
 * the operating system you wrote before you suspect the processor.
 *
 * What follows is an ordinary first-fit allocator: keep an array of blocks
 * sorted by address, split one when part of it is claimed, and merge adjacent
 * free ones when something is released.
 * ======================================================================== */
#define MAXBLK 512

/* The free list itself. An anonymous struct type declared inline with the
 * array; each entry is one region: where it starts (in paragraphs), how big it
 * is, and whether it is allocated.
 *
 * The invariant this whole section maintains, and which the bug described above
 * violated: the entries tile the managed region exactly — sorted by address,
 * with no gaps and no overlaps, blk[i].seg + blk[i].size == blk[i+1].seg for
 * every i. Every function below preserves that, and reading them is easiest if
 * you check each one against it.
 *
 * C structs are plain value types with no identity and no hidden header —
 * `blk[k]=blk[k-1]` copies all six bytes of the struct, which is how the insert
 * loops shuffle entries along to make room. Inserting into a sorted array by
 * shifting the tail is O(n) per insert and would be poor practice at scale;
 * with at most a few dozen live blocks it is faster than chasing pointers. */
static struct { uint16_t seg, size; int used; } blk[MAXBLK];
static int nblk=0;

/* Start with the whole free region as a single unallocated block. */
static void mm_init(uint16_t start, uint16_t top){
    nblk=1; blk[0].seg=start; blk[0].size=top-start; blk[0].used=0;
}

/* Mark an already-occupied region as used — needed at startup for the program
 * image and its environment, which are placed in memory before the program ever
 * asks for anything. Splits the enclosing free block into up to three pieces:
 * the part before, the reserved part, and the part after. */
static void mm_reserve(uint16_t seg, uint16_t size){
    for(int i=0;i<nblk;i++){
        if(!blk[i].used && seg>=blk[i].seg && seg+size<=blk[i].seg+blk[i].size){
            uint16_t s0=blk[i].seg, n0=blk[i].size;
            /* [s0,seg) free | [seg,seg+size) used | [seg+size, s0+n0) free */
            int j=i;
            blk[j].seg=seg; blk[j].size=size; blk[j].used=1;
            if(seg+size < s0+n0){
                for(int k=nblk;k>j+1;k--) blk[k]=blk[k-1];
                nblk++;
                blk[j+1].seg=seg+size; blk[j+1].size=(s0+n0)-(seg+size); blk[j+1].used=0;
            }
            if(seg>s0){
                for(int k=nblk;k>j;k--) blk[k]=blk[k-1];
                nblk++;
                blk[j].seg=s0; blk[j].size=seg-s0; blk[j].used=0;
                }
            return;
        }
    }
}
/* COALESCE: merge neighbouring free blocks, so that repeated allocate/free
 * cycles do not shred memory into a lot of small unusable holes. Merging is
 * possible at all only because the array is kept sorted by address: entries i
 * and i+1 are guaranteed to be physically adjacent, so combining them is
 * `blk[i].size += blk[i+1].size` followed by deleting entry i+1.
 *
 * Note the loop only advances `i` when it does *not* merge — after a merge, the
 * newly enlarged block might also be mergeable with the next one along, and
 * skipping past it would leave two adjacent free entries behind. A three-way
 * merge (free a block whose neighbours on both sides are already free) resolves
 * as two passes of this one rule. */
static void mm_coalesce(void){
    for(int i=0;i+1<nblk;){
        if(!blk[i].used && !blk[i+1].used){
            blk[i].size += blk[i+1].size;
            for(int k=i+1;k+1<nblk;k++) blk[k]=blk[k+1];   /* delete entry i+1 */
            nblk--;
        } else i++;
    }
}

/* Look up an allocated block by its starting segment. Returns -1 if not found —
 * C has no option type, so "impossible index" is the idiom. */
static int mm_find(uint16_t seg){ for(int i=0;i<nblk;i++) if(blk[i].used && blk[i].seg==seg) return i; return -1; }

/* Size of the biggest free block — not the total free space, which may be much
 * larger and spread over several holes. The difference between those two
 * numbers is the amount of external fragmentation. DOS reports this figure when
 * an allocation fails, so a program can retry asking for what is actually
 * available in one piece, which is exactly what this extractor does. */
static uint16_t mm_largest(void){ uint16_t m=0; for(int i=0;i<nblk;i++) if(!blk[i].used && blk[i].size>m) m=blk[i].size; return m; }

/* ALLOCATE, first fit: walk the list in address order, take the first free
 * block big enough, and split off the unused remainder as a new free block that
 * follows it. The scan is O(number of blocks) and the split is O(number of
 * blocks) too because of the array shuffle, so a request costs a linear pass —
 * acceptable here, and the reason production allocators keep size-bucketed free
 * lists so that the common case is O(1).
 *
 * The `nblk<MAXBLK` guard is worth noticing: if the table is full we hand back
 * the whole block without splitting, which wastes the tail but never breaks the
 * tiling invariant. Degrading by wasting space rather than by corrupting
 * structure is the right failure mode for this kind of code.
 *
 * `uint16_t *out` is C's stand-in for an out-parameter: the caller passes the
 * address of a variable and we write through the pointer with `*out = ...`. The
 * return value is then free to mean success or failure. C functions return one
 * value and have no tuples and no exceptions, so "result plus status" is
 * conventionally spelled this way. */
static int mm_alloc(uint16_t want, uint16_t *out){
    if(want==0){ *out=0; return 1; }
    for(int i=0;i<nblk;i++){
        if(!blk[i].used && blk[i].size>=want){
            if(blk[i].size>want && nblk<MAXBLK){
                for(int k=nblk;k>i+1;k--) blk[k]=blk[k-1];
                nblk++;
                blk[i+1].seg=blk[i].seg+want; blk[i+1].size=blk[i].size-want; blk[i+1].used=0;
                blk[i].size=want;
            }
            blk[i].used=1; *out=blk[i].seg; return 1;
        }
    }
    return 0;
}
static int mm_free(uint16_t seg){ int i=mm_find(seg); if(i<0) return 0; blk[i].used=0; mm_coalesce(); return 1; }

/* RESIZE, grow or shrink an existing block in place — the operation that was
 * broken, and the one worth reading most carefully.
 *
 * Shrinking always works: keep the front, hand the tail back as free space,
 * then coalesce in case the block after it was free too.
 *
 * Growing only works if the block immediately after this one is free and big
 * enough, because a block cannot move: the program is already holding raw
 * addresses into it, and neither DOS nor this emulator has any way to find and
 * update them. So the choice is grow-in-place or fail. (A higher-level
 * allocator such as C's realloc() has a third option — allocate elsewhere, copy
 * the contents, free the original — but that is only safe because realloc()
 * hands back a new address and its caller is required to stop using the old
 * one. The DOS call has no way to express that.)
 *
 * If it will not fit, we report the largest size that *would* have fitted,
 * which is what real DOS does and what lets a caller negotiate downwards. That
 * negotiation is exactly the protocol this extractor uses: ask for 64 KB, be
 * told the maximum, take what is offered. The original bug was to return
 * success here unconditionally without touching the free list — the caller
 * believed it owned memory that the allocator still considered free, and handed
 * out to somebody else moments later. */
static int mm_resize(uint16_t seg, uint16_t want, uint16_t *maxp){
    int i=mm_find(seg); if(i<0){ *maxp=0; return 0; }
    uint16_t cur=blk[i].size;
    if(want<=cur){
        if(want<cur && nblk<MAXBLK){
            for(int k=nblk;k>i+1;k--) blk[k]=blk[k-1];
            nblk++;
            blk[i+1].seg=blk[i].seg+want; blk[i+1].size=cur-want; blk[i+1].used=0;
            blk[i].size=want; mm_coalesce();
        }
        return 1;
    }
    uint16_t avail=cur;
    if(i+1<nblk && !blk[i+1].used) avail += blk[i+1].size;
    if(avail<want){ *maxp=avail; return 0; }
    /* absorb from the following free block */
    uint16_t need=want-cur;
    blk[i].size=want;
    blk[i+1].seg += need; blk[i+1].size -= need;
    if(blk[i+1].size==0){ for(int k=i+1;k+1<nblk;k++) blk[k]=blk[k+1]; nblk--; }
    return 1;
}

/* ===========================================================================
 * THE OPERATING SYSTEM
 *
 * WHAT A SYSTEM CALL IS, IN GENERAL
 *
 * A program on its own can compute, but it cannot do anything you would notice:
 * reading a file, writing to a screen, asking for more memory and finding out
 * what time it is are all operations on resources the program does not own and
 * cannot reach by itself. Those live behind the operating system, and a *system
 * call* is the controlled doorway through which a program asks the OS to
 * perform one on its behalf.
 *
 * It cannot be an ordinary function call, for two reasons. The program does not
 * know the address of the OS routine — the OS is a separate body of code that
 * may be a different version tomorrow — and on any system with memory
 * protection the program is not permitted to jump into it even if it did know.
 * So instead there is a rendezvous protocol, and the protocol always has the
 * same three parts: an agreed place to put the arguments, an agreed way to
 * name which service you want, and a single special instruction that transfers
 * control to a fixed, OS-chosen entry point rather than to an address of the
 * caller's choosing.
 *
 * HOW A DOS PROGRAM ASKS FOR SOMETHING
 *
 * There are no system call instructions and no linking against an OS library.
 * A program puts arguments in registers and executes `INT n` — a software
 * interrupt. The CPU looks up entry n in the interrupt vector table (256 far
 * pointers at the very bottom of memory), pushes the flags and the return
 * address, and jumps there. The handler does its work and returns with IRET.
 *
 * That is the same three parts: arguments in registers, service selected by the
 * interrupt number plus a function number in AH, control transferred by a
 * dedicated instruction through a table the OS filled in. What is *missing*
 * compared to a modern system is the protection. The 8086 has no privileged
 * mode and no memory protection, so INT 21h is in the end an indirect call
 * through a table in ordinary memory, and there is nothing stopping a program
 * from overwriting that table, calling the handler address directly, or simply
 * writing to the hardware itself — all of which DOS-era software did routinely.
 *
 * A modern equivalent looks like this. Your C code calls read(), an ordinary
 * function in the C library, in your own address space, on your own stack —
 * that part is a plain library call, no OS involvement, and if a call never
 * needs the kernel (strlen, say) it never leaves. Inside read(), the library
 * loads a syscall number and the arguments into specific registers and executes
 * one instruction (`syscall` on x86-64, `svc` on ARM) whose entire purpose is
 * to switch the processor into kernel mode and jump to a fixed entry point the
 * kernel registered at boot. The kernel checks the arguments precisely because
 * it does not trust them, does the work, and returns, switching back. The
 * shapes line up almost exactly with DOS; what forty years added was the
 * privilege boundary and the validation that goes with it.
 *
 * The other thing worth naming is that this arrangement is what an ABI is —
 * an application *binary* interface, a contract expressed in registers and
 * instruction encodings rather than in function signatures. Because the
 * contract is binary, a program compiled in 1983 still runs on DOS 6.22 in
 * 1998 with no recompilation, and, more to the point here, it is a contract
 * this emulator can satisfy without possessing any of DOS.
 *
 * The service is selected by the interrupt number plus, usually, a function
 * number in the AH register. So "print a string" is: point DS:DX at the text,
 * put 9 in AH, `INT 21h`. Note that "point DS:DX at the text" is how you pass a
 * pointer when the argument registers are 16 bits and addresses need 20: the
 * segment goes in one register and the offset in another. The conventions that
 * matter here:
 *
 *   INT 21h   DOS itself: files, memory, console, process control
 *   INT 13h   BIOS disk services: raw sector reads and writes
 *   INT 10h   BIOS video
 *   INT 16h   BIOS keyboard
 *   INT 25h/26h   DOS absolute disk read/write, by logical sector
 *
 * Errors are reported by setting the carry flag, with a code in AX. Because the
 * flags were pushed on entry and IRET restores them, a handler cannot signal an
 * error by setting CF in the live register — it has to reach into the stack and
 * modify the *saved* copy. That is what set_cf_on_stack() below is for, and it
 * is the kind of detail that is invisible until you get it wrong.
 * ======================================================================== */

static uint16_t psp_seg, env_seg, alloc_next, alloc_top;
static uint32_t dta;                 /* Disk Transfer Area, set by AH=1Ah */
static int trap_written = 0;         /* count of sectors written, for reporting */

static void out_str(const char*s){ fputs(s, stderr); }

/* Set or clear the carry flag in the FLAGS word that the INT instruction pushed
 * on the stack, so that the IRET at the end of our stub restores it.
 *
 * The +4 is the stack layout at the moment a handler runs:
 *      SS:SP+0   return IP
 *      SS:SP+2   return CS
 *      SS:SP+4   saved FLAGS      <- the one we want
 */
static void set_cf_on_stack(int cf){
    uint32_t a = lin(S[rSS], SP+4);
    uint16_t f = rd16(a);
    if(cf) f|=1; else f&=~1;
    wr16(a,f);
}

/* Convert a physical (cylinder, head, sector) address into a flat sector index.
 * Sector numbers are 1-based on this interface — a historical wart — hence the
 * `s-1`. Head varies fastest, then cylinder, which is why the two sides of the
 * disk interleave in the linear ordering. */
static int floppy_lba(int c,int h,int s){ return (c*FLOP_HD + h)*FLOP_SPT + (s-1); }

/* ---------------------------------------------------------------------------
 * INT 13h — BIOS disk services
 *
 * The low-level disk interface: talk in cylinders, heads and sectors, and copy
 * bytes to or from the buffer at ES:BX.
 *
 * THE LAYERS BETWEEN "SAVE THIS FILE" AND MAGNETISED IRON, because knowing
 * where this function sits explains what the extractor is doing and why the
 * output of this emulator is a disk image rather than a directory of files:
 *
 *   application      "write these bytes to the file README.TXT"
 *   filesystem       DOS finds free clusters, updates the FAT and the directory
 *                    entry, and turns the request into "write logical sectors
 *                    91 through 108"
 *   block layer      logical sector numbers are mapped to physical geometry:
 *                    sector 91 is cylinder 2, head 1, sector 10
 *   device driver    the BIOS routine reached by INT 13h, or a driver loaded
 *                    into DOS, issues the actual controller commands
 *   hardware         the floppy disk controller moves the head and clocks bytes
 *
 * Each layer knows only about the one below it. The value of the block layer's
 * interface — an addressable array of 512-byte blocks, see THE VIRTUAL FLOPPY
 * DRIVE above — is that everything beneath it can be swapped out without the
 * layers above noticing. A hard disk, a RAM disk, a network volume and this
 * emulator's floppy[] array are interchangeable as far as the filesystem code
 * is concerned.
 *
 * A self-extracting *disk image* program deliberately bypasses the top two
 * layers. It does not create files; it writes a pre-built copy of the entire
 * block array, filesystem structures and all, straight through the driver
 * interface. That is why intercepting this one layer is enough to capture the
 * whole diskette, and why the result can be handed to any tool that understands
 * FAT12 without the emulator ever implementing a filesystem.
 *
 * As it turned out these extractors do not use this path (they use the DOS
 * IOCTL "write track" call one layer up instead — see AH=44h, CL=41h in
 * int21()), but it is implemented because it is the obvious thing for a
 * disk-imaging tool to do and it was not obvious in advance which one they
 * would pick.
 * ------------------------------------------------------------------------ */
static void int13(void){
    int ah = getreg8(4); /* AH = function number */
    int dl = getreg8(2); /* DL = drive: 0/1 are floppies, 0x80+ are hard disks */
    if(dl >= 0x80){ setreg8(4,0x01); set_cf_on_stack(1); return; }  /* no hard disk here */
    switch(ah){
    case 0x00: setreg8(4,0); set_cf_on_stack(0); return;   /* reset drive: succeed */
    case 0x02: case 0x03: case 0x04: {                     /* read / write / verify */
        int cnt = getreg8(0);            /* AL = sector count */
        int cl = getreg8(1), ch = getreg8(5), dh = getreg8(6);
        /* The cylinder number is 10 bits split across two registers: the low 8
         * in CH, the top 2 in the high bits of CL. The sector number is the
         * bottom 6 bits of CL. This packing exists so the whole address fits in
         * three registers, and it is why BIOS disks topped out at 1024
         * cylinders. */
        int sect = cl & 0x3F;
        int cyl = ch | ((cl & 0xC0) << 2);
        uint32_t buf = lin(S[rES], BX);
        int done=0;
        for(int i=0;i<cnt;i++){
            int lba = floppy_lba(cyl,dh,sect+i);
            if(sect+i > FLOP_SPT || lba<0 || lba>=FLOP_CYL*FLOP_HD*FLOP_SPT) break;
            if(ah==0x02){ memcpy(mem+((buf+i*512)&0xFFFFF), floppy+lba*512, 512); }
            else if(ah==0x03){ memcpy(floppy+lba*512, mem+((buf+i*512)&0xFFFFF), 512); written[lba]=1; trap_written++; }
            done++;
        }
        setreg8(0,done); setreg8(4,0); set_cf_on_stack(0);
        if(verbose) fprintf(stderr,"[int13 ah=%02X c=%d h=%d s=%d n=%d ok=%d]\n",ah,cyl,dh,sect,cnt,done);
        return; }
    case 0x05: { /* format track: just zero it */
        int cl=getreg8(1), ch=getreg8(5), dh=getreg8(6);
        int cyl = ch | ((cl & 0xC0) << 2);
        for(int s=1;s<=FLOP_SPT;s++){ int lba=floppy_lba(cyl,dh,s); if(lba>=0&&lba<FLOP_CYL*FLOP_HD*FLOP_SPT) memset(floppy+lba*512,0xF6,512); }
        setreg8(4,0); set_cf_on_stack(0); return; }
    case 0x08:
        setreg8(4,0);            /* AH=0 */
        setreg8(3,0x04);         /* BL=4 -> 1.44M drive */
        setreg8(5,FLOP_CYL-1);   /* CH max cyl */
        setreg8(1,FLOP_SPT);     /* CL sectors */
        setreg8(6,FLOP_HD-1);    /* DH max head */
        setreg8(2,1);            /* DL drive count */
        S[rES]=0xF000; DI=0x0100;
        set_cf_on_stack(0); return;
    case 0x15: setreg8(4,0x02); DX=0; CX=0; set_cf_on_stack(0); return; /* floppy w/ change line */
    case 0x16: setreg8(4,0x00); set_cf_on_stack(0); return;
    case 0x17: case 0x18:
        setreg8(4,0x00); S[rES]=0xF000; DI=0x0100; set_cf_on_stack(0); return;
    case 0x01: setreg8(4,0); set_cf_on_stack(0); return;
    default:
        if(verbose) fprintf(stderr,"[int13 unhandled ah=%02X]\n",ah);
        setreg8(4,0x01); set_cf_on_stack(1); return;
    }
}

/* Find a free DOS file handle. Handles 0-4 are reserved by convention for
 * stdin, stdout, stderr, stdaux and stdprn, so user files start at 5. */
static int find_handle(void){ for(int i=5;i<64;i++) if(!handles[i]) return i; return -1; }

/* ---------------------------------------------------------------------------
 * INT 21h — DOS itself
 *
 * The big one: one interrupt, ~100 functions selected by AH. Only the ones this
 * program actually calls are implemented; the rest fall through to a default
 * that returns success and hopes for the best. That sounds reckless, and it is,
 * but a program that gets an unexpected failure from a call it did not expect
 * to fail usually behaves worse than one that gets a bland success.
 *
 * A running theme below: most of these are answered with a plausible constant
 * rather than a real implementation. There is no clock, so "get time" returns
 * midday. There is no filesystem, so every open succeeds and returns a handle
 * onto the .EXE we are running. That is enough because the program only ever
 * opens itself.
 * ------------------------------------------------------------------------ */
static void int21(void){
    int ah = getreg8(4);        /* AH selects the function */
    switch(ah){
    case 0x00: halted=1; exitcode=0; return;
    case 0x02: fputc(getreg8(2), stderr); return;
    case 0x06: {
        int dl=getreg8(2);
        if(dl==0xFF){ setreg8(0,0); ZF=1; }
        else fputc(dl, stderr);
        return; }
    case 0x07: case 0x08: setreg8(0,'\r'); return;
    case 0x09: {
        uint32_t a=lin(S[rDS],DX);
        for(int i=0;i<4096;i++){ uint8_t c=rd8(a+i); if(c=='$') break; fputc(c,stderr);} return; }
    case 0x0A: { uint32_t a=lin(S[rDS],DX); wr8(a+1,1); wr8(a+2,'\r'); return; }
    case 0x0B: setreg8(0,0); return;
    case 0x0C: setreg8(0,'\r'); return;
    case 0x0D: return;
    case 0x19: setreg8(0,2); return; /* current drive C: */
    case 0x1C: case 0x1B: { static uint8_t mediab=0xF0; wr8(lin(0x0060,0), 0xF0);
        setreg8(0,1); CX=512; DX=2847; S[rDS]=0x0060; BX=0; return; }
    case 0x1A: dta = lin(S[rDS],DX); return;
    case 0x25: { int n=getreg8(0); wr16(n*4, DX); wr16(n*4+2, S[rDS]); return; }
    case 0x2A: CX=1998; setreg8(6,7); setreg8(2,8); setreg8(0,3); return;
    case 0x2C: setreg8(5,12); setreg8(1,0); setreg8(6,0); setreg8(2,0); return;
    case 0x2F: S[rES]=(uint16_t)(dta>>4); BX=(uint16_t)(dta&0xF); return;
    case 0x30: setreg8(0,6); setreg8(4,22); BX=0; CX=0; return; /* DOS 6.22 */
    case 0x33: setreg8(2,0); return;
    case 0x35: { int n=getreg8(0); BX=rd16(n*4); S[rES]=rd16(n*4+2); return; }
    case 0x38: set_cf_on_stack(0); return;
    case 0x3B: case 0x3C: set_cf_on_stack(1); AX=5; return;
    case 0x3D: {
        uint32_t a=lin(S[rDS],DX); char nm[256]; int i=0;
        while(i<255){ uint8_t c=rd8(a+i); if(!c) break; nm[i++]=c; }
        nm[i]=0;
        FILE *f = fopen(selfpath,"rb");   /* everything maps to the archive itself */
        if(!f){ set_cf_on_stack(1); AX=2; return; }
        int h=find_handle(); if(h<0){ fclose(f); set_cf_on_stack(1); AX=4; return; }
        handles[h]=f; hpos[h]=0; AX=h; set_cf_on_stack(0);
        if(verbose) fprintf(stderr,"[open '%s' -> h%d]\n",nm,h);
        return; }
    case 0x3E: { int h=BX; if(h>=5&&h<64&&handles[h]){ fclose(handles[h]); handles[h]=NULL; } set_cf_on_stack(0); AX=0; return; }
    case 0x3F: {
        int h=BX; uint32_t a=lin(S[rDS],DX); int n=CX;
        if(h<5||h>=64||!handles[h]){ if(h==0){ AX=0; set_cf_on_stack(0); return;} set_cf_on_stack(1); AX=6; return; }
        static uint8_t buf[65536]; if(n>65536) n=65536;
        size_t got=fread(buf,1,n,handles[h]);
        for(size_t i=0;i<got;i++) wr8(a+i, buf[i]);
        hpos[h]+=got; AX=(uint16_t)got; set_cf_on_stack(0); return; }
    case 0x40: {
        if(verbose) fprintf(stderr,"[write h=%d n=%d]\n",BX,CX);
        int h=BX; uint32_t a=lin(S[rDS],DX); int n=CX;
        if(h==1||h==2){ for(int i=0;i<n;i++) fputc(rd8(a+i),stderr); AX=n; set_cf_on_stack(0); return; }
        AX=n; set_cf_on_stack(0); return; }
    case 0x42: {
        int h=BX; long off=(long)((int32_t)(((uint32_t)CX<<16)|DX));
        if(h<5||h>=64||!handles[h]){ set_cf_on_stack(1); AX=6; return; }
        int wh = getreg8(0)==0?SEEK_SET: getreg8(0)==1?SEEK_CUR:SEEK_END;
        fseek(handles[h],off,wh); long p=ftell(handles[h]); hpos[h]=p;
        DX=(uint16_t)(p>>16); AX=(uint16_t)(p&0xFFFF); set_cf_on_stack(0); return; }
    case 0x44: {
        int al=getreg8(0);
        if(verbose) fprintf(stderr,"[ioctl al=%02X bl=%02X cl=%02X ch=%02X cx=%04X dx=%04X ds=%04X]\n",al,getreg8(3),getreg8(1),getreg8(5),CX,DX,S[rDS]);
        if(al==0x00){ DX = (BX<=2)? 0x80D3 : 0x0000; set_cf_on_stack(0); return; }
        if(al==0x01){ set_cf_on_stack(0); return; }
        if(al==0x08){ AX=0; set_cf_on_stack(0); return; }  /* removable */
        /* Generic IOCTL, AH=44h AL=0Dh. IOCTL — "I/O control" — is the escape
         * hatch every OS ends up with: a call that passes a device-specific
         * command through to a driver, for operations that do not fit the
         * uniform read/write/seek interface. Formatting a track, reading a
         * drive's geometry and ejecting media are all real requests that have
         * no sensible expression as "write these bytes", so they arrive here
         * instead, with the parameters in a packet in memory rather than in
         * registers because there are more of them than registers to hold them.
         *
         * The minor code in CL says which operation, and 0x41 "write track" is
         * the one that matters: it is the path both extractors actually use to
         * put data on the disk, so the memcpy in the loop below is the point at
         * which this entire emulator earns its keep. The packet holds head,
         * cylinder, first sector and count, plus a far pointer to the buffer —
         * assembled from two 16-bit words at offsets 9 and 11 in the usual
         * segment:offset way. */
        if(al==0x0D){ /* generic ioctl */
            uint32_t a=lin(S[rDS],DX);
            int minor = getreg8(1); /* CL */
            if(minor==0x41 || minor==0x61){   /* write/read track on logical drive */
                int head  = rd16(a+1);
                int cyl   = rd16(a+3);
                int first = rd16(a+5);
                int nsec  = rd16(a+7);
                uint32_t buf = lin(rd16(a+11), rd16(a+9));
                int err=0;
                for(int i=0;i<nsec;i++){
                    int sec = first + i;              /* 0-based within track */
                    if(head<0||head>=FLOP_HD||cyl<0||cyl>=FLOP_CYL||sec<0||sec>=FLOP_SPT){ err=1; break; }
                    int lba = (cyl*FLOP_HD + head)*FLOP_SPT + sec;
                    if(minor==0x41){ memcpy(floppy+lba*512, mem+((buf+i*512)&0xFFFFF), 512); written[lba]=1; trap_written++; }
                    else memcpy(mem+((buf+i*512)&0xFFFFF), floppy+lba*512, 512);
                }
                if(verbose){ fprintf(stderr,"[track %s h=%d c=%d s=%d n=%d buf=%05X err=%d]\n", minor==0x41?"W":"R", head,cyl,first,nsec,buf,err);
                  for(int k=0;k<nsec && k<18;k++){ fprintf(stderr,"   s%02d: ",k);
                    for(int j=0;j<16;j++) fprintf(stderr,"%02x ", mem[(buf+k*512+j)&0xFFFFF]);
                    fprintf(stderr," | ");
                    for(int j=0;j<24;j++){ int c=mem[(buf+k*512+j)&0xFFFFF]; fputc(c>=32&&c<127?c:'.',stderr);} fputc('\n',stderr);} }
                if(err){ set_cf_on_stack(1); AX=0x001F; } else set_cf_on_stack(0);
                return;
            }
            if(minor==0x40){ set_cf_on_stack(0); return; }  /* set device parameters: accept */
            if(minor==0x42 || minor==0x62){ set_cf_on_stack(0); return; } /* format/verify track */
            if(minor==0x60){
                for(int i=0;i<64;i++) wr8(a+i,0);
                wr8(a+0,0); wr8(a+1,0x07); /* 1.44M */
                wr16(a+2,0); wr16(a+4,FLOP_CYL);
                wr8(a+6,0);
                /* BPB at offset 7 */
                uint32_t b=a+7;
                wr16(b+0,512); wr8(b+2,1); wr16(b+3,1); wr8(b+5,2); wr16(b+6,224);
                wr16(b+8,2880); wr8(b+10,0xF0); wr16(b+11,9); wr16(b+13,18); wr16(b+15,2);
                set_cf_on_stack(0); return;
            }
            set_cf_on_stack(0); return; }
        set_cf_on_stack(0); return; }
    case 0x43: AX=0x20; CX=0x20; set_cf_on_stack(0); return;
    case 0x47: { uint32_t a=lin(S[rDS],SI); wr8(a,0); set_cf_on_stack(0); return; }
    case 0x48: {
        uint16_t want=BX, got=0;
        if(!mm_alloc(want,&got)){ set_cf_on_stack(1); AX=8; BX=mm_largest(); return; }
        AX=got; set_cf_on_stack(0); return; }
    case 0x49: { mm_free(S[rES]); set_cf_on_stack(0); return; }
    case 0x4A: {
        uint16_t want=BX, maxp=0;
        if(!mm_resize(S[rES],want,&maxp)){ set_cf_on_stack(1); AX=8; BX=maxp; return; }
        set_cf_on_stack(0); return; }
    case 0x4C: halted=1; exitcode=getreg8(0); return;
    case 0x4E: case 0x4F: set_cf_on_stack(1); AX=18; return;
    case 0x50: psp_seg=BX; return;
    case 0x51: case 0x62: BX=psp_seg; return;
    case 0x58: AX=0; set_cf_on_stack(0); return;
    default:
        if(verbose) fprintf(stderr,"[int21 unhandled ah=%02X al=%02X]\n",ah,getreg8(0));
        set_cf_on_stack(0); AX=0; return;
    }
}

/* ---------------------------------------------------------------------------
 * INT 25h / INT 26h — DOS absolute disk read and write
 *
 * A layer above INT 13h: address the disk by logical sector number and let DOS
 * work out the geometry.
 *
 * These two have a famous wart. Every other interrupt handler returns with
 * IRET, which pops the saved flags. These return with a far RET instead, so the
 * flags word the INT pushed is STILL ON THE STACK when the handler returns —
 * the caller is expected to discard it with a POPF or an ADD SP,2. The reason
 * is that this pair predates the convention, and by the time anyone noticed,
 * too much software depended on it to change.
 *
 * That is why this function ends by manually popping the return address and
 * setting CS:IP itself rather than letting the usual IRET stub run: it has to
 * leave one extra word behind. Getting this wrong desynchronises the stack, and
 * a program with a misaligned stack fails in ways that look like anything
 * except a stack problem.
 * ------------------------------------------------------------------------ */
static void int2526(int isread){
    int drive = getreg8(0);            /* AL = drive number, 0 = A: */
    uint32_t nsec = CX;
    uint32_t first = DX;
    uint32_t buf = lin(S[rDS], BX);
    if(nsec == 0xFFFF){                /* extended packet */
        uint32_t p = lin(S[rDS], BX);
        first = rd32(p);
        nsec  = rd16(p+4);
        buf   = lin(rd16(p+8), rd16(p+6));
    }
    int err = 0;
    for(uint32_t i=0;i<nsec;i++){
        uint32_t lba = first + i;
        if(drive > 1 || lba >= (uint32_t)(FLOP_CYL*FLOP_HD*FLOP_SPT)){ err=1; break; }
        if(isread) memcpy(mem + ((buf + i*512) & 0xFFFFF), floppy + lba*512, 512);
        else { memcpy(floppy + lba*512, mem + ((buf + i*512) & 0xFFFFF), 512); written[lba]=1; trap_written++; }
    }
    if(verbose) fprintf(stderr,"[int%02X drive=%d first=%u n=%u err=%d]\n", isread?0x25:0x26, drive, first, nsec, err);
    /* Far-return by hand, deliberately leaving the flags word on the stack.
     * Because we return in the live flags rather than the saved copy, this one
     * sets CF directly instead of calling set_cf_on_stack(). */
    uint16_t rip = pop16();
    uint16_t rcs = pop16();
    S[rCS] = rcs; IPr = rip;
    if(err){ CF=1; AX=0x0207; } else { CF=0; AX=0; }
}

/* ---------------------------------------------------------------------------
 * INTERRUPT DISPATCH
 *
 * Called when the emulated CPU reaches one of the stub handlers planted in the
 * interrupt vector table (see the IVT setup in main()). `n` is the interrupt
 * number. Everything below INT 21h and INT 13h is answered with the smallest
 * plausible lie that keeps the program moving.
 * ------------------------------------------------------------------------ */
static void do_int(int n){
    switch(n){
    case 0x25: int2526(1); return;
    case 0x26: int2526(0); return;
    case 0x13: int13(); return;
    case 0x21: int21(); return;
    case 0x20: halted=1; exitcode=0; return;
    case 0x10: {
        int ah=getreg8(4);
        if(ah==0x0E) fputc(getreg8(0),stderr);
        else if(ah==0x0F){ setreg8(0,3); setreg8(4,80); setreg8(5,0); }
        else if(ah==0x03){ CX=0x0607; DX=0; }
        return; }
    case 0x16: {
        int ah=getreg8(4);
        if(ah==0x00||ah==0x10){ AX=0x1C0D; return; }
        if(ah==0x01||ah==0x11){ ZF=0; AX=0x1C0D; return; }
        AX=0; return; }
    case 0x11: AX=0x0021; return;       /* equipment list: one floppy, 80-col colour */
    case 0x12: AX=640; return;          /* conventional memory size in KB */
    case 0x1A: { int ah=getreg8(4); if(ah==0){ setreg8(0,0); CX=0; DX=0x1234; } return; }  /* fake tick count */

    /* The next three are deliberately refused rather than faked. INT 2Fh is the
     * multiplex interrupt, used among other things to detect an XMS driver;
     * INT 15h AH=87h is the "block move to extended memory" call; INT 67h is
     * EMS. Answering "not present" to all three forces the program to keep its
     * working set in conventional memory, which is the path this emulator
     * actually implements. During debugging it was worth knowing whether the
     * program ever tried any of them — it never did. */
    case 0x2F: if(verbose) fprintf(stderr,"[int2F ax=%04X bx=%04X]\n",AX,BX); AX=0; return;
    case 0x15: if(verbose) fprintf(stderr,"[int15 ax=%04X]\n",AX); set_cf_on_stack(1); setreg8(4,0x86); return;
    case 0x67: if(verbose) fprintf(stderr,"[int67(EMS) ax=%04X]\n",AX); setreg8(4,0x84); return;

    case 0x24: return;    /* critical error handler — never invoked here */
    case 0x23: return;    /* Ctrl-Break handler */
    case 0x1C: return;    /* timer tick hook */
    default:
        if(verbose) fprintf(stderr,"[int %02X ignored]\n", n);
        return;
    }
}

/* ===========================================================================
 * THE INSTRUCTION DISPATCHER
 * ======================================================================== */

/* Bail out loudly on an opcode we do not implement. Dumping the registers is
 * what makes this useful: it tells you where in the program you got to, which
 * combined with a disassembly usually identifies the instruction immediately.
 *
 * Stopping dead rather than ignoring the instruction is a deliberate choice.
 * An emulator that silently skips what it does not understand produces wrong
 * answers instead of error messages, and wrong answers are much harder to
 * track down. As it happens this never fired on these two programs. */
static void unimpl(uint8_t op, uint32_t ip0){
    fprintf(stderr,"UNIMPLEMENTED opcode %02X at %04X:%04X (icount=%lld)\n", op, S[rCS], ip0, icount);
    fprintf(stderr,"AX=%04X BX=%04X CX=%04X DX=%04X SI=%04X DI=%04X BP=%04X SP=%04X DS=%04X ES=%04X SS=%04X\n",
            AX,BX,CX,DX,SI,DI,BP,SP,S[rDS],S[rES],S[rSS]);
    exit(2);
}

/* ---------------------------------------------------------------------------
 * step() — execute exactly one instruction
 *
 * This is the heart of the emulator, and main() does nothing but call it in a
 * loop. One call is one state transition of the machine described in the
 * header: it reads the current registers, flags and memory, and writes back the
 * state they should have afterwards. There are no arguments and no return
 * value, because the state it operates on is the file-level variables at the
 * top — which reads as poor style in ordinary software but is an honest model
 * here, since a real CPU's state genuinely is a fixed set of globals.
 *
 * Structure:
 *
 *   1. reset the per-instruction decode state
 *   2. consume any prefix bytes
 *   3. read the opcode
 *   4. one enormous switch that does what the opcode says
 *
 * The switch is long because x86 is a large, irregular instruction set that
 * grew by accretion from 1978 onward. There is no clever way to shorten it; an
 * emulator for this architecture is mostly a transcription of the manual.
 * ------------------------------------------------------------------------ */
static void step(void){
    /* Per-instruction state, freshly reset. Prefixes apply to exactly one
     * instruction, so forgetting this reset would leak a segment override into
     * the following instruction — a classic emulator bug. */
    segovr=-1; opsz=2; adsz=2; repf=0;

    uint32_t ip0=IPr;    /* remembered only so error messages can report it */
    uint8_t op;

    /* Prefix loop. Prefixes are bytes that modify the instruction that follows;
     * there can be several, in any order, so we keep consuming until we see a
     * byte that is not one. The values are just numbers you have to know:
     *   26/2E/36/3E/64/65  use ES/CS/SS/DS/FS/GS for this memory access
     *   66                 flip operand size (16 <-> 32 bit)
     *   67                 flip address size
     *   F0                 LOCK — meaningless without other CPUs, ignored
     *   F2/F3              REPNE / REP, for the string instructions
     *
     * Note that F2/F3 are only "prefixes" before a string instruction; before
     * anything else the bytes mean other things. We are lenient about that. */
    for(;;){
        op=fetch8();
        if(op==0x26){ segovr=rES; continue; }
        if(op==0x2E){ segovr=rCS; continue; }
        if(op==0x36){ segovr=rSS; continue; }
        if(op==0x3E){ segovr=rDS; continue; }
        if(op==0x64){ segovr=rFS; continue; }
        if(op==0x65){ segovr=rGS; continue; }
        if(op==0x66){ opsz=(opsz==2)?4:2; continue; }
        if(op==0x67){ adsz=(adsz==2)?4:2; continue; }
        if(op==0xF0){ continue; }
        if(op==0xF2){ repf=1; continue; }
        if(op==0xF3){ repf=2; continue; }
        break;
    }
    int w = opsz;

    /* -----------------------------------------------------------------------
     * The arithmetic block: opcodes 0x00-0x3F.
     *
     * This range is beautifully regular, and exploiting that regularity
     * collapses 48 opcodes into a dozen lines. The opcode decomposes as:
     *
     *      bits 5-3   which operation   (ADD OR ADC SBB AND SUB XOR CMP)
     *      bits 2-0   which operand form
     *
     * and the six operand forms are:
     *
     *      0   8-bit    r/m  <- r/m  op  reg
     *      1   16/32    r/m  <- r/m  op  reg
     *      2   8-bit    reg  <- reg  op  r/m
     *      3   16/32    reg  <- reg  op  r/m
     *      4   8-bit    AL   <- AL   op  immediate
     *      5   16/32    AX   <- AX   op  immediate
     *
     * Forms 6 and 7 of each group are not arithmetic at all — those slots hold
     * the segment-register pushes and the packed-decimal instructions — which
     * is what the `(op&7)<6` test excludes, sending them to the main switch.
     *
     * `if(aop!=7)` is the CMP special case: compute flags, discard the result.
     * -------------------------------------------------------------------- */
    if(op<0x40 && (op&7)<6){
        int aop=(op>>3)&7; int form=op&7;
        switch(form){
        case 0: { decode_modrm(); uint32_t a=readrm(1),b=getreg8(reg_); uint32_t r=alu(aop,a,b,1); if(aop!=7) writerm(1,r); } break;
        case 1: { decode_modrm(); uint32_t a=readrm(w),b=getreg(reg_,w); uint32_t r=alu(aop,a,b,w); if(aop!=7) writerm(w,r); } break;
        case 2: { decode_modrm(); uint32_t a=getreg8(reg_),b=readrm(1); uint32_t r=alu(aop,a,b,1); if(aop!=7) setreg8(reg_,r); } break;
        case 3: { decode_modrm(); uint32_t a=getreg(reg_,w),b=readrm(w); uint32_t r=alu(aop,a,b,w); if(aop!=7) setreg(reg_,w,r); } break;
        case 4: { uint32_t b=fetch8(); uint32_t r=alu(aop,getreg8(0),b,1); if(aop!=7) setreg8(0,r); } break;
        case 5: { uint32_t b=fetchw(w); uint32_t r=alu(aop,getreg(0,w),b,w); if(aop!=7) setreg(0,w,r); } break;
        }
        return;
    }
    switch(op){
    case 0x06: push16(S[rES]); return;
    case 0x07: S[rES]=pop16(); return;
    case 0x0E: push16(S[rCS]); return;
    case 0x16: push16(S[rSS]); return;
    case 0x17: S[rSS]=pop16(); return;
    case 0x1E: push16(S[rDS]); return;
    case 0x1F: S[rDS]=pop16(); return;
    /* The packed-decimal (BCD) adjust instructions, 0x27/0x2F/0x37/0x3F. In
     * binary-coded decimal, each decimal digit is stored in its own 4-bit
     * nibble, so 0x42 represents forty-two rather than sixty-six. The appeal is
     * that conversion to text is trivial and decimal fractions such as money
     * are exact, at the cost of wasting bits and needing a fix-up after every
     * ordinary binary add or subtract — which is what these four instructions
     * are. They are implemented here only because they are cheap to implement;
     * neither extractor executes one. */
    case 0x27: { /* DAA */
        uint8_t al=getreg8(0); int cf=CF;
        if(((al&0xF)>9)||AF){ al+=6; AF=1; cf = cf || (al<6); } else AF=0;
        if((getreg8(0)>0x99)||CF){ al+=0x60; CF=1; } else CF=0;
        setreg8(0,al); ZF=al==0; SF=al>>7; PF=parity(al); return; }
    case 0x2F: { /* DAS */
        uint8_t al=getreg8(0), old=al; int cf=CF;
        if(((al&0xF)>9)||AF){ al-=6; AF=1; CF = cf || (old<6); } else AF=0;
        if((old>0x99)||cf){ al-=0x60; CF=1; }
        setreg8(0,al); ZF=al==0; SF=al>>7; PF=parity(al); return; }
    case 0x37: { uint8_t al=getreg8(0);
        if(((al&0xF)>9)||AF){ setreg8(0,(al+6)&0xF); setreg8(4,getreg8(4)+1); AF=1; CF=1; }
        else { setreg8(0,al&0xF); AF=0; CF=0; } return; }
    case 0x3F: { uint8_t al=getreg8(0);
        if(((al&0xF)>9)||AF){ setreg8(0,(al-6)&0xF); setreg8(4,getreg8(4)-1); AF=1; CF=1; }
        else { setreg8(0,al&0xF); AF=0; CF=0; } return; }
    /* Opcodes 0x40-0x5F: INC, DEC, PUSH and POP with the register number
     * packed into the low 3 bits of the opcode byte itself. Eight registers x
     * four operations = 32 single-byte instructions, which is the density
     * argument for variable-length encoding made concrete: `PUSH BP` is one
     * byte, and in code that is mostly moving things on and off the stack that
     * matters. `op&7` is the field extraction that recovers the register. */
    case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45: case 0x46: case 0x47:
        setreg(op&7,w,op_inc(getreg(op&7,w),w)); return;
    case 0x48: case 0x49: case 0x4A: case 0x4B: case 0x4C: case 0x4D: case 0x4E: case 0x4F:
        setreg(op&7,w,op_dec(getreg(op&7,w),w)); return;
    case 0x50: case 0x51: case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: case 0x57:
        push16(getreg(op&7,2)); return;
    case 0x58: case 0x59: case 0x5A: case 0x5B: case 0x5C: case 0x5D: case 0x5E: case 0x5F:
        setreg(op&7,2,pop16()); return;
    case 0x60: { uint16_t sp=SP; push16(AX);push16(CX);push16(DX);push16(BX);push16(sp);push16(BP);push16(SI);push16(DI); return; }
    case 0x61: { DI=pop16();SI=pop16();BP=pop16();(void)pop16();BX=pop16();DX=pop16();CX=pop16();AX=pop16(); return; }
    case 0x62: decode_modrm(); (void)readrm(w); return; /* BOUND: ignore */
    case 0x68: push16(fetchw(w)); return;
    case 0x69: { decode_modrm(); uint32_t a=readrm(w); int32_t b=(w==2)?(int16_t)fetch16():(int32_t)fetch32();
                 int32_t av=(w==2)?(int16_t)a:(int32_t)a; int64_t r=(int64_t)av*b;
                 setreg(reg_,w,(uint32_t)r); CF=OF=((int64_t)(int32_t)(uint32_t)r != r); return; }
    case 0x6A: push16((int8_t)fetch8()); return;
    case 0x6B: { decode_modrm(); uint32_t a=readrm(w); int32_t b=(int8_t)fetch8();
                 int32_t av=(w==2)?(int16_t)a:(int32_t)a; int64_t r=(int64_t)av*b;
                 setreg(reg_,w,(uint32_t)r); CF=OF=((int64_t)(int32_t)(uint32_t)r != r); return; }
    /* The conditional jumps, 0x70-0x7F. This is how every if, while and for in
     * the emulated program ultimately gets expressed, and in a decompressor
     * inner loop it is one of the two or three most executed instructions.
     *
     * Structure: the low 4 bits of the opcode select the condition. Bits 3-1
     * pick which flag combination to test, and bit 0 inverts it — so "jump if
     * zero" and "jump if not zero" are adjacent opcodes sharing one test, which
     * is what `if(c&1) t=!t;` exploits. The four signed comparisons (SF!=OF,
     * and that or-ed with ZF) look odd until you notice that a signed
     * less-than is exactly "the sign of the result disagrees with the sign the
     * true answer should have had", which is what the overflow flag records.
     *
     * The operand is a *relative* displacement: a signed byte added to IP,
     * reaching -128..+127 bytes from the instruction after the jump. Relative
     * addressing costs one byte instead of two and makes a block of code
     * position-independent — it can be loaded anywhere without fixing up its
     * internal branches, which is why the relocation table in main() only has
     * to patch segment values and never ordinary jumps. */
    case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x76: case 0x77:
    case 0x78: case 0x79: case 0x7A: case 0x7B: case 0x7C: case 0x7D: case 0x7E: case 0x7F: {
        int8_t d=(int8_t)fetch8(); int c=op&0xF, t=0;
        switch(c>>1){
        case 0: t=OF; break; case 1: t=CF; break; case 2: t=ZF; break;
        case 3: t=CF||ZF; break; case 4: t=SF; break; case 5: t=PF; break;
        case 6: t=(SF!=OF); break; case 7: t=(SF!=OF)||ZF; break; }
        if(c&1) t=!t;
        if(t) IPr=(IPr+d)&0xFFFF; return; }
    /* Opcodes 0x80-0x83, the "immediate group": arithmetic between a register
     * or memory operand and a constant embedded in the instruction. Note what
     * `reg_` is used for here — not a register, but as an extension of the
     * opcode, selecting which of the eight ALU operations this is. The
     * encoding space ran out, so a three-bit field that has no other job for
     * these opcodes was borrowed to carry more opcode. Several groups below
     * (0xC0, 0xD0-0xD3, 0xF6/0xF7, 0xFE, 0xFF) work the same way, which is why
     * they all read `int sub=reg_;` and switch on it.
     *
     * 0x83 is a nice space optimisation: a one-byte signed immediate that is
     * sign-extended to the operand width, so `ADD SI, -1` and `CMP AX, 4` cost
     * one byte of constant instead of two. */
    case 0x80: case 0x82: { decode_modrm(); int aop=reg_; uint32_t a=readrm(1); uint32_t b=fetch8();
                 uint32_t r=alu(aop,a,b,1); if(aop!=7) writerm(1,r); return; }
    case 0x81: { decode_modrm(); int aop=reg_; uint32_t a=readrm(w); uint32_t b=fetchw(w);
                 uint32_t r=alu(aop,a,b,w); if(aop!=7) writerm(w,r); return; }
    case 0x83: { decode_modrm(); int aop=reg_; uint32_t a=readrm(w); uint32_t b=(uint32_t)(int32_t)(int8_t)fetch8()&msk(w);
                 uint32_t r=alu(aop,a,b,w); if(aop!=7) writerm(w,r); return; }
    case 0x84: { decode_modrm(); uint32_t r=readrm(1)&getreg8(reg_); setlogic(r,1); return; }
    case 0x85: { decode_modrm(); uint32_t r=readrm(w)&getreg(reg_,w); setlogic(r,w); return; }
    case 0x86: { decode_modrm(); uint32_t a=readrm(1),b=getreg8(reg_); writerm(1,b); setreg8(reg_,a); return; }
    case 0x87: { decode_modrm(); uint32_t a=readrm(w),b=getreg(reg_,w); writerm(w,b); setreg(reg_,w,a); return; }
    case 0x88: decode_modrm(); writerm(1,getreg8(reg_)); return;
    case 0x89: decode_modrm(); writerm(w,getreg(reg_,w)); return;
    case 0x8A: decode_modrm(); setreg8(reg_,readrm(1)); return;
    case 0x8B: decode_modrm(); setreg(reg_,w,readrm(w)); return;
    case 0x8C: decode_modrm(); { uint16_t v=S[reg_&7]; if(ismem_) wr16(addr_,v); else setreg(rm_,2,v);} return;
    /* LEA — load effective address. It runs the full address computation of a
     * memory operand and then, instead of touching memory, stores the address
     * itself. `LEA AX,[BX+SI+4]` means AX = BX + SI + 4. Compilers love it as a
     * free three-input add, and it is the reason off_ is kept separately from
     * addr_ during decoding: LEA wants the offset before the segment is
     * folded in. */
    case 0x8D: decode_modrm(); setreg(reg_,w,off_); return;
    case 0x8E: decode_modrm(); S[reg_&7]=readrm(2); return;
    case 0x8F: decode_modrm(); writerm(2,pop16()); return;
    case 0x90: return;
    case 0x91: case 0x92: case 0x93: case 0x94: case 0x95: case 0x96: case 0x97: {
        uint32_t t=getreg(0,w); setreg(0,w,getreg(op&7,w)); setreg(op&7,w,t); return; }
    case 0x98: if(w==2) AX=(int16_t)(int8_t)getreg8(0); else R[0]=(int32_t)(int16_t)AX; return;
    case 0x99: if(w==2) DX=(AX&0x8000)?0xFFFF:0; else R[2]=(R[0]&0x80000000u)?0xFFFFFFFFu:0; return;
    case 0x9A: { uint16_t noff=fetch16(), nseg=fetch16(); push16(S[rCS]); push16(IPr); S[rCS]=nseg; IPr=noff; return; }
    case 0x9B: return;
    case 0x9C: push16(getflags()); return;
    case 0x9D: setflags(pop16()); return;
    case 0x9E: { uint8_t ahv=getreg8(4); CF=ahv&1; PF=(ahv>>2)&1; AF=(ahv>>4)&1; ZF=(ahv>>6)&1; SF=(ahv>>7)&1; return; }
    case 0x9F: setreg8(4, (getflags()&0xFF)|2); return;
    case 0xA0: { uint16_t o=fetch16(); setreg8(0, rd8(lin(S[seg_for(rDS)],o))); return; }
    case 0xA1: { uint16_t o=fetch16(); setreg(0,w, w==2?rd16(lin(S[seg_for(rDS)],o)):rd32(lin(S[seg_for(rDS)],o))); return; }
    case 0xA2: { uint16_t o=fetch16(); wr8(lin(S[seg_for(rDS)],o), getreg8(0)); return; }
    case 0xA3: { uint16_t o=fetch16(); if(w==2) wr16(lin(S[seg_for(rDS)],o),AX); else wr32(lin(S[seg_for(rDS)],o),R[0]); return; }
    /* ----------------------------------------------------------------------
     * The string instructions, 0xA4-0xAF: MOVS (copy), CMPS (compare), STOS
     * (fill), LODS (load), SCAS (scan). Each one processes a single element
     * between the address in SI and/or the address in DI, then advances both
     * pointers by the element size — forwards or backwards according to the
     * direction flag DF.
     *
     * What makes them interesting is the REP prefix. With REP in front, the
     * instruction repeats CX times, decrementing CX itself, so a whole copy
     * loop is one instruction: `REP MOVSB` is memcpy, `REP STOSW` is memset,
     * `REPNE SCASB` is strlen. The loop lives in the processor's microcode
     * rather than in the instruction stream, so the loop overhead — the
     * decrement, the test, the branch, and the re-fetch of the loop body —
     * disappears. It is the closest this instruction set gets to a bulk
     * operation on an array, and the reason DOS-era code copies memory with a
     * two-byte instruction rather than a loop.
     *
     * The C below is written as do/while for MOVS/STOS/LODS and as an explicit
     * for(;;) for CMPS/SCAS because the comparing forms have an extra exit
     * condition: they stop early when the comparison succeeds (REPE) or fails
     * (REPNE), which is what makes a search possible. `repf` carries which
     * prefix was seen: 1 = REPNE, 2 = REPE/REP.
     * ------------------------------------------------------------------- */
    case 0xA4: case 0xA5: { /* MOVS */
        int sz=(op==0xA4)?1:w; int d=DFf?-sz:sz;
        do {
            if(repf && CX==0) break;
            uint32_t s=lin(S[seg_for(rDS)],SI), dd=lin(S[rES],DI);
            if(sz==1) wr8(dd,rd8(s)); else if(sz==2) wr16(dd,rd16(s)); else wr32(dd,rd32(s));
            SI+=d; DI+=d;
            if(repf) CX--;
        } while(repf && CX);
        return; }
    case 0xA6: case 0xA7: { /* CMPS */
        int sz=(op==0xA6)?1:w; int d=DFf?-sz:sz;
        if(repf && CX==0) return;
        for(;;){
            uint32_t s=lin(S[seg_for(rDS)],SI), dd=lin(S[rES],DI);
            uint32_t a = sz==1?rd8(s): sz==2?rd16(s):rd32(s);
            uint32_t b = sz==1?rd8(dd): sz==2?rd16(dd):rd32(dd);
            op_sub(a,b,sz,0);
            SI+=d; DI+=d;
            if(!repf) break;
            CX--;
            if(CX==0) break;
            if(repf==2 && !ZF) break;
            if(repf==1 && ZF) break;
        }
        return; }
    case 0xA8: { uint32_t r=getreg8(0)&fetch8(); setlogic(r,1); return; }
    case 0xA9: { uint32_t r=getreg(0,w)&fetchw(w); setlogic(r,w); return; }
    case 0xAA: case 0xAB: { /* STOS */
        int sz=(op==0xAA)?1:w; int d=DFf?-sz:sz;
        do {
            if(repf && CX==0) break;
            uint32_t dd=lin(S[rES],DI);
            if(sz==1) wr8(dd,getreg8(0)); else if(sz==2) wr16(dd,AX); else wr32(dd,R[0]);
            DI+=d;
            if(repf) CX--;
        } while(repf && CX);
        return; }
    case 0xAC: case 0xAD: { /* LODS */
        int sz=(op==0xAC)?1:w; int d=DFf?-sz:sz;
        do {
            if(repf && CX==0) break;
            uint32_t s=lin(S[seg_for(rDS)],SI);
            if(sz==1) setreg8(0,rd8(s)); else if(sz==2) AX=rd16(s); else R[0]=rd32(s);
            SI+=d;
            if(repf) CX--;
        } while(repf && CX);
        return; }
    case 0xAE: case 0xAF: { /* SCAS */
        int sz=(op==0xAE)?1:w; int d=DFf?-sz:sz;
        if(repf && CX==0) return;
        for(;;){
            uint32_t dd=lin(S[rES],DI);
            uint32_t a=getreg(0,sz);
            uint32_t b= sz==1?rd8(dd): sz==2?rd16(dd):rd32(dd);
            op_sub(a,b,sz,0);
            DI+=d;
            if(!repf) break;
            CX--;
            if(CX==0) break;
            if(repf==2 && !ZF) break;
            if(repf==1 && ZF) break;
        }
        return; }
    case 0xB0: case 0xB1: case 0xB2: case 0xB3: case 0xB4: case 0xB5: case 0xB6: case 0xB7:
        setreg8(op&7, fetch8()); return;
    case 0xB8: case 0xB9: case 0xBA: case 0xBB: case 0xBC: case 0xBD: case 0xBE: case 0xBF:
        setreg(op&7,w,fetchw(w)); return;
    case 0xC0: { decode_modrm(); int sop=reg_; uint32_t v=readrm(1); int c=fetch8(); writerm(1,do_shift(sop,v,c,1)); return; }
    case 0xC1: { decode_modrm(); int sop=reg_; uint32_t v=readrm(w); int c=fetch8(); writerm(w,do_shift(sop,v,c,w)); return; }
    /* RET n — return, then discard n bytes of arguments from the stack. This
     * is the callee-cleans-up calling convention (Pascal/stdcall) described in
     * THE STACK: the caller pushed the arguments, and the callee removes them
     * on the way out, saving an instruction at every call site. The C
     * convention instead has the caller remove them, which costs a little more
     * but is what makes a variadic function like printf possible, since only
     * the caller knows how many arguments it actually pushed. */
    case 0xC2: { uint16_t n=fetch16(); IPr=pop16(); SP+=n; return; }
    case 0xC3: IPr=pop16(); return;
    case 0xC4: { decode_modrm(); uint32_t a=addr_; setreg(reg_,2,rd16(a)); S[rES]=rd16(a+2); return; }
    case 0xC5: { decode_modrm(); uint32_t a=addr_; setreg(reg_,2,rd16(a)); S[rDS]=rd16(a+2); return; }
    case 0xC6: decode_modrm(); { uint8_t v=fetch8(); writerm(1,v); } return;
    case 0xC7: decode_modrm(); { uint32_t v=fetchw(w); writerm(w,v); } return;
    /* ENTER and LEAVE — the stack frame prologue and epilogue as single
     * instructions. ENTER sz,0 is exactly the sequence from the frame diagram
     * in THE STACK: push the caller's BP, point BP at the saved copy, then
     * subtract sz from SP to reserve sz bytes of local variables. LEAVE undoes
     * it: SP=BP throws away the locals in one move, then pop restores the
     * caller's BP. The `lvl` argument to ENTER copies a chain of enclosing
     * frames' pointers into the new frame, which supports languages with
     * nested procedures that can see an outer procedure's locals (Pascal, Ada);
     * C has no such construct, so lvl is essentially always 0. */
    case 0xC8: { uint16_t sz=fetch16(); uint8_t lvl=fetch8(); push16(BP); uint16_t fp=SP;
                 for(int i=1;i<lvl;i++){ BP-=2; push16(rd16(lin(S[rSS],BP))); }
                 if(lvl>0) push16(fp);
                 BP=fp; SP-=sz; return; }
    case 0xC9: SP=BP; BP=pop16(); return;
    case 0xCA: { uint16_t n=fetch16(); IPr=pop16(); S[rCS]=pop16(); SP+=n; return; }
    case 0xCB: IPr=pop16(); S[rCS]=pop16(); return;
    case 0xCC: push16(getflags()); push16(S[rCS]); push16(IPr); { uint16_t o=rd16(3*4),s=rd16(3*4+2); S[rCS]=s; IPr=o; } return;
    /* INT n — the system call instruction, and the doorway described at THE
     * OPERATING SYSTEM. Its three pushes are why an interrupt handler can be
     * entered from anywhere and resume the interrupted code exactly: the flags
     * are saved because the handler will certainly disturb them, and CS:IP is
     * saved as the far return address. Then the CPU reads the two words at
     * address n*4 — entry n of the interrupt vector table at the very bottom of
     * memory — and jumps there. Note that the vector table is ordinary
     * writable memory, so "installing a handler" is a store, and hooking an
     * existing one is: read the old vector, save it, write yours, and jump to
     * the old one when you are finished. IFf=0 disables further interrupts on
     * entry, matching the hardware. */
    case 0xCD: { int n=fetch8(); push16(getflags()); push16(S[rCS]); push16(IPr);
                 uint16_t o=rd16(n*4), s=rd16(n*4+2); S[rCS]=s; IPr=o; IFf=0; TFf=0; return; }
    case 0xCE: return;
    case 0xCF: { IPr=pop16(); S[rCS]=pop16(); setflags(pop16()); return; }
    case 0xD0: { decode_modrm(); int sop=reg_; writerm(1,do_shift(sop,readrm(1),1,1)); return; }
    case 0xD1: { decode_modrm(); int sop=reg_; writerm(w,do_shift(sop,readrm(w),1,w)); return; }
    case 0xD2: { decode_modrm(); int sop=reg_; writerm(1,do_shift(sop,readrm(1),getreg8(1),1)); return; }
    case 0xD3: { decode_modrm(); int sop=reg_; writerm(w,do_shift(sop,readrm(w),getreg8(1),w)); return; }
    case 0xD4: { uint8_t b=fetch8(); if(!b){ /* div by zero */ } else { uint8_t al=getreg8(0); setreg8(4,al/b); setreg8(0,al%b);}
                 uint8_t al=getreg8(0); ZF=al==0; SF=al>>7; PF=parity(al); return; }
    case 0xD5: { uint8_t b=fetch8(); uint8_t al=getreg8(0)+getreg8(4)*b; setreg8(0,al); setreg8(4,0);
                 ZF=al==0; SF=al>>7; PF=parity(al); return; }
    case 0xD6: setreg8(0, CF?0xFF:0x00); return; /* SALC */
    /* XLAT — a one-byte table lookup: AL = memory[BX + AL]. An entire indexed
     * gather in a single instruction, which is what a byte-oriented translation
     * table (character set conversion, or a decoder's symbol table) wants. */
    case 0xD7: { uint32_t a=lin(S[seg_for(rDS)], BX+getreg8(0)); setreg8(0, rd8(a)); return; }
    case 0xD8: case 0xD9: case 0xDA: case 0xDB: case 0xDC: case 0xDD: case 0xDE: case 0xDF:
        decode_modrm(); return; /* FPU: ignore */
    /* LOOP and its variants, 0xE0-0xE3: decrement CX and branch back if it is
     * not yet zero (0xE0/0xE1 additionally require the zero flag to be clear or
     * set, for a loop with an early exit). A counted loop in two bytes, with CX
     * hardwired as the counter — another instance of registers having assigned
     * roles rather than being interchangeable. 0xE3 is the odd one out: JCXZ
     * tests CX without decrementing, used to skip a REP that would otherwise
     * run 65536 times when CX happens to be zero. */
    case 0xE0: { int8_t d=(int8_t)fetch8(); CX--; if(CX!=0 && !ZF) IPr=(IPr+d)&0xFFFF; return; }
    case 0xE1: { int8_t d=(int8_t)fetch8(); CX--; if(CX!=0 && ZF) IPr=(IPr+d)&0xFFFF; return; }
    case 0xE2: { int8_t d=(int8_t)fetch8(); CX--; if(CX!=0) IPr=(IPr+d)&0xFFFF; return; }
    case 0xE3: { int8_t d=(int8_t)fetch8(); if(CX==0) IPr=(IPr+d)&0xFFFF; return; }
    /* IN and OUT, 0xE4-0xE7 and 0xEC-0xEF. x86 has a second, separate address
     * space of 65536 "I/O ports" that hardware registers live in, reached only
     * by these instructions — a device is not a range of memory addresses here
     * but a port number you read and write. There is no hardware in this
     * emulator, so a read returns all-ones (which is what an absent device
     * looks like on a real bus, since nothing pulls the lines low) and a write
     * is discarded. The `fetch8()` consumes the port number that the immediate
     * forms carry, which must be done even though the value is unused, because
     * skipping it would leave IP pointing into the middle of the instruction. */
    case 0xE4: { fetch8(); setreg8(0,0xFF); return; }
    case 0xE5: { fetch8(); setreg(0,w,0xFFFF); return; }
    case 0xE6: { fetch8(); return; }
    case 0xE7: { fetch8(); return; }
    /* CALL (relative) — and here, in one line, is the entire function-call
     * mechanism described at THE STACK. `push16(IPr)` saves the return address:
     * IPr has already been advanced past this instruction by fetch16(), so it
     * holds the address of the next instruction, which is exactly where control
     * should resume. Then IP moves to the target. The matching RET at 0xC3 is
     * `IPr=pop16()` and nothing else. Two pushes and two pops (0x9A and 0xCB)
     * do the same for a *far* call, one that changes CS as well and so can
     * reach outside the current 64 KB code segment. */
    case 0xE8: { int16_t d=(int16_t)fetch16(); push16(IPr); IPr=(IPr+d)&0xFFFF; return; }
    case 0xE9: { int16_t d=(int16_t)fetch16(); IPr=(IPr+d)&0xFFFF; return; }
    case 0xEA: { uint16_t o=fetch16(), s=fetch16(); S[rCS]=s; IPr=o; return; }
    case 0xEB: { int8_t d=(int8_t)fetch8(); IPr=(IPr+d)&0xFFFF; return; }
    case 0xEC: setreg8(0,0xFF); return;
    case 0xED: setreg(0,w,0xFFFF); return;
    case 0xEE: case 0xEF: return;
    case 0xF1: { /* our INT callback marker */
        int n=fetch8(); do_int(n); return; }
    case 0xF4: halted=1; return;
    case 0xF5: CF=!CF; return;
    /* Group 0xF6/0xF7 — TEST, NOT, NEG, MUL, IMUL, DIV, IDIV, selected by the
     * reg field of the ModR/M byte as an opcode extension.
     *
     * Multiply and divide are where the implicit-operand style of this
     * instruction set is most visible. MUL takes one explicit operand and
     * multiplies it by AL/AX/EAX without being told to, and the product is
     * twice as wide as the inputs, so it is deposited in a register *pair*:
     * a 16x16 multiply puts its 32-bit result in DX:AX, high half in DX.
     * Divide runs the same way in reverse — it takes a double-width dividend
     * from DX:AX, and leaves the quotient in AX and the remainder in DX,
     * getting two results out of one instruction.
     *
     * The `if(!v){do_int(0);break;}` cases are divide-by-zero and quotient
     * overflow, which on this machine are not error codes but a *trap*: the CPU
     * itself raises interrupt 0, transferring control through the vector table
     * exactly as a software INT would. That is the ancestor of the hardware
     * exception that becomes SIGFPE or a divide-by-zero exception today. */
    case 0xF6: case 0xF7: {
        int sz=(op==0xF6)?1:w;
        decode_modrm(); int sub=reg_;
        uint32_t v=readrm(sz);
        switch(sub){
        case 0: case 1: { uint32_t imm=fetchw(sz); setlogic(v&imm,sz); break; }
        case 2: writerm(sz, (~v)&msk(sz)); break;
        case 3: { uint32_t r=op_sub(0,v,sz,0); writerm(sz,r); break; }
        case 4: { /* MUL */
            if(sz==1){ uint16_t r=(uint16_t)getreg8(0)*(uint8_t)v; AX=r; CF=OF=(getreg8(4)!=0); }
            else if(sz==2){ uint32_t r=(uint32_t)AX*(uint16_t)v; AX=r&0xFFFF; DX=r>>16; CF=OF=(DX!=0); }
            else { uint64_t r=(uint64_t)R[0]*v; R[0]=(uint32_t)r; R[2]=(uint32_t)(r>>32); CF=OF=(R[2]!=0);} break; }
        case 5: { /* IMUL */
            if(sz==1){ int16_t r=(int16_t)(int8_t)getreg8(0)*(int8_t)v; AX=(uint16_t)r; CF=OF=(((int16_t)(int8_t)(r&0xFF))!=r); }
            else if(sz==2){ int32_t r=(int32_t)(int16_t)AX*(int16_t)v; AX=r&0xFFFF; DX=(r>>16)&0xFFFF; CF=OF=(((int32_t)(int16_t)(r&0xFFFF))!=r); }
            else { int64_t r=(int64_t)(int32_t)R[0]*(int32_t)v; R[0]=(uint32_t)r; R[2]=(uint32_t)(r>>32); CF=OF=(((int64_t)(int32_t)(uint32_t)r)!=r);} break; }
        case 6: { /* DIV */
            if(sz==1){ if(!v){do_int(0);break;} uint16_t d=AX; if((d/v)>0xFF){do_int(0);break;} setreg8(0,d/v); setreg8(4,d%v); }
            else if(sz==2){ if(!v){do_int(0);break;} uint32_t d=((uint32_t)DX<<16)|AX; if((d/v)>0xFFFF){do_int(0);break;} AX=d/v; DX=d%v; }
            else { if(!v){do_int(0);break;} uint64_t d=((uint64_t)R[2]<<32)|R[0]; R[0]=(uint32_t)(d/v); R[2]=(uint32_t)(d%v);} break; }
        case 7: { /* IDIV */
            if(sz==1){ int8_t dv=(int8_t)v; if(!dv){do_int(0);break;} int16_t d=(int16_t)AX; setreg8(0,(uint8_t)(d/dv)); setreg8(4,(uint8_t)(d%dv)); }
            else if(sz==2){ int16_t dv=(int16_t)v; if(!dv){do_int(0);break;} int32_t d=(int32_t)(((uint32_t)DX<<16)|AX); AX=(uint16_t)(d/dv); DX=(uint16_t)(d%dv); }
            else { int32_t dv=(int32_t)v; if(!dv){do_int(0);break;} int64_t d=(int64_t)(((uint64_t)R[2]<<32)|R[0]); R[0]=(uint32_t)(d/dv); R[2]=(uint32_t)(d%dv);} break; }
        }
        return; }
    case 0xF8: CF=0; return;
    case 0xF9: CF=1; return;
    case 0xFA: IFf=0; return;
    case 0xFB: IFf=1; return;
    case 0xFC: DFf=0; return;
    case 0xFD: DFf=1; return;
    case 0xFE: { decode_modrm(); uint32_t v=readrm(1);
                 writerm(1, reg_==0? op_inc(v,1): op_dec(v,1)); return; }
    case 0xFF: { decode_modrm(); int sub=reg_;
        switch(sub){
        case 0: writerm(w, op_inc(readrm(w),w)); break;
        case 1: writerm(w, op_dec(readrm(w),w)); break;
        case 2: { uint16_t t=readrm(2); push16(IPr); IPr=t; break; }
        case 3: { uint16_t o=rd16(addr_), s=rd16(addr_+2); push16(S[rCS]); push16(IPr); S[rCS]=s; IPr=o; break; }
        case 4: IPr=readrm(2); break;
        case 5: { uint16_t o=rd16(addr_), s=rd16(addr_+2); S[rCS]=s; IPr=o; break; }
        case 6: push16(readrm(2)); break;
        default: unimpl(op,ip0);
        }
        return; }
    /* The 0x0F escape — how the instruction set grew after it ran out of room.
     * All 256 one-byte opcodes were spoken for by the mid-1980s, so every
     * instruction added from the 286 and 386 onward is encoded as 0x0F followed
     * by a second opcode byte, doubling the available space at a cost of one
     * byte per instruction. The handful decoded here are the ones a 1998
     * compiler might plausibly have emitted: the long-displacement conditional
     * jumps (0x80-0x8F, which lift the +/-127 byte limit of the short forms),
     * SETcc (0x90-0x9F, which materialises a condition as a 0 or 1 value in a
     * register instead of branching on it), MOVZX/MOVSX (0xB6-0xBF, widening a
     * byte or word to a larger register with zero- or sign-extension), the FS
     * and GS segment pushes, and a two-operand IMUL.
     *
     * 0x0F31 is RDTSC, "read time stamp counter", which on real hardware
     * returns the processor's cycle count. Here it returns icount — the number
     * of instructions this emulator has executed — which is a perfectly
     * reasonable answer for a machine whose only notion of time is how many
     * state transitions have happened, and which keeps the run deterministic. */
    case 0x0F: {
        uint8_t o2=fetch8();
        if(o2>=0x80 && o2<=0x8F){ int16_t d=(int16_t)fetch16(); int c=o2&0xF,t=0;
            switch(c>>1){ case 0:t=OF;break; case 1:t=CF;break; case 2:t=ZF;break; case 3:t=CF||ZF;break;
                          case 4:t=SF;break; case 5:t=PF;break; case 6:t=(SF!=OF);break; case 7:t=(SF!=OF)||ZF;break; }
            if(c&1) t=!t; if(t) IPr=(IPr+d)&0xFFFF; return; }
        if(o2>=0x90 && o2<=0x9F){ decode_modrm(); int c=o2&0xF,t=0;
            switch(c>>1){ case 0:t=OF;break; case 1:t=CF;break; case 2:t=ZF;break; case 3:t=CF||ZF;break;
                          case 4:t=SF;break; case 5:t=PF;break; case 6:t=(SF!=OF);break; case 7:t=(SF!=OF)||ZF;break; }
            if(c&1) t=!t; writerm(1,t?1:0); return; }
        if(o2==0xB6){ decode_modrm(); setreg(reg_,w,readrm(1)); return; }
        if(o2==0xB7){ decode_modrm(); setreg(reg_,w,readrm(2)); return; }
        if(o2==0xBE){ decode_modrm(); setreg(reg_,w,(uint32_t)(int32_t)(int8_t)readrm(1)); return; }
        if(o2==0xBF){ decode_modrm(); setreg(reg_,w,(uint32_t)(int32_t)(int16_t)readrm(2)); return; }
        if(o2==0xA0){ push16(S[rFS]); return; }
        if(o2==0xA1){ S[rFS]=pop16(); return; }
        if(o2==0xA8){ push16(S[rGS]); return; }
        if(o2==0xA9){ S[rGS]=pop16(); return; }
        if(o2==0xAF){ decode_modrm(); uint32_t b=readrm(w);
            if(w==2){ int32_t r=(int32_t)(int16_t)getreg(reg_,2)*(int16_t)b; setreg(reg_,2,r); CF=OF=((int32_t)(int16_t)(r&0xFFFF)!=r); }
            else { int64_t r=(int64_t)(int32_t)getreg(reg_,4)*(int32_t)b; setreg(reg_,4,(uint32_t)r); CF=OF=((int64_t)(int32_t)(uint32_t)r!=r); }
            return; }
        if(o2==0x31){ R[0]=(uint32_t)icount; R[2]=0; return; }
        unimpl(0x0F,ip0); return; }
    default:
        unimpl(op,ip0);
    }
}

/* ===========================================================================
 * A FILESYSTEM AS A DATA STRUCTURE: FAT12
 *
 * Lay down a blank but validly formatted 1.44 MB FAT12 filesystem.
 *
 * This is needed because the extractor refuses to write to an unformatted
 * disk — it reads the boot sector first and checks the geometry looks sane.
 * The whole thing gets overwritten moments later by the real image, so only the
 * fields the extractor inspects have to be right, but it is little extra work
 * to write a genuinely correct one.
 *
 * WHAT A FILESYSTEM IS
 *
 * The disk, as established at THE VIRTUAL FLOPPY DRIVE, is an array of 2880
 * numbered 512-byte blocks and nothing else. It has no concept of a file. A
 * filesystem is a data structure written into that array which adds three
 * things: names, variable length, and a record of which blocks are in use.
 * Designing one is an exercise in laying out a data structure when your entire
 * address space is "block number", your pointers are integers, and any update
 * may be interrupted by someone ejecting the disk.
 *
 * THE LAYOUT, in order from block 0:
 *
 *   boot sector      1 sector   — a jump instruction, then the BPB describing
 *                                 the geometry, then boot code
 *   FAT              9 sectors  — the allocation table (see below)
 *   FAT copy         9 sectors  — an identical spare, for resilience
 *   root directory  14 sectors  — 224 fixed-size 32-byte entries
 *   data            the rest    — 2847 sectors of file contents
 *
 * THE BPB (BIOS Parameter Block) is the self-description at offset 11 of the
 * boot sector: bytes per sector, sectors per cluster, how many FATs, how many
 * root entries, total sectors, sectors per FAT, sectors per track, heads. It is
 * the header record that lets one piece of code handle every FAT disk from a
 * 160 KB single-sided floppy to a hard disk partition — the on-disk equivalent
 * of a schema stored alongside the data rather than hard-coded into the reader.
 *
 * THE FAT ITSELF is the interesting part, and it is a genuinely nice piece of
 * data-structure design. Divide the data area into CLUSTERS, the unit of
 * allocation (here one cluster is one sector, so cluster and sector coincide;
 * on a hard disk a cluster is several sectors, trading wasted space at the end
 * of each file for a smaller table). Now build an array with one entry per
 * cluster. The entry for cluster i holds:
 *
 *      0            this cluster is free
 *      2 .. 0xFEF   the number of the NEXT cluster of the same file
 *      0xFF7        this cluster is defective, never use it
 *      0xFF8..0xFFF end of chain — this is the file's last cluster
 *
 * So the File Allocation Table is a LINKED LIST STORED AS AN ARRAY OF INDICES.
 * Instead of each node carrying a pointer next to its payload, all the "next"
 * fields are pulled out into one dense array, and the payload — the actual file
 * data — sits in a parallel array (the data area) indexed the same way. A
 * directory entry supplies the head of the chain, and reading a file is the
 * loop any linked-list traversal is:
 *
 *      c = directory_entry.first_cluster;
 *      while (c < 0xFF8) { read_data_sector(c); c = fat[c]; }
 *
 * Two consequences worth drawing out, because they are the sort of thing that
 * shows up as a performance property of the whole system. First, the chain
 * array is small and separate from the data, so it can be cached in RAM in its
 * entirety — 4.5 KB for a whole floppy — and the OS can then answer "which
 * blocks does this file occupy" without touching the disk at all. That is the
 * design's main virtue on hardware where a seek cost tens of milliseconds.
 * Second, the access path is inherently sequential: to reach byte 900,000 of a
 * file you must walk every link that precedes it, so random access is O(n) in
 * the file's length. That is the design's main vice, and it is why later
 * filesystems moved to extents (store a start and a run length, so contiguous
 * regions cost one record) and to B-trees for directories.
 *
 * TWELVE-BIT ENTRIES are what the "12" means, and they are the awkward part of
 * the format: a 2880-cluster disk needs 2880 x 1.5 = 4320 bytes = 9 sectors of
 * table, which is why the BPB says 9 sectors per FAT. Two entries share three
 * bytes, and unpacking one requires knowing whether its index is odd or even —
 * the packed-field idiom from getreg8(), taken to an inconvenient extreme in
 * exchange for saving 1.4 KB. The companion script fat12_extract.py in this
 * directory does that unpacking if you want to see it.
 *
 * TWO COPIES of the FAT are kept because the table is the one structure whose
 * loss destroys everything: the data blocks would all still be present but no
 * longer joined into files. Duplicating it is the cheapest possible resilience
 * measure, and DOS wrote both copies on every update.
 *
 * The magic numbers below are the standard 1.44 MB values. 0xF0 is the media
 * descriptor byte meaning "3.5 inch, 18 sectors"; 0x55 0xAA at the end of the
 * boot sector is the signature every PC boot sector carries; FAT entries 0 and
 * 1 are reserved (entry 0 holds a copy of the media byte, padded with 1 bits)
 * so real clusters are numbered from 2, which is why the three bytes F0 FF FF
 * are all this function needs to write to produce a validly empty table.
 *
 * A footnote that connects back to the allocator: a filesystem *is* a memory
 * allocator, over a different medium. Free-space tracking, allocation units,
 * fragmentation, the desire to keep a file's blocks adjacent — the free list in
 * THE MEMORY MANAGER and the FAT are the same problem solved twice, and the
 * missing root directory in the corrupted images was a symptom in one that had
 * its cause in the other.
 * ======================================================================== */
static void format_floppy(const char *label){
    memset(floppy,0,FLOP_SIZE);
    uint8_t *b=floppy;
    b[0]=0xEB; b[1]=0x3C; b[2]=0x90;
    memcpy(b+3,"MSDOS5.0",8);
    b[11]=0x00; b[12]=0x02;   /* 512 bytes/sector */
    b[13]=1;                  /* sectors/cluster */
    b[14]=1; b[15]=0;         /* reserved */
    b[16]=2;                  /* FATs */
    b[17]=224; b[18]=0;       /* root entries */
    b[19]=0x40; b[20]=0x0B;   /* 2880 sectors */
    b[21]=0xF0;               /* media */
    b[22]=9; b[23]=0;         /* sectors/FAT */
    b[24]=18; b[25]=0;        /* sectors/track */
    b[26]=2; b[27]=0;         /* heads */
    b[36]=0x00; b[38]=0x29;
    b[39]=0x34; b[40]=0x12; b[41]=0x78; b[42]=0x56;
    memcpy(b+43, label, 11);
    memcpy(b+54,"FAT12   ",8);
    b[510]=0x55; b[511]=0xAA;
    /* FATs */
    for(int f=0;f<2;f++){
        uint8_t *fat = floppy + (1 + f*9)*512;
        fat[0]=0xF0; fat[1]=0xFF; fat[2]=0xFF;
    }
}

/* ===========================================================================
 * STARTUP — becoming a DOS machine, then running the program
 *
 * WHAT "LOADING A PROGRAM" ACTUALLY MEANS
 *
 * A program on disk is a file. A program running is a machine in a particular
 * state. Everything in main() before the final loop is the work of getting from
 * one to the other, and it is the same work any operating system does when you
 * launch anything — worth spelling out, because it is usually invisible:
 *
 *   1. READ THE FILE and parse its header. An executable is not a raw memory
 *      image; it is a container with a header saying how big the code is, where
 *      execution starts, how much stack it wants, and what needs fixing up.
 *      Here that is the MZ header, 28 bytes.
 *
 *   2. FIND MEMORY for it and decide the layout — where the image goes, where
 *      its stack goes, what is left over for it to allocate later.
 *
 *   3. COPY THE IMAGE into that memory. On a modern system this step is lazier
 *      than it sounds: pages are mapped from the file and faulted in on first
 *      touch rather than copied up front. With no virtual memory hardware,
 *      copying is all there is.
 *
 *   4. RELOCATE. The file was written without knowing what address it would end
 *      up at, so any absolute address baked into it is wrong by a constant. The
 *      loader walks a table of the places that need adjusting and adds the
 *      offset. Modern formats do the same thing under the names "relocations"
 *      and "dynamic linking"; the mechanism below is its direct ancestor.
 *
 *   5. BUILD THE PROCESS CONTEXT: the block of information the program will
 *      need about itself — its command line, its environment, its open file
 *      handles. DOS puts all of that in the PSP; a Unix system puts argv and
 *      envp on the new process's stack; the idea is identical.
 *
 *   6. SET THE REGISTERS to their agreed initial values and jump to the entry
 *      point. From the program's perspective this is the moment it comes into
 *      existence: it wakes up mid-machine with certain registers meaning
 *      certain things, and everything it knows it has to read out of the state
 *      the loader arranged.
 *
 * What a modern loader adds on top is mostly protection and sharing: a private
 * virtual address space, pages marked read-only or non-executable, shared
 * libraries mapped in and resolved lazily, address-space randomisation. None of
 * that exists here. The program is placed at a known address in the one and
 * only address space, and it can reach every byte of the machine.
 * ======================================================================== */
int main(int argc, char **argv){
    if(argc<3){ fprintf(stderr,"usage: %s <exe> <out.img> [cmdline] [-v]\n", argv[0]); return 1; }
    selfpath = argv[1];
    const char *outimg = argv[2];
    const char *cmdline = argc>3? argv[3] : "/s a:";
    for(int i=1;i<argc;i++) if(!strcmp(argv[i],"-v")) verbose=1;

    /* Read the whole .EXE into a buffer on the *host's* heap. This is the one
     * place the emulator itself allocates memory dynamically, and it is a
     * useful contrast with the allocator implemented above: malloc(fsz) asks
     * the C runtime for fsz bytes, whose size is not known until the file has
     * been measured, and returns a pointer to them — the same first-fit /
     * splitting / coalescing machinery as mm_alloc(), just several layers down
     * and asking the kernel for more address space when it runs short.
     *
     * The buffer is never freed. That is a deliberate non-bug: the process is
     * about to exit, and the operating system reclaims everything a process
     * owns when it terminates, so freeing memory moments before exit is work
     * with no observable effect. (In a long-running program the same omission
     * would be a leak.) */
    FILE *f=fopen(selfpath,"rb");
    if(!f){ perror("open"); return 1; }
    fseek(f,0,SEEK_END); long fsz=ftell(f); fseek(f,0,SEEK_SET);
    uint8_t *raw=malloc(fsz); fread(raw,1,fsz,f); fclose(f);

    /* -----------------------------------------------------------------------
     * Parse the MZ header.
     *
     * A DOS .EXE begins with a 28-byte header named after Mark Zbikowski, whose
     * initials are the first two bytes of every one ever made — and, because
     * Windows kept it as a stub, of every Windows .exe to this day.
     *
     * The fields are quirky. The file length is stored as a count of 512-byte
     * pages plus the number of bytes used in the last one. The entry point and
     * initial stack are stored as segment:offset pairs *relative to wherever
     * the program gets loaded*, which is not known until load time — hence the
     * relocation table.
     *
     * Reading the fields byte by byte, rather than casting a struct over the
     * buffer, avoids any dependence on the host's byte order or struct padding.
     * `raw[8]|(raw[9]<<8)` is the little-endian assembly described above rd16():
     * low byte first. Struct padding is the other hazard — a C compiler is free
     * to insert unnamed gaps between struct members to keep each one aligned,
     * so a struct laid over a file buffer may not line up with the file at all
     * unless you use non-portable packing directives. Reading field by field
     * sidesteps both problems for the cost of a few lines.
     * -------------------------------------------------------------------- */
    uint16_t hdrpar = raw[8]|(raw[9]<<8);
    uint16_t pages  = raw[4]|(raw[5]<<8);
    uint16_t lastpg = raw[2]|(raw[3]<<8);
    uint16_t nreloc = raw[6]|(raw[7]<<8);
    uint16_t reloff = raw[24]|(raw[25]<<8);
    uint16_t e_ss   = raw[14]|(raw[15]<<8);
    uint16_t e_sp   = raw[16]|(raw[17]<<8);
    uint16_t e_ip   = raw[20]|(raw[21]<<8);
    uint16_t e_cs   = raw[22]|(raw[23]<<8);
    long hdrbytes = hdrpar*16L;
    long imgsize  = (pages-1)*512L + (lastpg? lastpg:512) - hdrbytes;

    /* Choose a memory map. Real DOS would place these wherever it had room;
     * any consistent choice works, as long as the program is told the truth.
     *
     *   0x0000  interrupt vector table, then BIOS data
     *   0x0800  environment block               (segment 0x080)
     *   0x1000  PSP                             (segment 0x100)
     *   0x1100  the program image itself        (segment 0x110)
     *    ...    free memory for the program to allocate
     *   0x9F000 top of conventional memory — above this, on a real PC, sat
     *           video RAM and option ROMs, which is the origin of the famous
     *           640 KB limit.
     */
    env_seg  = 0x0080;
    psp_seg  = 0x0100;
    uint16_t loadseg = psp_seg + 0x10;    /* the image starts right after the PSP */
    alloc_top = 0x9F00;

    /* -----------------------------------------------------------------------
     * The environment block: NUL-terminated "NAME=VALUE" strings, then an extra
     * NUL to end the list, then a word 1, then the full path of the running
     * program. That trailing path is how a DOS program learns its own filename
     * — there is no argv[0] — and it is precisely what this extractor needs in
     * order to open itself and read the compressed image out of its own tail.
     * -------------------------------------------------------------------- */
    {
        uint32_t e = lin(env_seg,0);
        const char *ev = "COMSPEC=C:\\COMMAND.COM";
        int i=0; while(ev[i]){ wr8(e+i, ev[i]); i++; } wr8(e+i,0); i++;
        wr8(e+i,0); i++;
        wr16(e+i,1); i+=2;
        char pathbuf[128]; snprintf(pathbuf,sizeof pathbuf,"C:\\DXP.EXE");
        int j=0; while(pathbuf[j]){ wr8(e+i+j, pathbuf[j]); j++; } wr8(e+i+j,0);
    }
    /* -----------------------------------------------------------------------
     * The PSP, or Program Segment Prefix: a 256-byte control block DOS builds
     * immediately below every program. Part header, part scratch area, part
     * backwards compatibility with CP/M. The fields that matter here:
     *
     *   0x00  an INT 20h instruction, so a program can terminate by jumping
     *         to the start of its own PSP — a CP/M-era convention
     *   0x02  segment just past the end of this program's memory
     *   0x18  the Job File Table: 20 slots mapping this program's file handles
     *         to system-wide ones. Slots 0-4 are the standard streams
     *   0x2C  segment of the environment block
     *   0x50  an INT 21h / RETF sequence, an alternative call entry point
     *   0x80  the command line: a length byte, the text, then a carriage return
     *
     * The command line is how "/s a:" reaches the program. The leading space
     * DOS normally includes is why the argument string starts where it does.
     * -------------------------------------------------------------------- */
    {
        uint32_t p=lin(psp_seg,0);
        wr8(p,0xCD); wr8(p+1,0x20);
        wr16(p+2, alloc_top);
        wr16(p+0x2C, env_seg);
        wr8(p+0x50,0xCD); wr8(p+0x51,0x21); wr8(p+0x52,0xCB);
        int n=strlen(cmdline);
        wr8(p+0x80,n);
        for(int i=0;i<n;i++) wr8(p+0x81+i, cmdline[i]);
        wr8(p+0x81+n,0x0D);
        /* JFT: 20 entries, first 5 open */
        for(int i=0;i<20;i++) wr8(p+0x18+i, i<5? i : 0xFF);
        wr16(p+0x32,20); wr16(p+0x34,0x18); wr16(p+0x36,psp_seg);
    }
    /* Copy the program image into emulated memory, straight after the header. */
    for(long i=0;i<imgsize && hdrbytes+i<fsz;i++) wr8(lin(loadseg,0)+i, raw[hdrbytes+i]);

    /* -----------------------------------------------------------------------
     * Apply relocations.
     *
     * The linker cannot know what segment the program will be loaded at, so
     * every place in the image that contains a literal segment value is listed
     * in a relocation table. The loader walks the table and adds the actual
     * load segment to each of them. This is the ancestor of the relocation
     * mechanism in every modern executable format.
     *
     * These two .EXEs have exactly one relocation entry each.
     * -------------------------------------------------------------------- */
    for(int i=0;i<nreloc;i++){
        uint16_t ro = raw[reloff+i*4]|(raw[reloff+i*4+1]<<8);
        uint16_t rs = raw[reloff+i*4+2]|(raw[reloff+i*4+3]<<8);
        uint32_t a = lin(loadseg+rs, ro);
        wr16(a, rd16(a)+loadseg);
    }
    { uint16_t need1=(uint16_t)((imgsize+15)/16); uint16_t need2=e_ss+(uint16_t)((e_sp+15)/16)+1;
      alloc_next = loadseg + (need1>need2?need1:need2) + 16;
      mm_init(env_seg, alloc_top);
      mm_reserve(env_seg, psp_seg-env_seg);          /* environment block */
      mm_reserve(psp_seg, alloc_next-psp_seg);       /* the program itself */
    }

    /* -----------------------------------------------------------------------
     * Build a real interrupt vector table.
     *
     * The lazy way to emulate INT would be to special-case the INT opcode in
     * step() and call do_int() directly. That breaks as soon as a program does
     * anything more interesting — and DOS programs do. They read vectors and
     * call them with a far CALL, they install their own handler and chain to
     * the previous one, they inspect the vector to see whether a driver is
     * loaded.
     *
     * So instead this builds the genuine article. All 256 vectors are filled
     * in, each pointing at its own four-byte stub up in the BIOS ROM area:
     *
     *      F1 nn C F  ->  [illegal opcode] [int number] [IRET] [NOP]
     *
     * 0xF1 is an opcode the real 8086 does not define, which makes it a safe
     * choice of trap: when step() meets it, it reads the byte after it as an
     * interrupt number and calls do_int(). Then the IRET returns normally.
     *
     * The payoff is that every route into an interrupt handler now works
     * identically to real hardware, including the ones we did not anticipate.
     * -------------------------------------------------------------------- */
    for(int n=0;n<256;n++){
        wr16(n*4, 0x1000+n*4);
        wr16(n*4+2, 0xF000);
        uint32_t a = lin(0xF000, 0x1000+n*4);
        wr8(a,0xF1); wr8(a+1,n); wr8(a+2,0xCF); wr8(a+3,0x90);
    }

    /* -----------------------------------------------------------------------
     * Set the registers to what DOS hands a freshly loaded program: code and
     * stack from the header (relocated by the load segment), and DS and ES both
     * pointing at the PSP, which is how the program finds its command line.
     * setflags(0x0202) is the conventional initial value: interrupts enabled,
     * everything else clear.
     * -------------------------------------------------------------------- */
    S[rCS]=loadseg+e_cs; IPr=e_ip;
    S[rSS]=loadseg+e_ss; SP=e_sp;
    S[rDS]=psp_seg; S[rES]=psp_seg;
    AX=0x0000; BX=0; CX=0; DX=0; SI=0; DI=0; BP=0;
    setflags(0x0202);

    format_floppy("BLANK      ");

    /* -----------------------------------------------------------------------
     * And this is the entire machine: fetch, execute, repeat, until the program
     * terminates itself via INT 21h AH=4Ch. Everything above exists to make
     * these two lines mean something.
     *
     * This is the iterated state transition from the header, written out. Note
     * what is *not* in the loop: no timing, no I/O polling, no scheduling, no
     * event queue. The emulated machine advances only because step() is called,
     * so the whole run is a deterministic function of the input file and the
     * command line, and running it twice produces identical output down to the
     * last byte. That property is what makes a bug in an emulator tractable:
     * you can bisect the run by instruction number, dump the state at exactly
     * the point of divergence, and be sure the next run behaves the same way.
     *
     * The instruction cap is a safety net against an infinite loop — real DOS
     * would just hang. Disk 1 takes about 40 million instructions to extract,
     * disk 2 about 138 million, both a fraction of a second on a modern CPU and
     * both a good deal faster than the 1998 hardware this was written for.
     * -------------------------------------------------------------------- */
    long long maxi = 4000000000LL;
    while(!halted && icount<maxi){ step(); icount++; }

    fprintf(stderr,"\n[emulator stopped: halted=%d exit=%d icount=%lld sectors_written=%d]\n",
            halted, exitcode, icount, trap_written);

    int nw=0; for(int i=0;i<FLOP_CYL*FLOP_HD*FLOP_SPT;i++) if(written[i]) nw++;
    fprintf(stderr,"[distinct sectors written: %d]\n", nw);

    FILE *o=fopen(outimg,"wb"); fwrite(floppy,1,FLOP_SIZE,o); fclose(o);
    return 0;
}
