"""Public NV40 ISA operations shared by the original-program translators.

Field references: RPCS3 RSX{Fragment,Vertex}Program.h; NVIDIA's
NV_fragment_program2 and NV_vertex_program3 specifications.
"""

from cg import field


CHANNELS = "xyzw"
HEADER = "#version 430 core\n"


def varyings(direction):
    return "\n".join(f"layout(location={i}) {direction} vec4 varying{i};" for i in range(13)) + "\n"


def clamp_precision(expression, precision):
    ranges = {1: ("-65504.0", "65504.0"), 2: ("-2.0", "2.0"),
              3: ("-1.0", "1.0"), 4: ("0.0", "1.0")}
    if precision == 0:
        return expression
    if precision not in ranges:
        raise ValueError(f"Unsupported RSX precision modifier {precision}")
    low, high = ranges[precision]
    return f"clamp({expression}, {low}, {high})"


def predicate(condition, value):
    if condition == 0:
        return "bvec4(false)"
    if condition == 7:
        return "bvec4(true)"
    operation = {1: "lessThan", 2: "equal", 3: "lessThanEqual", 4: "greaterThan",
                 5: "notEqual", 6: "greaterThanEqual"}[condition]
    return f"{operation}({value}, vec4(0.0))"


def vector_operation(mnemonic, a="a", b="b", c="c", vertex=False):
    operations = {
        "MOV": a, "MUL": f"({a} * {b})", "ADD": f"({a} + {c if vertex else b})",
        "MAD": f"fma({a}, {b}, {c})", "MIN": f"min({a}, {b})", "MAX": f"max({a}, {b})",
        "FRC": f"fract({a})", "FLR": f"floor({a})",
        "DP2": f"vec4(dot({a}.xy, {b}.xy))", "DP3": f"vec4(dot({a}.xyz, {b}.xyz))",
        "DP4": f"vec4(dot({a}, {b}))", "DPH": f"vec4(dot({a}.xyz, {b}.xyz) + {b}.w)",
        "DIV": f"({a} / {b}.x)", "DIVSQ": f"({a} * inversesqrt({b}.x))",
        "RCP": f"vec4(1.0 / {a}.x)", "RSQ": f"vec4(inversesqrt(abs({a}.x)))",
        "EX2": f"vec4(exp2({a}.x))", "LG2": f"vec4(log2({a}.x))",
        "SIN": f"vec4(sin({a}.x))", "COS": f"vec4(cos({a}.x))",
        "SLT": f"vec4(lessThan({a}, {b}))", "SGE": f"vec4(greaterThanEqual({a}, {b}))",
        "SGT": f"vec4(greaterThan({a}, {b}))", "SEQ": f"vec4(equal({a}, {b}))",
    }
    if mnemonic not in operations:
        raise ValueError(f"Unsupported original RSX arithmetic operation: {mnemonic}")
    return operations[mnemonic]


def swizzle(word, shifts):
    return "".join(CHANNELS[field(word, shift, 2)] for shift in shifts)


def uint_vector(words):
    return "uvec4(" + ", ".join(f"0x{word:08x}u" for word in words) + ")"
