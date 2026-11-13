"""Read the SCE metadata used by the original renderer, without executing it.

Format reference: RPCS3 rpcs3/Crypto/unself.{h,cpp}, GPL-2.0-or-later.
The public firmware keys below come from RPCS3's Crypto/key_vault files.
"""

import struct
import zlib
from dataclasses import dataclass

import shutil
import subprocess
import sys

try:  # the Python cryptography package when installed, else the openssl command
    from cryptography.hazmat.primitives.ciphers import Cipher, algorithms
except ImportError:
    Cipher = None


class modes:
    """AES modes as (name, iv) for decrypt()."""
    @staticmethod
    def CBC(iv): return ("cbc", bytes(iv))
    @staticmethod
    def CTR(nonce): return ("ctr", bytes(nonce))


PACKAGE_KEY = bytes.fromhex("A97818BD193A67A16FE83A855E1BE9FB5640938D4DBCB2CB52C5A2F8B02B1031")
PACKAGE_IV = bytes.fromhex("4ACEF01224FBEEDF8245F8FF10211E6E")
APPLICATION_KEY = bytes.fromhex("CFF025375BA0079226BE01F4A31F346D79F62CFB643CA910E16CF60BD9092752")
APPLICATION_IV = bytes.fromhex("FD40664E2EBBA01BF359B0DCDF543DA4")
MAX_SECTION = 32 * 1024 * 1024


def region(data, offset, size):
    if offset < 0 or size < 0 or offset > len(data) or size > len(data) - offset:
        raise ValueError("SCE field points outside its containing buffer")
    return data[offset:offset + size]


def unpack(data, offset, layout):
    return struct.unpack(layout, region(data, offset, struct.calcsize(layout)))


def decrypt(data, key, mode):
    name, iv = mode
    if Cipher is not None:
        from cryptography.hazmat.primitives.ciphers import modes as cipher_modes
        cipher = Cipher(algorithms.AES(key), (cipher_modes.CBC if name == "cbc" else cipher_modes.CTR)(iv)).decryptor()
        return cipher.update(data) + cipher.finalize()
    if sys.platform == "win32":
        return _windows_aes(bytes(data), bytes(key), name, iv)
    if not shutil.which("openssl"):
        raise ValueError("Decrypting the update needs the openssl command or Python's cryptography package")
    result = subprocess.run(["openssl", "enc", "-d", f"-aes-{len(key) * 8}-{name}", "-K", key.hex(), "-iv", iv.hex(),
                             "-nopad"], input=bytes(data), capture_output=True)
    if result.returncode or len(result.stdout) != len(data):
        raise ValueError("openssl could not decrypt the update: " + result.stderr.decode(errors="replace").strip())
    return result.stdout


def _windows_aes(data, key, name, iv):
    """AES through Windows CNG (bcrypt.dll): CBC directly, CTR as ECB over the counter blocks."""
    import ctypes
    bcrypt = ctypes.WinDLL("bcrypt")
    def check(status):
        if status:
            raise ValueError(f"Windows could not decrypt the update (CNG status {status & 0xFFFFFFFF:#x})")
    algorithm, handle = ctypes.c_void_p(), ctypes.c_void_p()
    check(bcrypt.BCryptOpenAlgorithmProvider(ctypes.byref(algorithm), ctypes.c_wchar_p("AES"), None, 0))
    try:
        mode = ctypes.create_unicode_buffer("ChainingModeCBC" if name == "cbc" else "ChainingModeECB")
        check(bcrypt.BCryptSetProperty(algorithm, ctypes.c_wchar_p("ChainingMode"), mode, ctypes.sizeof(mode), 0))
        check(bcrypt.BCryptGenerateSymmetricKey(algorithm, ctypes.byref(handle), None, 0, key, len(key), 0))
        try:
            if name == "cbc":
                source, vector, function = data, ctypes.create_string_buffer(iv, 16), bcrypt.BCryptDecrypt
            else:
                start = int.from_bytes(iv, "big")
                blocks = (len(data) + 15) // 16
                source = b"".join(((start + i) % (1 << 128)).to_bytes(16, "big") for i in range(blocks))
                vector, function = None, bcrypt.BCryptEncrypt
            output = ctypes.create_string_buffer(len(source))
            written = ctypes.c_ulong()
            check(function(handle, source, len(source), None, vector, 16 if vector is not None else 0,
                           output, len(source), ctypes.byref(written), 0))
            result = output.raw[:written.value]
        finally:
            bcrypt.BCryptDestroyKey(handle)
    finally:
        bcrypt.BCryptCloseAlgorithmProvider(algorithm, 0)
    if name == "cbc":
        return result
    stream = result[:len(data)]
    return (int.from_bytes(data, "big") ^ int.from_bytes(stream, "big")).to_bytes(len(data), "big")


def inflate(data, maximum=MAX_SECTION):
    decoder = zlib.decompressobj()
    result = decoder.decompress(data, maximum + 1)
    if len(result) > maximum or decoder.unconsumed_tail or not decoder.eof or decoder.unused_data:
        raise ValueError("Invalid or oversized compressed SCE section")
    return result


@dataclass(frozen=True)
class Section:
    kind: int
    program: int
    data: bytes


def sections(data, key, iv, expected_type):
    magic, version, revision, kind, meta, header_size, image_size = unpack(data, 0, ">IIHHIQQ")
    if magic != 0x53434500 or version != 2 or kind != expected_type:
        raise ValueError("Unexpected SCE header")
    if expected_type == 1 and revision != 0x1C:
        raise ValueError("This importer supports the verified 4.93 application key revision only")
    region(data, 0, header_size)
    info = decrypt(region(data, meta + 32, 64), key, modes.CBC(iv))
    if info[16:32] != bytes(16) or info[48:64] != bytes(16):
        raise ValueError("SCE metadata key did not match")
    start = meta + 96
    metadata = decrypt(region(data, start, header_size - start), info[:16], modes.CTR(info[32:48]))
    _, _, count, key_count, _, _, _ = unpack(metadata, 0, ">Q6I")
    if count > 1024 or key_count > 8192:
        raise ValueError("Implausible SCE metadata table")
    key_table = region(metadata, 32 + count * 48, key_count * 16)
    for index in range(count):
        offset, length, kind, program, _, _, encrypted, key_index, iv_index, compressed = unpack(
            metadata, 32 + index * 48, ">QQ8I")
        if length > MAX_SECTION:
            raise ValueError("SCE section exceeds the import limit")
        content = region(data, offset, length)
        if encrypted == 3:
            key = region(key_table, key_index * 16, 16)
            iv = region(key_table, iv_index * 16, 16)
            content = decrypt(content, key, modes.CTR(iv))
        elif encrypted != 1:
            raise ValueError(f"Unsupported SCE encryption mode {encrypted}")
        if compressed == 2:
            content = inflate(content)
        elif compressed != 1:
            raise ValueError(f"Unsupported SCE compression mode {compressed}")
        yield Section(kind, program, content)


def package_sections(data):
    return sections(data, PACKAGE_KEY, PACKAGE_IV, 3)


def application_elf(data):
    extension = unpack(data, 32, ">10Q")
    header = region(data, extension[2], 64)
    values = unpack(header, 0, ">16sHHIQQQIHHHHHH")
    if header[:6] != b"\x7fELF\x02\x02" or values[1] != 0xFFA4 or values[2] != 21:
        raise ValueError("Expected a big-endian PPC64 PRX")
    program_offset, section_offset = values[5:7]
    program_size, program_count, section_size, section_count = values[9:13]
    if program_size != 56 or program_count > 32 or section_count > 4096:
        raise ValueError("Unsupported PRX table layout")
    programs = [unpack(data, extension[3] + index * program_size, ">II6Q")
                for index in range(program_count)]
    length = max(64, program_offset + program_count * program_size,
                 section_offset + section_count * section_size,
                 *(program[2] + program[5] for program in programs))
    if length > MAX_SECTION:
        raise ValueError("PRX image exceeds the import limit")
    image = bytearray(length)
    image[:64] = header
    image[program_offset:program_offset + program_count * program_size] = region(
        data, extension[3], program_count * program_size)
    populated = set()
    for section in sections(data, APPLICATION_KEY, APPLICATION_IV, 1):
        if section.kind != 2:
            continue
        if section.program >= len(programs) or section.program in populated:
            raise ValueError("Invalid or repeated PRX program segment")
        program = programs[section.program]
        if len(section.data) != program[5]:
            raise ValueError("Decrypted PRX segment has the wrong size")
        populated.add(section.program)
        image[program[2]:program[2] + program[5]] = section.data
    if len(populated) != program_count:
        raise ValueError("The PRX image is missing a program segment")
    if section_offset and section_count:
        image[section_offset:section_offset + section_count * section_size] = region(
            data, extension[4], section_count * section_size)
    return bytes(image)
