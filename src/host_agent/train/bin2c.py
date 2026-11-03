"""二进制转 4 字节对齐 C 数组，供板上 ncnn load_param/load_model 内存加载。"""
from __future__ import annotations

import sys
from pathlib import Path


def write_header(src: Path, name: str, dst: Path) -> int:
    data = Path(src).read_bytes()
    lines = [
        f"// generated from {src} - do not edit\n",
        "#pragma once\n\n",
        "#if defined(_MSC_VER)\n",
        "#define BIN2C_ALIGN4 __declspec(align(4))\n",
        "#else\n",
        "#define BIN2C_ALIGN4 __attribute__((aligned(4)))\n",
        "#endif\n\n",
        f"#define {name.upper()}_LEN {len(data)}u\n\n",
        "BIN2C_ALIGN4\n",
        f"static const unsigned char {name}[] = {{\n",
    ]
    for off in range(0, len(data), 12):
        chunk = data[off : off + 12]
        lines.append("    " + ",".join(f"0x{b:02x}" for b in chunk) + ",\n")
    lines.append("};\n")
    Path(dst).write_text("".join(lines), encoding="utf-8")
    return len(data)


def main(argv: list[str] | None = None) -> int:
    argv = list(sys.argv[1:] if argv is None else argv)
    if len(argv) != 3:
        print("usage: bin2c.py <input> <array_name> <output.h>")
        return 1
    n = write_header(Path(argv[0]), argv[1], Path(argv[2]))
    print(f"wrote {argv[2]}: {argv[1]}[{n}]")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
