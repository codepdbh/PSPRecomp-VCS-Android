"""Embed the Vulkan preview shaders after compiling them with NDK glslc."""

from pathlib import Path
import struct
import sys


def main() -> None:
    if len(sys.argv) != 5:
        raise SystemExit("usage: embed_vulkan_shaders.py VERT.spv FRAG.spv TEXTURED.spv OUTPUT.hpp")
    output = ["#pragma once", "#include <cstdint>", "namespace vcs::vulkan_shaders {"]
    for name, path in (("vertex", Path(sys.argv[1])), ("fragment", Path(sys.argv[2])),
                       ("textured_fragment", Path(sys.argv[3]))):
        data = path.read_bytes()
        if len(data) % 4 or data[:4] != b"\x03\x02\x23\x07":
            raise SystemExit(f"invalid SPIR-V: {path}")
        words = struct.unpack(f"<{len(data) // 4}I", data)
        output.append(f"inline constexpr std::uint32_t {name}[] = {{")
        for offset in range(0, len(words), 8):
            output.append("    " + ", ".join(f"0x{word:08x}u" for word in words[offset:offset + 8]) + ",")
        output.append("};")
    output.append("} // namespace vcs::vulkan_shaders")
    Path(sys.argv[4]).write_text("\n".join(output) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
