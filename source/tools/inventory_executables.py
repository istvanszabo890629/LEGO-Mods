"""Inventory EXE files as data; never load or execute their machine code.

Python 3 standard library only. PE32 and PE32+ are both supported. Observed
sections, entropy and imports do not establish compatibility or maliciousness.
"""

from __future__ import annotations

import argparse
from collections import Counter
import hashlib
import json
import math
from pathlib import Path
import struct
import sys


class PEFormatError(ValueError):
    """An input does not contain readable, sufficiently complete PE metadata."""


def entropy(data: bytes) -> float:
    if not data:
        return 0.0
    count = len(data)
    return -sum((n / count) * math.log2(n / count)
                for n in Counter(data).values())


class PEReader:
    def __init__(self, data: bytes):
        self.data = data
        self.sections: list[dict] = []
        self.header_size = 0

    def unpack(self, fmt: str, offset: int):
        length = struct.calcsize(fmt)
        if offset < 0 or offset + length > len(self.data):
            raise PEFormatError(f"Metadata outside file at offset 0x{offset:x}")
        return struct.unpack_from(fmt, self.data, offset)

    def rva_offset(self, rva: int, length: int = 1) -> int:
        if length < 0:
            raise PEFormatError("Negative requested data length")
        if 0 <= rva and rva + length <= min(self.header_size, len(self.data)):
            return rva
        for section in self.sections:
            delta = rva - section["rva"]
            if 0 <= delta and delta + length <= section["raw_size"]:
                offset = section["raw_offset"] + delta
                if offset + length <= len(self.data):
                    return offset
        raise PEFormatError(f"RVA 0x{rva:x} has no complete on-disk data")

    def cstring(self, rva: int) -> str:
        offset = self.rva_offset(rva)
        # A DLL name is bounded by its containing on-disk region and 4096 bytes.
        available = min(self.header_size, len(self.data)) - offset
        for section in self.sections:
            delta = rva - section["rva"]
            if 0 <= delta < section["raw_size"]:
                available = min(section["raw_size"] - delta,
                                len(self.data) - offset)
                break
        end = self.data.find(b"\0", offset, offset + min(available, 4096))
        if end < 0:
            raise PEFormatError(f"Unterminated DLL name at RVA 0x{rva:x}")
        return self.data[offset:end].decode("ascii", errors="backslashreplace")

    def parse(self) -> dict:
        if self.data[:2] != b"MZ":
            raise PEFormatError("Missing DOS MZ signature")
        pe, = self.unpack("<I", 0x3C)
        if self.data[pe:pe + 4] != b"PE\0\0":
            raise PEFormatError("Missing PE signature")
        machine, section_count, _, _, _, optional_size, _ = self.unpack(
            "<HHIIIHH", pe + 4)
        optional = pe + 24
        if optional + optional_size > len(self.data):
            raise PEFormatError("Truncated optional header")
        magic, = self.unpack("<H", optional)
        if magic == 0x10B:
            pe_format, directory_offset, count_offset = "PE32", 96, 92
            image_base_offset, image_base_format = 28, "<I"
        elif magic == 0x20B:
            pe_format, directory_offset, count_offset = "PE32+", 112, 108
            image_base_offset, image_base_format = 24, "<Q"
        else:
            raise PEFormatError(f"Unsupported optional-header magic 0x{magic:x}")
        if optional_size < directory_offset:
            raise PEFormatError("Optional header lacks standard PE fields")
        entry_rva, = self.unpack("<I", optional + 16)
        image_base, = self.unpack(image_base_format, optional + image_base_offset)
        image_size, self.header_size = self.unpack("<II", optional + 56)
        directories, = self.unpack("<I", optional + count_offset)
        section_table = optional + optional_size
        if section_table + section_count * 40 > len(self.data):
            raise PEFormatError("Truncated section table")
        warnings = []
        entry_section = None
        for i in range(section_count):
            at = section_table + i * 40
            name = self.data[at:at + 8].rstrip(b"\0").decode(
                "ascii", errors="backslashreplace")
            virtual_size, rva, raw_size, raw = self.unpack("<IIII", at + 8)
            characteristics, = self.unpack("<I", at + 36)
            chunk = self.data[raw:raw + raw_size] if raw_size else b""
            if len(chunk) != raw_size:
                warnings.append(f"Section {name!r}: raw data extends beyond file")
            section = dict(name=name, rva=rva, virtual_size=virtual_size,
                           raw_offset=raw, raw_size=raw_size,
                           bytes_present=len(chunk), entropy=round(entropy(chunk), 8),
                           characteristics=characteristics,
                           executable=bool(characteristics & 0x20000000))
            self.sections.append(section)
            if rva <= entry_rva < rva + max(virtual_size, raw_size):
                entry_section = name

        imported_dlls = []
        if directories > 1 and optional_size >= directory_offset + 16:
            import_rva, import_size = self.unpack(
                "<II", optional + directory_offset + 8)
            if import_rva:
                try:
                    # Only IMAGE_IMPORT_DESCRIPTORs and their DLL names are
                    # read. Import thunks are not needed for this inventory.
                    count_limit = min(import_size // 20, 4096) if import_size else 4096
                    terminated = False
                    for i in range(count_limit):
                        at = self.rva_offset(import_rva + 20 * i, 20)
                        descriptor = self.unpack("<IIIII", at)
                        if not any(descriptor):
                            terminated = True
                            break
                        name_rva = descriptor[3]
                        if not name_rva:
                            raise PEFormatError("Import descriptor has no DLL name")
                        imported_dlls.append(self.cstring(name_rva))
                    if not terminated:
                        warnings.append("Import descriptor list lacks a terminator within its bounds")
                except PEFormatError as exc:
                    warnings.append(f"Incomplete import list: {exc}")

        architecture = {0x14C: "x86", 0x8664: "x64", 0xAA64: "arm64"}.get(
            machine, "unknown")
        has_bind = any(s["name"].lower() == ".bind" for s in self.sections)
        text_entropy = [s["entropy"] for s in self.sections
                        if s["name"].lower() == ".text" and s["bytes_present"]]
        text_high_entropy = any(value >= 7.9 for value in text_entropy)
        return dict(machine=machine, machine_hex=f"0x{machine:04x}",
                    architecture=architecture, pe_format=pe_format,
                    image_base=image_base, image_size=image_size,
                    entry_rva=entry_rva, entry_section=entry_section,
                    imported_dlls=imported_dlls, sections=self.sections,
                    observations=dict(has_bind_section=has_bind,
                                      text_entropy_at_least_7_9=text_high_entropy,
                                      bind_and_high_text_entropy=has_bind and text_high_entropy),
                    warnings=warnings)


def inventory(samples: Path) -> dict:
    root = samples.resolve(strict=True)
    if not root.is_dir():
        raise ValueError("--samples must identify a directory")
    paths = sorted((path for path in root.rglob("*")
                    if path.is_file() and path.suffix.lower() == ".exe"),
                   key=lambda path: path.relative_to(root).as_posix().casefold())
    records = []
    by_hash: dict[str, list[dict]] = {}
    for path in paths:
        record = dict(file=path.name, relative_path=path.relative_to(root).as_posix())
        try:
            data = path.read_bytes()
            record.update(file_size=len(data), sha256=hashlib.sha256(data).hexdigest())
            by_hash.setdefault(record["sha256"], []).append(record)
            record.update(PEReader(data).parse(), status="parsed")
        except (OSError, PEFormatError) as exc:
            record.update(status="error", error=f"{type(exc).__name__}: {exc}")
        records.append(record)
    duplicates = [dict(sha256=sha, file_size=items[0]["file_size"],
                       files=[item["relative_path"] for item in items])
                  for sha, items in sorted(by_hash.items()) if len(items) > 1]
    return dict(schema_version=1, sample_root=str(root), samples_executed=False,
                interpretation="PE metadata observations only; no compatibility or malware verdict.",
                file_count=len(records), parsed_count=sum(r["status"] == "parsed" for r in records),
                error_count=sum(r["status"] == "error" for r in records),
                unique_content_count=len(by_hash), duplicate_groups=duplicates,
                files=records)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--samples", type=Path, required=True,
                        help="Recursively scan EXE files below this directory")
    parser.add_argument("--output", type=Path, required=True, help="JSON report to write")
    args = parser.parse_args()
    try:
        report = inventory(args.samples)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(report, indent=2, ensure_ascii=False) + "\n",
                               encoding="utf-8")
    except (OSError, ValueError) as exc:
        parser.error(str(exc))
    print(f"Inventoried {report['file_count']} EXEs: {report['parsed_count']} parsed, "
          f"{report['error_count']} errors, {len(report['duplicate_groups'])} duplicate groups.")
    print(f"Report: {args.output}")
    return 2 if report["error_count"] else 0


if __name__ == "__main__":
    sys.exit(main())
