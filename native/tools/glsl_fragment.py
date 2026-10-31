"""Lower the original straight-line NV40 fragment programs to GLSL.

R/H register aliasing follows the physical word/half-word map, including masked
writes. Texture scheduling fences have no observable effect in these programs:
the GLSL expressions retain their data dependencies and perform no memory writes.
"""

from cg import field
from glsl_common import (CHANNELS, HEADER, clamp_precision, predicate, swizzle,
                         varyings, vector_operation)


REGISTERS = """
uvec4 registers[64];
vec4 readR(int n) { return uintBitsToFloat(registers[n]); }
vec4 readH(int n) {
    int i = (n & 1) * 2;
    return vec4(unpackHalf2x16(registers[n / 2][i]), unpackHalf2x16(registers[n / 2][i + 1]));
}
void writeR(int n, vec4 value, bvec4 mask) {
    for (int c = 0; c < 4; ++c) if (mask[c]) registers[n][c] = floatBitsToUint(value[c]);
}
void writeH(int n, vec4 value, bvec4 mask) {
    vec4 previous = readH(n);
    for (int c = 0; c < 4; ++c) if (mask[c]) previous[c] = value[c];
    int i = (n & 1) * 2;
    registers[n / 2][i] = packHalf2x16(previous.xy);
    registers[n / 2][i + 1] = packHalf2x16(previous.zw);
}
"""


def operand(program, instruction, index):
    words = instruction.words
    source = words[index + 1]
    kind = field(source, 0, 2)
    if kind == 0:
        expression = f"read{'H' if field(source, 8) else 'R'}({field(source, 2, 6)})"
    elif kind == 1:
        attr = field(words[0], 13, 4)
        if attr == 0:
            expression = "gl_FragCoord"
        elif attr <= 13:
            expression = f"varying{attr - 1}"
        else:
            raise ValueError(f"Unsupported original fragment input {attr}")
        if field(words[3], 30):
            raise ValueError("Indexed fragment inputs require an explicit adapter")
    elif kind == 2:
        expression = f"fc[{instruction.offset // 16 + 1}]"
    else:
        raise ValueError("Invalid fragment source register type")
    expression = f"({expression}).{swizzle(source, (9, 11, 13, 15))}"
    precision = field(words[2], 19 + 3 * index, 3)
    if precision == 4:
        expression = clamp_precision(expression, precision)
    if field(source, 29 if index == 0 else 18):
        expression = f"abs({expression})"
    if precision != 4:
        expression = clamp_precision(expression, precision)
    if field(source, 17):
        expression = f"(-{expression})"
    return expression


def translate(program):
    if program.profile != 7004:
        raise ValueError("Expected an original fragment program")
    samplers = {}
    for parameter in program.parameters:
        if 2048 <= parameter.resource <= 2063:
            if parameter.type not in (1066, 1067, 1068):
                raise ValueError(f"Unsupported original sampler type {parameter.type}")
            samplers[parameter.resource - 2048] = parameter.type
    lines = [HEADER, "layout(origin_upper_left) in vec4 gl_FragCoord;", varyings("in"),
             f"layout(std140, binding=1) uniform FragmentConstants {{ vec4 fc[{len(program.code) // 16}]; }};",
             *[f"layout(location={i}) out vec4 colour{i};" for i in range(4)], REGISTERS]
    sampler_names = {1066: "sampler2D", 1067: "sampler3D", 1068: "sampler2DRect"}
    lines += [f"layout(binding={unit}) uniform {sampler_names[kind]} tex{unit};"
              for unit, kind in sorted(samplers.items())]
    if any(kind != 1067 for kind in samplers.values()):
        lines += ["uniform vec4 textureTransform[16];", "uniform vec2 texturePeriod[16];",
                  "vec2 textureCoordinate(int unit, vec2 coordinate) {",
                  "    vec2 period = texturePeriod[unit];",
                  "    if (period.x > 0.0) coordinate = mod(coordinate, period);",
                  "    return fma(coordinate, textureTransform[unit].xy, textureTransform[unit].zw);", "}"]
    lines += ["void main() {", "for (int i=0; i<64; ++i) registers[i] = uvec4(0u);",
              "vec4 cc[2]; cc[0]=vec4(0.0); cc[1]=vec4(0.0);"]
    if not program.domain["depth_replace"]:
        lines.append("registers[1].z = floatBitsToUint(gl_FragCoord.z);")
    for instruction in program.instructions:
        dest, src0, src1, src2 = instruction.words
        name = instruction.mnemonic
        if name in ("NOP", "FENCT", "FENCB"):
            continue
        operands = {"MOV": 1, "FRC": 1, "FLR": 1, "RCP": 1, "RSQ": 1, "EX2": 1,
                    "LG2": 1, "SIN": 1, "COS": 1, "TEX": 1, "MAD": 3}.get(name, 2)
        lines.append(f"{{ // Original instruction at {instruction.offset:#x}: {name}")
        for index in range(operands):
            lines.append(f"precise vec4 {'abc'[index]} = {operand(program, instruction, index)};")
        if name == "TEX":
            unit = field(dest, 17, 4)
            if unit not in samplers:
                raise ValueError(f"Texture unit {unit} has no original binding")
            coordinate = "a.xyz" if samplers[unit] == 1067 else (
                f"textureCoordinate({unit}, a.xy)")
            expression = f"texture(tex{unit}, {coordinate})"
            if field(dest, 21):
                expression = f"({expression} * 2.0 - 1.0)"
        else:
            expression = vector_operation(name)
        scale = field(src1, 28, 3)
        if scale == 4:
            raise ValueError("Reserved RSX result scale")
        if scale:
            expression = f"({expression} * {2.0 ** (scale if scale < 4 else 4 - scale)})"
        if field(dest, 7):
            expression = clamp_precision(expression, 1)
        if field(dest, 31):
            expression = clamp_precision(expression, 4)
        elif field(dest, 22, 2) > 1:
            expression = clamp_precision(expression, field(dest, 22, 2))
        lines.append(f"precise vec4 value = {expression};")
        condition = predicate(field(src0, 18, 3),
                              f"cc[{field(src0, 31)}].{swizzle(src0, (21, 23, 25, 27))}")
        lines.append(f"bvec4 enabled = {condition};")
        mask = field(dest, 9, 4)
        for index in range(4):
            if not mask & (1 << index):
                lines.append(f"enabled[{index}] = false;")
        half, register = bool(field(dest, 7)), field(dest, 1, 6)
        if not field(dest, 30):
            lines.append(f"write{'H' if half else 'R'}({register}, value, enabled);")
        if field(dest, 8):
            source = "value" if field(dest, 30) else f"read{'H' if half else 'R'}({register})"
            for index in range(4):
                if mask & (1 << index):
                    guard = f"if (enabled[{index}]) " if field(dest, 30) else ""
                    lines.append(f"{guard}cc[{field(src0, 30)}][{index}] = {source}[{index}];")
        lines.append("}")
    exports = (0, 4, 6, 8) if program.domain["output_from_h0"] else (0, 2, 3, 4)
    for target, register in enumerate(exports):
        lines.append(f"colour{target} = read{'H' if program.domain['output_from_h0'] else 'R'}({register});")
    if program.domain["depth_replace"]:
        lines.append("gl_FragDepth = readR(1).z;")
    lines.append("}")
    return "\n".join(lines) + "\n"
