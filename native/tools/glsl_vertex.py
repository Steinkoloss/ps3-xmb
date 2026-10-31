"""Lower the original NV40 co-issued vertex instructions to native GLSL."""

from cg import VP_SCALAR_NAMES, VP_VECTOR_NAMES, field
from glsl_common import (CHANNELS, HEADER, clamp_precision, predicate, swizzle,
                         varyings, vector_operation)


def operand(instruction, index):
    d0, d1, d2, d3 = instruction.words
    source = ((field(d1, 0, 8) << 9) | field(d2, 23, 9), field(d2, 6, 17),
              (field(d2, 0, 6) << 11) | field(d3, 21, 11))[index]
    kind = field(source, 0, 2)
    if kind == 1:
        expression = f"r[{field(source, 2, 6)}]"
    elif kind == 2:
        expression = f"attribute{field(d1, 8, 4)}"
        if field(d0, 27):
            raise ValueError("Indexed vertex inputs require an explicit adapter")
    elif kind == 3:
        if field(d3, 1):
            raise ValueError("Indexed vertex constants require an explicit adapter")
        expression = f"vc[{field(d1, 12, 10)}]"
    else:
        raise ValueError("Invalid vertex source register type")
    expression = f"({expression}).{swizzle(source, (14, 12, 10, 8))}"
    if field(d0, 21 + index):
        expression = f"abs({expression})"
    if field(source, 16):
        expression = f"(-{expression})"
    return expression


def translate(program):
    if program.profile != 7003:
        raise ValueError("Expected an original vertex program")
    lines = [HEADER, *[f"layout(location={i}) in vec4 attribute{i};" for i in range(16)], varyings("out"),
             "layout(std140, binding=0) uniform VertexConstants { vec4 vc[512]; };",
             "void main() {", "vec4 r[64]; vec4 o[16]; vec4 cc[2];",
             "for (int i=0; i<64; ++i) r[i]=vec4(0.0);",
             "for (int i=0; i<16; ++i) o[i]=vec4(0.0, 0.0, 0.0, 1.0);",
             "cc[0]=vec4(0.0); cc[1]=vec4(0.0);"]
    for instruction in program.instructions:
        d0, d1, d2, d3 = instruction.words
        vector_name, scalar_name = VP_VECTOR_NAMES[instruction.vector_opcode], VP_SCALAR_NAMES[instruction.scalar_opcode]
        vm, sm = field(d3, 13, 4), field(d3, 17, 4)
        lines.append(f"{{ // Original instruction at {instruction.offset:#x}: {vector_name}/{scalar_name}")
        used = set()
        if vm and vector_name != "NOP":
            used |= {0}
            if vector_name not in ("MOV", "FRC", "FLR"):
                used |= {2} if vector_name == "ADD" else {1}
            if vector_name == "MAD":
                used.add(2)
        if sm and scalar_name != "NOP":
            used.add(2)
        for index in sorted(used):
            lines.append(f"precise vec4 {'abc'[index]} = {operand(instruction, index)};")
        condition = (predicate(field(d0, 10, 3), f"cc[{field(d0, 25)}].{swizzle(d0, (8, 6, 4, 2))}")
                     if field(d0, 13) else "bvec4(true)")
        lines.append(f"bvec4 enabled = {condition};")
        if vm and vector_name != "NOP":
            lines.append(f"precise vec4 vectorValue = {vector_operation(vector_name, vertex=True)};")
        if sm and scalar_name != "NOP":
            lines.append(f"precise vec4 scalarValue = {vector_operation(scalar_name, a='c')};")
        for scalar, name, mask in ((False, vector_name, vm), (True, scalar_name, sm)):
            if not mask or name == "NOP":
                continue
            expression = "scalarValue" if scalar else "vectorValue"
            if field(d0, 26):
                lines.append(f"{expression} = {clamp_precision(expression, 4)};")
            destinations = []
            output_owner = bool(field(d0, 30)) != scalar
            if output_owner and field(d3, 2, 5) != 31:
                destinations.append(f"o[{field(d3, 2, 5)}]")
            temporary = field(d3, 7, 6) if scalar else field(d0, 15, 6)
            if temporary != 63:
                destinations.append(f"r[{temporary}]")
            if field(d0, 14) and field(d0, 29):
                destinations.append(f"cc[{field(d0, 25)}]")
            for channel, bit in zip(CHANNELS, (8, 4, 2, 1)):
                if mask & bit:
                    for destination in destinations:
                        lines.append(f"if (enabled.{channel}) {destination}.{channel} = {expression}.{channel};")
        lines.append("}")
    lines += ["gl_Position=o[0];", "varying0=o[1]; varying1=o[2]; varying2=o[5];"]
    lines += [f"varying{i + 3}=o[{i + 7}];" for i in range(8)]
    lines += ["varying11=o[15]; varying12=o[6];", "}"]
    return "\n".join(lines) + "\n"
