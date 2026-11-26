"""Write a WAP to the package slot of a newly created Quota OS data disk."""
import struct


def seed_new_disk(data, payload):
    if len(data) < 8192 or any(data[4096:8192]):
        raise ValueError("WAP package slots unavailable")
    if len(payload) > 2032:
        raise ValueError(f"{len(payload)} bytes exceed the guest package slot")
    generation = 1
    checksum = ((2166136261 ^ generation) * 16777619) & 0xffffffff
    for byte in payload:
        checksum = ((checksum ^ byte) * 16777619) & 0xffffffff
    record = struct.pack("<4I", 0x51415031, generation, len(payload), checksum) + payload
    data[4096:4096 + len(record)] = record
