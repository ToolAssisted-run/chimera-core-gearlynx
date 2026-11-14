#!/usr/bin/env python3
"""Writes the core's test program: an Atari Lynx program of this repository's
own, so the tests need no game of anyone's and no boot ROM.

usage: make-testprog.py OUT.o [nopad]

The file is in BS93 format, the one homebrew is sent to a Lynx in: a 10-byte
header (0x80 0x08, where it goes in memory, its length, "BS93") and then the
program, which Gearlynx copies to 0x0200 and starts there, with the boot ROM
hidden. The program is 65C02 machine code, assembled below by a small
assembler of this file's own (an instruction a line). It is released under
the MIT licence, as the rest of this repository.

At the start it points the screen at 0xA000, gives colours 1 to 15 a yellow
ramp (green and red the same, no blue), lets timer 2 (the one that ends each
frame) raise its flag, and keeps what the processor reads at 0xFFF9 (the
memory map register, which reads differently on a Lynx I and a Lynx II) at
0x0087. Then, once a frame, when
timer 2's flag goes up, it:

  - counts frames at 0x0080 (16 bits, low byte first);
  - reads the joystick (Suzy's JOYSTICK, 0xFCB0) into 0x0082 and ORs it into
    0x0083 (every key pressed so far): Up 0x80, Down 0x40, Left 0x20,
    Right 0x10, Option 1 0x08, Option 2 0x04, B 0x02, A 0x01; and ORs the
    switches (SWITCHES, 0xFCB1, Pause is 0x01) into 0x0084;
  - fills one line of the screen with the frame count, a line further down
    each frame, so every picture is new;
  - sets colour 1 from the joystick, so the picture shows the keys;
  - plays a tone on sound channel 0 while A is held, its pitch from the
    joystick, and nothing otherwise.

With "nopad" the program never reads the joystick or the switches: every
frame of it is a lag frame.
"""
import sys

OPS = {
    # name: {mode: opcode}
    "LDA": {"imm": 0xA9, "zp": 0xA5, "abs": 0xAD},
    "STA": {"zp": 0x85, "abs": 0x8D, "absx": 0x9D, "indy": 0x91},
    "ORA": {"zp": 0x05, "imm": 0x09},
    "AND": {"imm": 0x29},
    "CMP": {"imm": 0xC9},
    "ADC": {"imm": 0x69},
    "LDX": {"imm": 0xA2},
    "LDY": {"imm": 0xA0},
    "INC": {"zp": 0xE6},
    "TXS": {"impl": 0x9A}, "TXA": {"impl": 0x8A}, "DEX": {"impl": 0xCA},
    "DEY": {"impl": 0x88}, "CLC": {"impl": 0x18}, "SEI": {"impl": 0x78},
    "CLD": {"impl": 0xD8},
    "BNE": {"rel": 0xD0}, "BEQ": {"rel": 0xF0}, "BPL": {"rel": 0x10},
    "JMP": {"abs": 0x4C},
}
SIZE = {"impl": 1, "imm": 2, "zp": 2, "indy": 2, "rel": 2, "abs": 3, "absx": 3}


def assemble(lines, origin):
    """Two passes over (label or None, mnemonic, operand) lines."""
    def mode_of(op, arg):
        if arg is None:
            return "impl"
        if op in ("BNE", "BEQ", "BPL"):
            return "rel"
        if arg.startswith("#"):
            return "imm"
        if arg.startswith("(") and arg.endswith("),Y"):
            return "indy"
        if arg.endswith(",X"):
            return "absx"
        return "zp" if "zp" in OPS[op] and arg.startswith("$") and len(arg) == 3 else "abs"

    labels, pc = {}, origin
    for label, op, arg in lines:
        if label:
            labels[label] = pc
        if op:
            pc += SIZE[mode_of(op, arg)]

    def value(text):
        text = text.lstrip("#").strip("()").replace(",Y", "").replace(",X", "").replace(")", "")
        if text.startswith("$"):
            return int(text[1:], 16)
        if text.isdigit():
            return int(text)
        if text.startswith("<"):
            return labels[text[1:]] & 0xFF
        if text.startswith(">"):
            return labels[text[1:]] >> 8
        return labels[text]

    out, pc = bytearray(), origin
    for label, op, arg in lines:
        if not op:
            continue
        mode = mode_of(op, arg)
        out.append(OPS[op][mode])
        if mode in ("imm", "zp", "indy"):
            out.append(value(arg) & 0xFF)
        elif mode in ("abs", "absx"):
            v = value(arg)
            out += bytes((v & 0xFF, v >> 8))
        elif mode == "rel":
            d = value(arg) - (pc + 2)
            assert -128 <= d <= 127, f"branch to {arg} too far"
            out.append(d & 0xFF)
        pc += SIZE[mode]
    return bytes(out)


def program(nopad):
    src = """
        SEI
        CLD
        LDX #$FF
        TXS
        LDA #$00
        STA $FD94          ; DISPADR: the screen at 0xA000
        LDA #$A0
        STA $FD95
        LDA #$9F           ; timer 2: reload, count, linked, flag on borrow
        STA $FD09
        LDX #$0F
pal:    TXA
        STA $FDA0,X        ; GREEN1..15
        STA $FDB0,X        ; BLUERED1..15: red = the index, no blue
        DEX
        BNE pal
        LDA $FFF9          ; MAPCTL as this Lynx reads it
        STA $87
        LDA #$00
        STA $80
        STA $81
        STA $82
        STA $83
        STA $84
        STA $86
        STA $88            ; the line pointer: 0xA000
        LDA #$A0
        STA $89
        LDA #$01           ; sound channel 0: a square wave
        STA $FD21
        STA $FD23
        LDA #$00
        STA $FD50
loop:   LDA $FD81          ; INTSET: wait for timer 2's flag
        AND #$04
        BEQ loop
        LDA #$04
        STA $FD80          ; INTRST: clear it
        INC $80
        BNE counted
        INC $81
counted:
"""
    if not nopad:
        src += """
        LDA $FCB0          ; JOYSTICK
        STA $82
        ORA $83
        STA $83
        LDA $FCB1          ; SWITCHES
        ORA $84
        STA $84
"""
    src += """
        LDA $80            ; a line of the screen gets the frame count
        LDY #79
fill:   STA ($88),Y
        DEY
        BPL fill
        CLC
        LDA $88
        ADC #80
        STA $88
        LDA $89
        ADC #0
        STA $89
        INC $86            ; the line number, 0 to 101
        LDA $86
        CMP #102
        BNE colour
        LDA #$00
        STA $86
        STA $88
        LDA #$A0
        STA $89
colour: LDA $82
        STA $FDB1          ; colour 1's blue and red from the joystick
        STA $FDA2          ; colour 2's green
        AND #$01           ; A held: a tone
        BEQ quiet
        LDA #$60
quiet:  STA $FD20          ; AUD0VOL
        LDA $82
        ORA #$10
        STA $FD24          ; AUD0TBACK: the pitch
        LDA #$1B           ; AUD0CTL: reload, count, clock 3
        STA $FD25
        JMP loop
"""
    lines = []
    for raw in src.splitlines():
        text = raw.split(";")[0].rstrip()
        if not text.strip():
            continue
        label = None
        if not raw[0].isspace():
            label, _, text = text.partition(":")
            label = label.strip()
        parts = text.split(None, 1)
        op = parts[0].upper() if parts else None
        arg = parts[1].strip() if len(parts) > 1 else None
        lines.append((label, op, arg))
    return assemble(lines, 0x0200)


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    code = program("nopad" in sys.argv[2:])
    size = len(code) + 10
    header = bytes((0x80, 0x08, 0x02, 0x00, size >> 8, size & 0xFF)) + b"BS93"
    with open(sys.argv[1], "wb") as f:
        f.write(header + code)


if __name__ == "__main__":
    main()
