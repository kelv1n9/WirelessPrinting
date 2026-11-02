#!/usr/bin/env python3
"""Asks Marlin's own reference sender whether our packets are the packets it would build.

Both our sender and our simulated printer were written from one reading of the protocol,
so a misreading would agree with itself and the tests would still pass. This reads nothing:
it imports buildroot/share/scripts/MarlinBinaryProtocol.py out of the user's Marlin tree
and rebuilds every packet with their code, then compares bytes.

    test/check_against_marlin.py /tmp/wire.bin [path to Marlin]
"""

import importlib.util, os, sys

MARLIN = sys.argv[2] if len(sys.argv) > 2 else os.path.expanduser(
    "~/Documents/My Projects/Kingroon KP3S/KP3S/Marlin-2.1.2")
REFERENCE = os.path.join(MARLIN, "buildroot/share/scripts/MarlinBinaryProtocol.py")

spec = importlib.util.spec_from_file_location("MarlinBinaryProtocol", REFERENCE)
mbp = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mbp)

theirs = object.__new__(mbp.Protocol)        # their builder, without opening a serial port
theirs.max_block_size = 96

stream = open(sys.argv[1], "rb").read()
failures = []
packets = 0
order = []

at = 0
if stream.startswith(b"M28B1\n"):
    at = len(b"M28B1\n")
else:
    failures.append("the stream does not begin by switching the printer to binary")

NAMES = {(0, 1): "control sync", (0, 2): "control close", (1, 0): "query", (1, 1): "open",
         (1, 2): "close", (1, 3): "write", (1, 4): "abort"}

while at < len(stream):
    if stream[at:at + 2] != b"\xad\xb5":
        failures.append(f"offset {at}: no start token, found {stream[at:at+2].hex()}")
        break
    sync = stream[at + 2]
    meta = stream[at + 3]
    size = int.from_bytes(stream[at + 4:at + 6], "little")
    protocol, kind = (meta >> 4) & 0xF, meta & 0xF
    end = at + 8 + (size + 2 if size else 0)
    ours = stream[at:end]

    theirs.sync = sync
    rebuilt = bytes(theirs.build_packet(protocol, kind, bytearray(stream[at + 8:at + 8 + size])))

    if rebuilt != ours:
        failures.append(f"offset {at}: {NAMES.get((protocol, kind), 'unknown')} packet differs\n"
                        f"    ours   {ours.hex()}\n    theirs {rebuilt.hex()}")
    body = stream[at + 8:at + 8 + size]
    order.append((protocol, kind))

    # The framing is theirs now, but the open payload is ours to get right: their
    # FileTransferProtocol.open builds dummy, compression, filename, terminator.
    if (protocol, kind) == (1, 1):
        if body[:1] != b"\x00":
            failures.append("the open packet asks for a dummy transfer, which writes nothing")
        if body[1:2] != b"\x00":
            failures.append("the open packet asks for compression, and Marlin ships only a decoder")
        if not body.endswith(b"\x00"):
            failures.append("the filename in the open packet has no terminator")
    if (protocol, kind) == (1, 3) and size > 96:
        failures.append(f"a write packet carries {size} bytes, past Marlin's line buffer")

    packets += 1
    at = end

# The order their copy() goes in: sync, open, writes, close, and out of binary mode
if order[:2] != [(0, 1), (1, 1)]:
    failures.append(f"the conversation does not open with a sync and an open: {order[:2]}")
if order[-2:] != [(1, 2), (0, 2)]:
    failures.append(f"the conversation does not end by closing the file and the mode: {order[-2:]}")
if not any(step == (1, 3) for step in order):
    failures.append("nothing was ever written")

print(f"{packets} packets checked against {os.path.basename(REFERENCE)}")
for trouble in failures:
    print("  FAIL ", trouble)
print("\nall packets match Marlin's own builder" if not failures
      else f"\n{len(failures)} packets differ")
sys.exit(1 if failures else 0)
