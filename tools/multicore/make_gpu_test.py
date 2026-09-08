#!/usr/bin/env python3
"""Generate a self-checking PS-X EXE that exercises the GPU rasteriser.

The test needs no commercial BIOS or game. --bios-dir generates a minimal
reset vector that enters Beetle's PS-X EXE loader, with no proprietary code.  The ROM draws primitives whose result in VRAM is exactly predictable,
reads them back with GP0 C0h, and compares every returned word against the
value the hardware is specified to produce.  A renderer that rasterises to the
wrong place, drops the mask bit, dithers something it should not, or loses a
CPU->VRAM upload fails here rather than in someone's game.

Only primitives with an exact expected value are used, and triangles are
sampled well inside the edges, so nothing here depends on fill-rule tie
breaking.

The result lands at physical RAM 0x1f0000 in the format tools/multicore/
testrom_report.h parses:

    +0  magic 'GPUT'   +4  done   +8  cases   +12 words   +16 failures
    +20 first-fail case  +24 first-fail word  +28 actual  +32 expected

Usage: make_gpu_test.py [out.exe]
Then:  GLHOST_TEST_REPORT=1 tools/glhost/glhost <core> out.exe
"""

import argparse
from pathlib import Path
import struct
import sys

LOAD_ADDR = 0x80010000
SP_INIT = 0x801FFF00
REPORT = 0x801F0000

GP0 = 0x1F801810
GP1 = 0x1F801814

# ---------------------------------------------------------------- assembler
ZERO, AT, V0, V1, A0, A1, A2, A3 = 0, 1, 2, 3, 4, 5, 6, 7
T0, T1, T2, T3, T4, T5, T6, T7 = 8, 9, 10, 11, 12, 13, 14, 15
S0, S1, S2, S3, S4, S5, S6, S7 = 16, 17, 18, 19, 20, 21, 22, 23
T8, T9 = 24, 25
SP, RA = 29, 31


class Asm:
    def __init__(self, base):
        self.base = base
        self.words = []
        self.labels = {}
        self.fix = []          # (index, label, kind)

    # -- encodings
    def _r(self, rs, rt, rd, sh, fn):
        self.words.append((rs << 21) | (rt << 16) | (rd << 11) | (sh << 6) | fn)

    def _i(self, op, rs, rt, imm):
        self.words.append((op << 26) | (rs << 21) | (rt << 16) | (imm & 0xFFFF))

    def _j(self, op, label):
        self.fix.append((len(self.words), label, "j"))
        self.words.append(op << 26)

    # -- labels
    def label(self, name):
        self.labels[name] = self.base + 4 * len(self.words)

    def addr_of(self, name):
        return self.labels[name]

    # -- instructions
    def nop(self):                 self.words.append(0)
    def lui(self, rt, i):          self._i(0x0F, 0, rt, i)
    def ori(self, rt, rs, i):      self._i(0x0D, rs, rt, i)
    def andi(self, rt, rs, i):     self._i(0x0C, rs, rt, i)
    def addiu(self, rt, rs, i):    self._i(0x09, rs, rt, i)
    def sltiu(self, rt, rs, i):    self._i(0x0B, rs, rt, i)
    def lw(self, rt, off, rs):     self._i(0x23, rs, rt, off)
    def sw(self, rt, off, rs):     self._i(0x2B, rs, rt, off)
    def addu(self, rd, rs, rt):    self._r(rs, rt, rd, 0, 0x21)
    def subu(self, rd, rs, rt):    self._r(rs, rt, rd, 0, 0x23)
    def and_(self, rd, rs, rt):    self._r(rs, rt, rd, 0, 0x24)
    def or_(self, rd, rs, rt):     self._r(rs, rt, rd, 0, 0x25)
    def sltu(self, rd, rs, rt):    self._r(rs, rt, rd, 0, 0x2B)
    def sll(self, rd, rt, sa):     self._r(0, rt, rd, sa, 0x00)
    def srl(self, rd, rt, sa):     self._r(0, rt, rd, sa, 0x02)
    def jr(self, rs):              self._r(rs, 0, 0, 0, 0x08)

    def _branch(self, op, rs, rt, label):
        self.fix.append((len(self.words), label, "b"))
        self.words.append((op << 26) | (rs << 21) | (rt << 16))

    def beq(self, rs, rt, label):  self._branch(0x04, rs, rt, label)
    def bne(self, rs, rt, label):  self._branch(0x05, rs, rt, label)
    def j(self, label):            self._j(0x02, label)
    def jal(self, label):          self._j(0x03, label)

    # -- pseudo
    def li(self, rt, value):
        value &= 0xFFFFFFFF
        if value < 0x10000:
            self.ori(rt, ZERO, value)
        elif (value & 0xFFFF) == 0:
            self.lui(rt, value >> 16)
        else:
            self.lui(rt, value >> 16)
            self.ori(rt, rt, value & 0xFFFF)

    def link(self):
        for idx, name, kind in self.fix:
            target = self.labels[name]
            if kind == "b":
                delta = (target - (self.base + 4 * (idx + 1))) >> 2
                self.words[idx] |= delta & 0xFFFF
            else:
                self.words[idx] |= (target >> 2) & 0x3FFFFFF
        return b"".join(struct.pack("<I", w) for w in self.words)


# ------------------------------------------------------------- expectations
def bgr555(r, g, b):
    """The 15-bit VRAM value the GPU stores for a 24-bit colour."""
    return ((b >> 3) << 10) | ((g >> 3) << 5) | (r >> 3)


def two(px):
    """Two identical pixels as one GPUREAD word."""
    return (px << 16) | px


# Colour channels are multiples of 8 so the >>3 truncation is exact and the
# expected value cannot be argued with.
C1 = (0x08, 0x10, 0x18)
C2 = (0xF8, 0x00, 0x80)
C3 = (0x00, 0xF8, 0x38)

def cmd_colour(op, c):
    r, g, b = c
    return (op << 24) | (b << 16) | (g << 8) | r


def build(unmapped_probes=0, dma=False):
    a = Asm(LOAD_ADDR)
    cases = []
    pending = []
    transfers = 0

    def flush():
        """Submit actual DMA2 linked-list packets, including split commands."""
        nonlocal transfers
        if not pending:
            return
        transfers += 1
        base = 0x000A0000
        packet = []
        for start in range(0, len(pending), 255):
            words = pending[start:start + 255]
            next_addr = base + 4 * (len(packet) + len(words) + 1)
            if start + len(words) == len(pending):
                next_addr = 0xFFFFFF
            packet.append((len(words) << 24) | next_addr)
            packet.extend(words)
        assert len(packet) * 4 < 0x8000  # signed store offset
        a.li(T1, base | 0x80000000)
        for i, word in enumerate(packet):
            a.li(T0, word)
            a.sw(T0, i * 4, T1)
        pending.clear()
        a.li(T0, 0x04000002)       # GP1 DMA direction: CPU -> GPU
        a.sw(T0, 0, S1)
        a.li(T1, 0x1F8010A0)      # DMA2 MADR/BCR/CHCR; DPCR at +0x50
        a.lw(T0, 0x50, T1)
        a.nop()                   # R3000 load delay
        a.ori(T0, T0, 0x0800)     # enable DMA2, preserve other priorities
        a.sw(T0, 0x50, T1)
        a.li(T0, base)
        a.sw(T0, 0, T1)
        a.sw(ZERO, 4, T1)
        a.li(T0, 0x01000401)      # linked list, from RAM, start
        a.sw(T0, 8, T1)
        label = "wait_dma_%d" % transfers
        a.label(label)
        a.lw(T0, 8, T1)
        a.lui(T2, 0x0100)
        a.and_(T0, T0, T2)
        a.bne(T0, ZERO, label)
        a.nop()

    def gp0(word):
        if dma:
            pending.append(word)
        else:
            a.li(T0, word)
            a.sw(T0, 0, S0)

    def gp1(word):
        flush()
        a.li(T0, word)
        a.sw(T0, 0, S1)

    def check(x, y, w, h, expected, tag):
        """Read back w*h pixels at (x,y) and compare every word to expected."""
        flush()
        cases.append(tag)
        a.li(A0, (y << 16) | x)
        a.li(A1, (h << 16) | w)
        a.li(A2, expected)
        a.jal("check_rect")
        a.nop()

    # ---- entry
    a.li(S0, GP0)
    a.li(S1, GP1)
    a.li(S2, REPORT)
    a.li(S3, 0)                     # cases
    a.li(S4, 0)                     # words
    a.li(S5, 0)                     # failures

    # Magic first, so "ROM did not start" is distinguishable from "ROM failed".
    a.li(T0, 0x54555047)
    a.sw(T0, 0, S2)
    a.sw(ZERO, 4, S2)
    for off in (8, 12, 16, 20, 24, 28, 32, 36):
        a.sw(ZERO, off, S2)

    if unmapped_probes:
        # Exercise the phone/TV's unmapped-load failure without corrupting
        # guest control flow. More than 64 completed probes must not retire
        # Lightrec, and all following GPU checks must still complete.
        a.li(T6, 0x5FFFFCFC)
        a.li(T7, unmapped_probes)
        a.label("unmapped_probe")
        a.lw(T5, 0, T6)
        a.addiu(T7, T7, -1)
        a.bne(T7, ZERO, "unmapped_probe")
        a.nop()

    # ---- GPU init
    gp1(0x00000000)                 # reset
    gp1(0x04000000)                 # DMA direction: off, we write GP0 directly
    gp1(0x03000000)                 # display enabled
    gp0(0xE1000000)                 # draw mode: no dither, no texture page
    gp0(0xE3000000)                 # drawing area top-left  (0,0)
    gp0(0xE4000000 | (511 << 10) | 1023)   # bottom-right (1023,511)
    gp0(0xE5000000)                 # drawing offset (0,0)
    gp0(0xE6000000)                 # mask: don't set, don't test

    # ---- case 1: Fill Rect (GP0 02h).  Writes the raw colour, no mask bit,
    # no dither, no rasteriser edge rules - the most basic thing that can be
    # wrong, and the one whose expected value is beyond dispute.
    gp0(cmd_colour(0x02, C1))
    gp0((0 << 16) | 0)
    gp0((16 << 16) | 16)
    check(0, 0, 16, 16, two(bgr555(*C1)), "fill_rect")

    # ---- case 2: monochrome rectangle (GP0 60h) - the sprite rasteriser.
    gp0(cmd_colour(0x60, C2))
    gp0((32 << 16) | 32)
    gp0((16 << 16) | 16)
    check(32, 32, 16, 16, two(bgr555(*C2)), "rect_60")

    # ---- case 3: flat opaque triangle (GP0 20h).  Two triangles cover a
    # 16x16 box at (64,64); the read-back is the box interior only, so no
    # sample sits on a shared edge where fill-rule tie-breaking would decide
    # it.  Vertices are the box corners expanded by one pixel.
    gp0(cmd_colour(0x20, C3))
    gp0((63 << 16) | 63)
    gp0((63 << 16) | 81)
    gp0((81 << 16) | 63)
    gp0(cmd_colour(0x20, C3))
    gp0((63 << 16) | 81)
    gp0((81 << 16) | 63)
    gp0((81 << 16) | 81)
    check(64, 64, 16, 16, two(bgr555(*C3)), "tri_20")

    # ---- case 4: VRAM->VRAM copy (GP0 80h) of the case-1 block.
    gp0(0x80000000)
    gp0((0 << 16) | 0)              # source (0,0)
    gp0((128 << 16) | 128)          # destination (128,128)
    gp0((16 << 16) | 16)
    check(128, 128, 16, 16, two(bgr555(*C1)), "vram_copy")

    # ---- case 5: CPU->VRAM (GP0 A0h) round trip.  Catches an upload path
    # that writes the wrong rectangle or never reaches the readable copy.
    gp0(0xA0000000)
    gp0((200 << 16) | 200)
    gp0((4 << 16) | 8)              # 8x4 pixels = 16 words
    for _ in range(16):
        gp0(0x3C0D3C0D)
    check(200, 200, 8, 4, 0x3C0D3C0D, "cpu_to_vram")

    # ---- case 6: fill rect again over the case-1 block with a new colour,
    # proving a later write is visible to a later read (no stale mirror).
    gp0(cmd_colour(0x02, C2))
    gp0((0 << 16) | 0)
    gp0((16 << 16) | 16)
    check(0, 0, 16, 16, two(bgr555(*C2)), "fill_overwrite")

    # Transfers and primitives must keep the destination mask coherent.
    def upload(x, y, w, h, word):
        gp0(0xA0000000)
        gp0((y << 16) | x)
        gp0((h << 16) | w)
        for _ in range((w * h + 1) // 2):
            gp0(word)

    def copy(sx, sy, dx, dy, w, h):
        gp0(0x80000000)
        gp0((sy << 16) | sx)
        gp0((dy << 16) | dx)
        gp0((h << 16) | w)

    def rect(x, y, w, h, color):
        gp0(cmd_colour(0x60, color))
        gp0((y << 16) | x)
        gp0((h << 16) | w)

    # Larger than both the host staging buffer and a linked-list packet;
    # the upload remains in progress across four packet boundaries.
    upload(0, 100, 1024, 2, 0x12341234)
    check(0, 100, 1024, 2, 0x12341234, "streamed_upload")

    # Quad and polyline continuations have special FIFO readiness rules.
    gp0(cmd_colour(0x28, C1))
    for x, y in ((200, 160), (232, 160), (200, 180), (232, 180)):
        gp0((y << 16) | x)
    check(208, 168, 8, 2, two(bgr555(*C1)), "quad_continuation")
    gp0(cmd_colour(0x48, C2))
    for x in (200, 216, 232):
        gp0((190 << 16) | x)
    gp0(0x50005000)
    # Check that the terminator releases the decoder for the next command.
    # Native GL line coverage depends on the backend's pixel-center rules.
    rect(240, 190, 2, 1, C2)
    check(240, 190, 2, 1, two(bgr555(*C2)), "polyline_termination")

    upload(300, 300, 8, 1, 0x80018001)
    gp0(0xE6000002)
    upload(300, 300, 8, 1, 0x001F001F)
    check(300, 300, 8, 1, 0x80018001, "masked_upload")
    rect(300, 300, 8, 1, C2)
    check(300, 300, 8, 1, 0x80018001, "uploaded_mask_blocks_draw")

    gp0(0xE6000000)
    copy(300, 300, 320, 300, 8, 1)
    gp0(0xE6000002)
    rect(320, 300, 8, 1, C2)
    check(320, 300, 8, 1, 0x80018001, "copied_mask_blocks_draw")
    copy(200, 200, 320, 300, 8, 1)
    check(320, 300, 8, 1, 0x80018001, "masked_copy")

    gp0(0xE6000000)
    upload(340, 300, 8, 1, 0x00010001)
    gp0(0xE6000001)
    copy(340, 300, 340, 300, 8, 1)
    check(340, 300, 8, 1, 0x80018001, "same_location_set_mask")

    gp0(0xE6000000)
    upload(0, 256, 8, 1, 0x80018001)
    gp0(0xE1000110)              # 15bpp texture, page at (0,256)
    gp0(0x65000000)              # raw opaque textured rectangle
    gp0((256 << 16) | 400)
    gp0(0)
    gp0((1 << 16) | 8)
    gp0(0xE6000002)
    rect(400, 256, 8, 1, C2)
    check(400, 256, 8, 1, 0x80018001, "texture_mask_blocks_draw")

    gp0(0xE6000000)
    upload(1022, 450, 4, 1, 0x01230123)
    copy(1022, 450, 1023, 451, 4, 1)
    check(1023, 451, 4, 1, 0x01230123, "wrapped_copy")

    upload(0, 460, 128, 1, 0x00010001)
    upload(128, 460, 128, 1, 0x00020002)
    copy(0, 460, 1, 460, 256, 1)
    check(128, 460, 2, 1, 0x00010001, "overlap_128_word_boundary")
    check(130, 460, 2, 1, 0x00020002, "overlap_after_boundary")

    # Fill converts 8-bit channels by truncating before RGB5 storage.
    gp0(cmd_colour(0x02, (7, 15, 23)))
    gp0((470 << 16) | 32)
    gp0((1 << 16) | 16)
    check(32, 470, 16, 1, two(bgr555(7, 15, 23)), "fill_truncation")

    # Leave GPU-written texture pixels and a CLUT dirty across several frames.
    # Presentation must not discard their dependency records. The final VRAM
    # row/column also catches an inclusive/exclusive full-mirror off-by-one.
    gp0(0xE1000000)
    rect(16, 272, 8, 1, C1)
    rect(0, 480, 16, 1, C2)
    rect(1022, 511, 2, 1, C3)
    upload(640, 256, 2, 1, 0x11111111)
    flush()
    a.li(T7, 800000)
    a.label("wait_frames")
    a.addiu(T7, T7, -1)
    a.bne(T7, ZERO, "wait_frames")
    a.nop()

    gp0(0xE1000110)                 # 15bpp page (0,256)
    gp0(0x65000000)
    gp0((320 << 16) | 400)
    gp0((16 << 8) | 16)
    gp0((1 << 16) | 8)
    check(400, 320, 8, 1, two(bgr555(*C1)), "texture_across_frames")

    gp0(0xE100001A)                 # 4bpp page (640,256)
    gp0(0x65000000)
    gp0((322 << 16) | 400)
    gp0((480 << 6) << 16)           # palette (0,480), UV (0,0)
    gp0((1 << 16) | 8)
    check(400, 322, 8, 1, two(bgr555(*C2)), "clut_across_frames")

    gp0(0xE100011F)                 # 15bpp page (960,256)
    gp0(0x65000000)
    gp0((324 << 16) | 400)
    gp0((255 << 8) | 62)
    gp0((1 << 16) | 2)
    check(400, 324, 2, 1, two(bgr555(*C3)), "vram_edge_across_frames")

    # ---- report
    a.li(T0, len(cases))
    a.sw(T0, 8, S2)
    a.sw(S4, 12, S2)
    a.sw(S5, 16, S2)
    a.li(T0, 1)
    a.sw(T0, 4, S2)                 # done last

    a.label("spin")
    a.j("spin")
    a.nop()

    # ------------------------------------------------------- check_rect
    # a0 = (y<<16)|x, a1 = (h<<16)|w, a2 = expected word.
    # Issues GP0 C0h, then reads (w*h)/2 words and compares each.
    a.label("check_rect")
    a.addiu(S3, S3, 1)              # cases++ (index of the case being checked)

    a.li(T0, 0xC0000000)
    a.sw(T0, 0, S0)
    a.sw(A0, 0, S0)
    a.sw(A1, 0, S0)

    # words = w * h / 2, by shift-add: w and h are small powers of two here,
    # but do it generally with a multiply loop to keep the ROM assembler-free.
    a.andi(T1, A1, 0xFFFF)          # w
    a.srl(T2, A1, 16)               # h
    a.li(T3, 0)                     # accumulator = w*h
    a.label("mul_loop")
    a.beq(T2, ZERO, "mul_done")
    a.nop()
    a.addu(T3, T3, T1)
    a.addiu(T2, T2, -1)
    a.j("mul_loop")
    a.nop()
    a.label("mul_done")
    a.srl(T3, T3, 1)                # pixels -> 32-bit words

    a.label("read_loop")
    a.beq(T3, ZERO, "read_done")
    a.nop()

    # Wait for "ready to send VRAM to CPU" (GPUSTAT bit 27).
    a.label("wait_vram")
    a.lw(T4, 0, S1)
    a.lui(T5, 0x0800)
    a.and_(T4, T4, T5)
    a.beq(T4, ZERO, "wait_vram")
    a.nop()

    a.lw(T6, 0, S0)                 # GPUREAD
    a.addiu(S4, S4, 1)              # words++
    a.beq(T6, A2, "word_ok")
    a.nop()

    # First failure only: record case, word index and both values.
    a.bne(S5, ZERO, "already_failed")
    a.nop()
    a.sw(S3, 20, S2)
    a.sw(S4, 24, S2)
    a.sw(T6, 28, S2)
    a.sw(A2, 32, S2)
    a.label("already_failed")
    a.addiu(S5, S5, 1)              # failures++

    a.label("word_ok")
    a.addiu(T3, T3, -1)
    a.j("read_loop")
    a.nop()

    a.label("read_done")
    a.jr(RA)
    a.nop()

    return a.link(), cases


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("out", nargs="?", default="gpu_test.exe")
    parser.add_argument("--bios-dir", type=Path, help="write a minimal test-only reset BIOS here")
    parser.add_argument("--unmapped-probes", type=int, default=0,
                        help="issue completed unmapped loads before the GPU checks")
    parser.add_argument("--dma", action="store_true",
                        help="send drawing commands via DMA2 linked lists instead of PIO")
    args = parser.parse_args()
    if not 0 <= args.unmapped_probes <= 0x7fffffff:
        parser.error("unmapped-probes must be between 0 and 2147483647")
    out = args.out
    if args.bios_dir:
        args.bios_dir.mkdir(parents=True, exist_ok=True)
        bios = bytearray(512 * 1024)
        # Reset PC BFC00000 -> BF001000, where LoadEXE installs its loader.
        struct.pack_into("<IIII", bios, 0, 0x3C08BF00, 0x35081000, 0x01000008, 0)
        for name in ("scph5500.bin", "scph5501.bin", "scph5502.bin"):
            (args.bios_dir / name).write_bytes(bios)
    text, cases = build(args.unmapped_probes, args.dma)

    # Pad the text to the 2048-byte granularity a PS-X EXE header declares.
    if len(text) % 2048:
        text += b"\x00" * (2048 - len(text) % 2048)

    hdr = bytearray(2048)
    hdr[0:8] = b"PS-X EXE"
    struct.pack_into("<I", hdr, 0x10, LOAD_ADDR)      # initial PC
    struct.pack_into("<I", hdr, 0x14, 0)              # initial GP
    struct.pack_into("<I", hdr, 0x18, LOAD_ADDR)      # load address
    struct.pack_into("<I", hdr, 0x1C, len(text))      # text size
    struct.pack_into("<I", hdr, 0x30, SP_INIT)        # SP base
    struct.pack_into("<I", hdr, 0x34, 0)              # SP offset
    hdr[0x4C:0x4C + 30] = b"Sony Computer Entertainment Inc"[:30]

    with open(out, "wb") as f:
        f.write(bytes(hdr))
        f.write(text)

    print("wrote %s (%d bytes text, %d cases: %s)"
          % (out, len(text), len(cases), ", ".join(cases)))


if __name__ == "__main__":
    main()
