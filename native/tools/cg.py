"""Lossless decoding of the renderer's original Cg/RSX programs.

Binary layout and ISA references:
  RPCS3/Emu/RSX/Program/CgBinaryProgram.h
  RPCS3/Emu/RSX/Program/RSX{Fragment,Vertex}Program.h
Numeric fields are retained even when this tool has no executable translation
for them. A decoded instruction is not evidence of shader equivalence.
"""

from dataclasses import asdict, dataclass

from sce import region, unpack


FP_NAMES = (
    "NOP MOV MUL ADD MAD DP3 DP4 DST MIN MAX SLT SGE SLE SGT SNE SEQ FRC FLR KIL "
    "PK4 UP4 DDX DDY TEX TXP TXD RCP RSQ EX2 LG2 LIT LRP STR SFL COS SIN PK2 UP2 "
    "POW PKB UPB PK16 UP16 BEM PKG UPG DP2A TXL RESERVED48 TXB RESERVED50 TEXBEM "
    "TXPBEM BEMLUM REFL TIMESWTEX DP2 NRM DIV DIVSQ LIF FENCT FENCB RESERVED63 "
    "BRK CAL IFE LOOP REP RET"
).split()
VP_VECTOR_NAMES = (
    "NOP MOV MUL ADD MAD DP3 DPH DP4 DST MIN MAX SLT SGE ARL FRC FLR SEQ SFL "
    "SGT SLE SNE STR SSG RESERVED23 RESERVED24 TXL"
).split()
VP_SCALAR_NAMES = "NOP MOV RCP RCC RSQ EXP LOG LIT BRA BRI CAL CLI RET LG2 EX2 SIN COS BRB CLB PSH POP".split()


def name_at(data, offset):
    if offset == 0:
        return ""
    region(data, offset, 1)
    end = data.find(b"\0", offset)
    if end < 0:
        raise ValueError("Unterminated Cg parameter name")
    return data[offset:end].decode("utf-8")


def signed(value):
    return value - 0x100000000 if value & 0x80000000 else value


def field(value, shift, width=1):
    return (value >> shift) & ((1 << width) - 1)


@dataclass(frozen=True)
class Parameter:
    name: str
    semantic: str
    type: int
    resource: int
    variability: int
    resource_index: int
    direction: int
    number: int
    referenced: bool
    shared: bool
    default_bits: tuple
    constant_offsets: tuple


@dataclass(frozen=True)
class FragmentInstruction:
    offset: int
    words: tuple
    opcode: int
    mnemonic: str
    constant_bits: tuple

    def to_dict(self):
        result = asdict(self)
        dest, src0, src1, src2 = self.words
        result.update({
            "end": bool(field(dest, 0)),
            "destination": field(dest, 1, 6),
            "half": bool(field(dest, 7)),
            "set_condition": bool(field(dest, 8)),
            "write_mask": field(dest, 9, 4),
            "input": field(dest, 13, 4),
            "texture": field(dest, 17, 4),
            "precision": field(dest, 22, 2),
            "no_destination": bool(field(dest, 30)),
            "saturate": bool(field(dest, 31)),
            "predicate": field(src0, 18, 3),
            "source_precision": [field(src1, shift, 3) for shift in (19, 22, 25)],
            "scale": field(src1, 28, 3),
            "perspective_correct": bool(field(src2, 31)),
        })
        return result


@dataclass(frozen=True)
class VertexInstruction:
    offset: int
    words: tuple
    vector_opcode: int
    scalar_opcode: int

    def to_dict(self):
        result = asdict(self)
        d0, d1, d2, d3 = self.words
        result.update({
            "vector_mnemonic": VP_VECTOR_NAMES[self.vector_opcode],
            "scalar_mnemonic": VP_SCALAR_NAMES[self.scalar_opcode],
            "end": bool(field(d3, 0)),
            "indexed_constant": bool(field(d3, 1)),
            "constant": field(d1, 12, 10),
            "input": field(d1, 8, 4),
            "destination": field(d3, 2, 5),
            "temporary": field(d0, 15, 6),
            "scalar_temporary": field(d3, 7, 6),
            "vector_mask_wzyx": field(d3, 13, 4),
            "scalar_mask_wzyx": field(d3, 17, 4),
            "source_words": [
                (field(d1, 0, 8) << 9) | field(d2, 23, 9),
                field(d2, 6, 17),
                (field(d2, 0, 6) << 11) | field(d3, 21, 11),
            ],
        })
        return result


class Program:
    def __init__(self, data):
        self.data = data
        (self.profile, revision, total, count, params, domain,
         size, code) = unpack(data, 0, ">8I")
        if self.profile not in (7003, 7004) or revision != 6 or total != len(data):
            raise ValueError("Unsupported or truncated Cg binary")
        if count > 4096 or params < 32 or domain < 32 or code % 16 or size % 16 or size == 0:
            raise ValueError("Invalid Cg table layout")
        region(data, params, count * 48)
        domain_bytes = region(data, domain, 24)
        self.domain_words = unpack(domain_bytes, 0, ">6I")
        if self.profile == 7004:
            fields = unpack(domain_bytes, 0, ">IIIHHHBBBB")
            self.domain = dict(zip((
                "instruction_slots", "attribute_input_mask", "partial_texture_types",
                "texture_coordinate_input_mask", "texture_coordinate_2d_mask",
                "texture_coordinate_centroid_mask", "register_count", "output_from_h0",
                "depth_replace", "pixel_kill"), fields))
        else:
            self.domain = dict(zip((
                "instruction_slots", "instruction_slot", "register_count",
                "attribute_input_mask", "attribute_output_mask", "user_clip_mask"), self.domain_words))
        self.code = region(data, code, size)
        self.parameters = []
        for index in range(count):
            p = unpack(data, params + index * 48, ">12I")
            defaults = unpack(data, p[5], ">4I") if p[5] else ()
            patches = ()
            if p[6]:
                length, = unpack(data, p[6], ">I")
                if length > size // 16:
                    raise ValueError("Invalid Cg constant patch table")
                patches = unpack(data, p[6] + 4, f">{length}I")
                if any(offset % 16 or offset + 16 > size for offset in patches):
                    raise ValueError("Cg constant patch points outside the microcode")
            if p[10] not in (0, 1) or p[11] not in (0, 1):
                raise ValueError("Invalid Cg parameter flags")
            self.parameters.append(Parameter(
                name_at(data, p[4]), name_at(data, p[7]), p[0], p[1], p[2], signed(p[3]),
                p[8], signed(p[9]), bool(p[10]), bool(p[11]), defaults, patches))
        self.instructions = self._fragment() if self.profile == 7004 else self._vertex()
        if self.domain_words[0] != len(self.code) // 16:
            raise ValueError("Cg instruction-slot count does not match its microcode")
        if self.profile == 7004:
            literals = {instruction.offset + 16 for instruction in self.instructions if instruction.constant_bits}
            if any(offset not in literals for parameter in self.parameters for offset in parameter.constant_offsets):
                raise ValueError("Cg uniform patch does not refer to an inline literal")

    def _fragment(self):
        result = []
        offset = 0
        while offset < len(self.code):
            packed = unpack(self.code, offset, ">4I")
            words = tuple((word >> 16) | ((word & 0xFFFF) << 16) for word in packed)
            opcode = field(words[0], 24, 6) | (field(words[2], 31) << 6)
            if opcode >= len(FP_NAMES) or FP_NAMES[opcode].startswith("RESERVED"):
                raise ValueError(f"Unknown RSX fragment opcode {opcode} at {offset:#x}")
            literal = any(field(word, 0, 2) == 2 for word in words[1:])
            constants = ()
            if literal:
                constants = tuple((word >> 16) | ((word & 0xFFFF) << 16)
                                  for word in unpack(self.code, offset + 16, ">4I"))
            result.append(FragmentInstruction(offset, words, opcode, FP_NAMES[opcode], constants))
            offset += 32 if literal else 16
            if words[0] & 1:
                if offset != len(self.code):
                    raise ValueError("Data follows the RSX fragment end instruction")
                return result
        raise ValueError("RSX fragment program has no end instruction")

    def _vertex(self):
        result = []
        for offset in range(0, len(self.code), 16):
            words = unpack(self.code, offset, ">4I")
            vector = field(words[1], 22, 5)
            scalar = field(words[1], 27, 5)
            if (vector >= len(VP_VECTOR_NAMES) or scalar >= len(VP_SCALAR_NAMES)
                    or VP_VECTOR_NAMES[vector].startswith("RESERVED")):
                raise ValueError(f"Unknown RSX vertex opcode at {offset:#x}")
            result.append(VertexInstruction(offset, words, vector, scalar))
        if not (result[-1].words[3] & 1):
            raise ValueError("RSX vertex program has no final end instruction")
        return result

    def to_dict(self):
        return {
            "profile": self.profile,
            "domain": self.domain,
            "domain_words": self.domain_words,
            "parameters": [asdict(parameter) for parameter in self.parameters],
            "instructions": [instruction.to_dict() for instruction in self.instructions],
        }
