#!/usr/bin/env python3
"""Extract the ISO9660 files used by a PSP UMD image (no third-party modules)."""

from __future__ import annotations

import argparse
import json
import shutil
from pathlib import Path

SECTOR_SIZE = 2048


def both_endian_32(data: bytes, offset: int) -> int:
    little = int.from_bytes(data[offset : offset + 4], "little")
    big = int.from_bytes(data[offset + 4 : offset + 8], "big")
    if little != big:
        raise ValueError("Invalid ISO9660 both-endian 32-bit field")
    return little


def directory_records(image, extent: int, length: int):
    image.seek(extent * SECTOR_SIZE)
    remaining = length
    absolute = extent * SECTOR_SIZE
    while remaining:
        chunk_size = min(SECTOR_SIZE, remaining)
        data = image.read(chunk_size)
        if len(data) != chunk_size:
            raise ValueError("Truncated ISO directory")
        pos = 0
        while pos < len(data):
            record_length = data[pos]
            if record_length == 0:
                break  # The rest of this logical sector is padding.
            record = data[pos : pos + record_length]
            if len(record) != record_length or record_length < 34:
                raise ValueError("Malformed ISO9660 directory record")
            name_length = record[32]
            raw_name = record[33 : 33 + name_length]
            if raw_name == b"\0":
                name = "."
            elif raw_name == b"\1":
                name = ".."
            else:
                name = raw_name.decode("ascii").split(";", 1)[0].rstrip(".")
            yield name, both_endian_32(record, 2), both_endian_32(record, 10), bool(record[25] & 2)
            pos += record_length
        consumed = min(SECTOR_SIZE, remaining)
        remaining -= consumed
        absolute += consumed


def find_path(image, root_extent: int, root_length: int, parts: list[str]):
    extent, length = root_extent, root_length
    record = None
    for part in parts:
        wanted = part.upper()
        record = next(
            (entry for entry in directory_records(image, extent, length) if entry[0].upper() == wanted),
            None,
        )
        if record is None:
            raise FileNotFoundError("ISO path not found: " + "/".join(parts))
        _, extent, length, is_dir = record
        if part != parts[-1] and not is_dir:
            raise ValueError("Non-directory in ISO path: " + part)
    return record, extent, length


def extract_tree(image, extent: int, length: int, destination: Path, image_size: int):
    destination.mkdir(parents=True, exist_ok=True)
    for name, child_extent, child_length, is_dir in directory_records(image, extent, length):
        if name in (".", ".."):
            continue
        # PSP ISO filenames are ASCII. Reject path syntax to keep extraction rooted.
        if "/" in name or "\\" in name or name in ("", ".", ".."):
            raise ValueError("Unsafe ISO9660 path component: " + repr(name))
        target = destination / name
        byte_offset = child_extent * SECTOR_SIZE
        if byte_offset + child_length > image_size:
            raise ValueError("ISO entry extends beyond the image: " + str(target))
        if is_dir:
            extract_tree(image, child_extent, child_length, target, image_size)
        else:
            target.parent.mkdir(parents=True, exist_ok=True)
            image.seek(byte_offset)
            with target.open("wb") as output:
                shutil.copyfileobj(_LimitedReader(image, child_length), output, length=1024 * 1024)


class _LimitedReader:
    def __init__(self, image, remaining: int):
        self.image = image
        self.remaining = remaining

    def read(self, size=-1):
        if self.remaining <= 0:
            return b""
        if size < 0 or size > self.remaining:
            size = self.remaining
        data = self.image.read(size)
        self.remaining -= len(data)
        return data


def parse_sfo(data: bytes) -> dict[str, str]:
    if len(data) < 20 or data[:4] != b"\0PSF":
        raise ValueError("PSP_GAME/PARAM.SFO is missing or invalid")
    key_offset = int.from_bytes(data[8:12], "little")
    data_offset = int.from_bytes(data[12:16], "little")
    count = int.from_bytes(data[16:20], "little")
    values = {}
    for i in range(count):
        entry = 20 + i * 16
        key_rel = int.from_bytes(data[entry : entry + 2], "little")
        value_length = int.from_bytes(data[entry + 4 : entry + 8], "little")
        value_rel = int.from_bytes(data[entry + 12 : entry + 16], "little")
        start = key_offset + key_rel
        end = data.index(b"\0", start)
        key = data[start:end].decode("ascii")
        value_start = data_offset + value_rel
        raw_value = data[value_start : value_start + value_length].rstrip(b"\0")
        if value_start + value_length > len(data):
            raise ValueError("PARAM.SFO value is out of bounds: " + key)
        values[key] = raw_value.decode("utf-8", "replace")
    return values


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("iso", type=Path)
    parser.add_argument("destination", type=Path)
    parser.add_argument("--verify-only", action="store_true")
    args = parser.parse_args()
    iso_path = args.iso.resolve(strict=True)
    destination = args.destination.resolve()
    image_size = iso_path.stat().st_size
    if destination == iso_path.parent or iso_path.is_relative_to(destination):
        parser.error("Destination must not contain or replace the ISO")

    with iso_path.open("rb") as image:
        image.seek(16 * SECTOR_SIZE)
        pvd = image.read(SECTOR_SIZE)
        if len(pvd) != SECTOR_SIZE or pvd[0] != 1 or pvd[1:6] != b"CD001" or pvd[6] != 1:
            parser.error("Image has no ISO9660 primary volume descriptor at sector 16")
        root = pvd[156:190]
        if root[0] < 34 or not root[25] & 2:
            parser.error("Invalid ISO9660 root directory record")
        root_extent = both_endian_32(root, 2)
        root_length = both_endian_32(root, 10)
        record, game_extent, game_length = find_path(
            image, root_extent, root_length, ["PSP_GAME"]
        )
        if not record[3]:
            parser.error("PSP_GAME is not an ISO directory")
        _, sfo_extent, sfo_length = find_path(
            image, game_extent, game_length, ["PARAM.SFO"]
        )
        image.seek(sfo_extent * SECTOR_SIZE)
        sfo = parse_sfo(image.read(sfo_length))
        disc_id = sfo.get("DISC_ID", "")
        if args.verify_only:
            print(json.dumps({"title": sfo.get("TITLE", ""), "disc_id": disc_id,
                              "compatible": disc_id.replace("-", "").upper() == "ULUS10160"}))
            return 0
        if disc_id.replace("-", "").upper() != "ULUS10160":
            parser.error(f"Unsupported DISC_ID {disc_id!r}; expected ULUS10160")
        extract_tree(image, game_extent, game_length, destination / "PSP_GAME", image_size)
        try:
            _, umd_extent, umd_length = find_path(
                image, root_extent, root_length, ["UMD_DATA.BIN"]
            )
        except FileNotFoundError:
            pass
        else:
            target = destination / "UMD_DATA.BIN"
            target.parent.mkdir(parents=True, exist_ok=True)
            image.seek(umd_extent * SECTOR_SIZE)
            with target.open("wb") as output:
                shutil.copyfileobj(_LimitedReader(image, umd_length), output)

    print(f"Extracted PSP_GAME from {iso_path} to {destination}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
