#!/usr/bin/env python3
"""Build the isolated upright Super Stalker Actor sidecar.

The only retail-authored Super Stalker Actor recovered by the corpus is named
``spawn_super_stalker_legless_shack3_01`` and contains the crawl-routing
commands.  It is a valid Actor, but deliberately the wrong variant for a
general randomizer.

This tool starts from the proven full *upright* Stalker Actor sidecar and
changes only its Actor identity plus the model/model-stream references.  The
Stalker controller therefore continues to own navigation, vent exit and
animation state, while the model comes from DS3's original Super Stalker
character package.  No retail file is modified.
"""

from __future__ import annotations

import hashlib
import struct
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SOURCE = (
    ROOT.parent / "trainer" / "DS3_Debug_Menu_0.7.22_Source" / "release"
    / "ds3_debug_assets" / "authored" / "stalker_077.str"
)
OUTPUT = ROOT / "assets" / "ds3_debug_assets" / "authored" / "super_stalker_upright.str"

# The first value is the original 0.7.7 artifact manifest.  The second is the
# audited 0.7.22 release copy in this workspace; it has the same reviewed
# Actor #5 contract but a different container identity history.
SOURCE_SHA256S = {
    "091909F3C00AF2125ADFC5491E0FA09B8B1451D408C00602B8E68F1083904826",
    "2E70F013EC1089D5DFA0843233C37D5F0DF3D15BE69621C44BEFA101AEE008A7",
}
SOURCE_SIMGROUP_GUID = b"{C814AC57-0004-0770-4453-334445425547}"
SOURCE_SIMGROUP_HASH = 0xD377F004
SIDECAR_SIMGROUP_HASH = 0xD378F107

# A new entity identity avoids shadowing the original plaza Stalker if that
# chapter happens to be live.  The values are fixed so all clients load the
# identical SimManager definition.
UPRIGHT_ACTOR_UID = (0x4ECDB2E0, 0xF1A6C789, 0x53315357, 0x4952414D)

NORMAL_ACTOR_UID = (0x45CCF5B5, 0xF03B3965, 0x4A315357, 0x48475548)
NORMAL_MODEL = (0x13E8C2C5, 0x13E8C2C5, 0x13E8C2C5, 0x0A90A865)
NORMAL_MODEL_STREAM = (0x39CA4211, 0x7DBD0F44, 0x574F5242, 0x3130314E)
SUPER_MODEL = (0x3710E78B, 0x3710E78B, 0x3710E78B, 0xB238423B)
SUPER_MODEL_STREAM = (0x58CB2E99, 0x5FCA306F, 0x4E4F574B, 0x35373230)

MAGIC = 0xCCB51828
STRIDE = 0x44
REF_ARRAY = 0x3C


def hash_name(name: str) -> int:
    value = 0
    for character in name:
        code = ord(character)
        if 0x41 <= code <= 0x5A:
            code += 0x20
        value = (value * 0x1003F + code) & 0xFFFFFFFF
    return value


def guid_hash(words: tuple[int, int, int, int]) -> int:
    value = 0
    for word in words:
        value = (value * 33 + word) & 0xFFFFFFFF
    return value


def synthetic_guid(ordinal: int, desired_hash: int) -> bytes:
    word1 = ordinal | (0x0780 << 16)
    tail = bytes.fromhex("4453334445425547")
    word2 = int.from_bytes(tail[:4], "little")
    word3 = int.from_bytes(tail[4:], "little")
    constant = (((word1 * 33) + word2) * 33 + word3) & 0xFFFFFFFF
    word0 = ((desired_hash - constant) * pow(33 ** 3, -1, 1 << 32)) & 0xFFFFFFFF
    text = f"{{{word0:08X}-{ordinal:04X}-0780-4453-334445425547}}".encode("ascii")
    if len(text) != 38:
        raise ValueError("unexpected synthetic GUID length")
    return text


def find_single(data: bytes | bytearray, needle: bytes, label: str) -> int:
    first = data.find(needle)
    if first < 0 or data.find(needle, first + 1) >= 0:
        raise ValueError(f"{label} must occur exactly once")
    return first


def find_stalker_simgroup(data: bytearray) -> int:
    """Find the real header, not an incidental copy of the magic value."""
    needle = struct.pack("<I", MAGIC)
    cursor = 0
    matches: list[int] = []
    while True:
        magic = data.find(needle, cursor)
        if magic < 0:
            break
        basis = magic - 0x24
        cursor = magic + 1
        if basis < 0 or basis + 0x2C > len(data):
            continue
        if struct.unpack_from("<I", data, basis + 0x28)[0] != 0x00010003:
            continue
        instance = basis + struct.unpack_from("<I", data, basis + 0x0C)[0]
        entities = struct.unpack_from("<H", data, basis + 0x16)[0]
        if entities != 26 or instance + 6 * STRIDE > len(data):
            continue
        if struct.unpack_from("<IIII", data, instance + 5 * STRIDE + 0x0C) == NORMAL_ACTOR_UID:
            matches.append(basis)
    if len(matches) != 1:
        raise ValueError(f"expected one reviewed Stalker SimGroup, got {len(matches)}")
    return matches[0]


def locate_records(data: bytearray, basis: int) -> tuple[int, int, int, list[tuple[int, int, int]]]:
    payload = basis + struct.unpack_from("<I", data, basis + 0x04)[0]
    instance = basis + struct.unpack_from("<I", data, basis + 0x0C)[0]
    packet_count = struct.unpack_from("<H", data, basis + 0x14)[0]
    refs = [struct.unpack_from("<I", data, basis + REF_ARRAY + index * 4)[0]
            for index in range(packet_count)]
    start = max(basis + REF_ARRAY + packet_count * 4, payload)
    record_start = -1
    for candidate in range(start, len(data) - packet_count * 12 + 1, 4):
        if all(struct.unpack_from("<I", data, candidate + index * 12)[0] == refs[index]
               for index in range(min(6, packet_count))):
            record_start = candidate
            break
    if record_start < 0:
        raise ValueError("packet-record table not found")
    records = [struct.unpack_from("<III", data, record_start + index * 12)
               for index in range(packet_count)]
    payload_offsets = sorted({payload + rel for _, _, rel in records if rel})
    value_table = record_start + packet_count * 12
    value_count = (payload_offsets[0] - value_table) // 12 if payload_offsets else 0
    return payload, instance, value_count, records


def set_guid_field(data: bytearray, basis: int, instance: int, records: list[tuple[int, int, int]],
                   value_count: int, entity_index: int, field_name: str,
                   expected: tuple[int, int, int, int], replacement: tuple[int, int, int, int]) -> None:
    entity = instance + entity_index * STRIDE
    packet_ref = struct.unpack_from("<I", data, entity + 0x08)[0]
    packet_index = (packet_ref - REF_ARRAY) // 4
    if packet_index < 0 or packet_index >= len(records):
        raise ValueError("target actor packet is invalid")
    _, flags, rel = records[packet_index]
    payload = basis + struct.unpack_from("<I", data, basis + 0x04)[0]
    packet = payload + rel
    wanted = hash_name(field_name)
    value_table = (min(
        index for index in range(basis + REF_ARRAY + len(records) * 4, len(data) - len(records) * 12 + 1, 4)
        if all(struct.unpack_from("<I", data, index + part * 12)[0] == records[part][0]
               for part in range(min(6, len(records))))
    ) + len(records) * 12)
    for cursor in range(packet, packet + (flags >> 16) * 2 - 1, 2):
        value_index = struct.unpack_from("<H", data, cursor)[0]
        if value_index >= value_count:
            continue
        record = value_table + value_index * 12
        field, type_flags, relative = struct.unpack_from("<III", data, record)
        if field != wanted or ((type_flags >> 16) & 0xFF) != 13:
            continue
        address = record + 8 + relative
        observed = struct.unpack_from("<IIII", data, address)
        if observed != expected:
            raise ValueError(f"{field_name}: source GUID mismatch: {observed!r}")
        struct.pack_into("<IIII", data, address, *replacement)
        return
    raise ValueError(f"{field_name}: GUID field missing")


def build() -> None:
    source = SOURCE.read_bytes()
    if hashlib.sha256(source).hexdigest().upper() not in SOURCE_SHA256S:
        raise ValueError("stalker_077.str SHA-256 does not match the reviewed source")
    data = bytearray(source)
    basis = find_stalker_simgroup(data)

    new_simgroup_guid = synthetic_guid(0x0107, SIDECAR_SIMGROUP_HASH)
    offset = find_single(data, SOURCE_SIMGROUP_GUID, "source SimGroup GUID")
    data[offset:offset + len(SOURCE_SIMGROUP_GUID)] = new_simgroup_guid
    # Older 0.7.7 sidecars preserve an ASCII dictionary copy; the compact
    # 0.7.22 release container does not.  Update it when present, but do not
    # invent bytes in the compact layout.
    old_ascii_hash = f"{SOURCE_SIMGROUP_HASH:08x}".encode("ascii")
    ascii_offset = data.find(old_ascii_hash)
    if ascii_offset >= 0:
        if data.find(old_ascii_hash, ascii_offset + 1) >= 0:
            raise ValueError("source ASCII SimGroup hash is ambiguous")
        data[ascii_offset:ascii_offset + len(old_ascii_hash)] = (
            f"{SIDECAR_SIMGROUP_HASH:08x}".encode("ascii")
        )
    old_hash = struct.pack("<I", SOURCE_SIMGROUP_HASH)
    offset = find_single(data, old_hash, "source SimGroup hash")
    data[offset:offset + 4] = struct.pack("<I", SIDECAR_SIMGROUP_HASH)

    _, instance, value_count, records = locate_records(data, basis)
    actor_index = 5
    actor = instance + actor_index * STRIDE
    if struct.unpack_from("<IIII", data, actor + 0x0C) != NORMAL_ACTOR_UID:
        raise ValueError("reviewed upright Stalker Actor is no longer entity #5")
    struct.pack_into("<IIII", data, actor + 0x0C, *UPRIGHT_ACTOR_UID)
    struct.pack_into("<I", data, actor + 0x1C, guid_hash(UPRIGHT_ACTOR_UID))

    set_guid_field(data, basis, instance, records, value_count, actor_index,
                   "CMD_SetModel", NORMAL_MODEL, SUPER_MODEL)
    set_guid_field(data, basis, instance, records, value_count, actor_index,
                   "CMD_SetModelStream", NORMAL_MODEL_STREAM, SUPER_MODEL_STREAM)

    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    OUTPUT.write_bytes(data)
    print(f"built: {OUTPUT}")
    print(f"sha256: {hashlib.sha256(data).hexdigest().upper()}")
    print("actor UID: " + "-".join(f"{word:08X}" for word in UPRIGHT_ACTOR_UID))
    print(f"actor hash: {guid_hash(UPRIGHT_ACTOR_UID):08X}")
    print(f"stream GUID: {SIDECAR_SIMGROUP_HASH:08X}")


if __name__ == "__main__":
    build()
