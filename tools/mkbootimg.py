#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
#
# mkbootimg.py - Assemble the native boot disk image.
#
# Disk layout (512-byte sectors):
#   sector 0             bootsect.bin   (stage 1)
#   sectors 1..L         loader.bin     (stage 2), L = LOADER_SECTORS
#   sectors L+1..        jexos.bin      (kernel ELF)
#
# bootsect reads the loader and the loader reads the kernel via BIOS
# INT 13h AH=42h using Disk Address Packets.  The sector counts / LBAs
# are patched into the assembled binaries here, because they can only be
# known after the loader and kernel have been built.

import argparse
import os
import struct
import subprocess

SECTOR = 512
BOOTSECT_BASE = 0x7C00
LOADER_BASE = 0x7E00


def symbol_addr(elf, name):
    """Return the link-time address of a symbol, via nm."""
    out = subprocess.check_output(["nm", elf], text=True)
    for line in out.splitlines():
        try:
            addr, _type, sym = line.split(maxsplit=2)
        except ValueError:
            continue
        if sym == name:
            return int(addr, 16)
    raise SystemExit(f"mkbootimg: symbol '{name}' not found in {elf}")


def patch_file(path, offset, data):
    with open(path, "r+b") as f:
        f.seek(offset)
        f.write(data)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--bootsect", required=True, help="stage 1 binary")
    ap.add_argument("--bootsect-elf", required=True, help="stage 1 ELF (for symbols)")
    ap.add_argument("--loader", required=True, help="stage 2 binary")
    ap.add_argument("--loader-elf", required=True, help="stage 2 ELF (for symbols)")
    ap.add_argument("--kernel", required=True, help="kernel ELF")
    ap.add_argument("--output", required=True, help="output disk image")
    args = ap.parse_args()

    loader_sectors = (os.path.getsize(args.loader) + SECTOR - 1) // SECTOR
    kernel_sectors = (os.path.getsize(args.kernel) + SECTOR - 1) // SECTOR
    kernel_lba = loader_sectors + 1
    total_sectors = 1 + loader_sectors + kernel_sectors

    # bootsect reads the loader with its DAP: patch the sector count.
    bs_off = symbol_addr(args.bootsect_elf, "dap_sectors") - BOOTSECT_BASE
    patch_file(args.bootsect, bs_off, struct.pack("<H", loader_sectors))

    # loader reads the kernel with its DAP: patch count and LBA.
    ld_off = symbol_addr(args.loader_elf, "kernel_dap_sectors") - LOADER_BASE
    patch_file(args.loader, ld_off, struct.pack("<H", kernel_sectors))
    l0_off = symbol_addr(args.loader_elf, "kernel_dap_lba_0") - LOADER_BASE
    patch_file(args.loader, l0_off, struct.pack("<H", kernel_lba & 0xFFFF))
    l2_off = symbol_addr(args.loader_elf, "kernel_dap_lba_2") - LOADER_BASE
    patch_file(args.loader, l2_off, struct.pack("<H", (kernel_lba >> 16) & 0xFFFF))

    # Assemble the image (truncate first => zero-filled).
    with open(args.output, "wb") as img:
        img.truncate(total_sectors * SECTOR)
        for offset, src in ((0, args.bootsect),
                            (SECTOR, args.loader),
                            (kernel_lba * SECTOR, args.kernel)):
            with open(src, "rb") as f:
                data = f.read()
            img.seek(offset)
            img.write(data)


if __name__ == "__main__":
    main()
