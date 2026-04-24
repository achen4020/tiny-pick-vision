#!/usr/bin/env python3
"""
Parse timing.bin produced by the Android bench-test APP and report the
distribution of tpv processing time per frame.

Usage:
    python3 tools/analyze_timing.py <timing.bin>

Output: p50, p95, p99 of (t_tpv_exit_ns - t_tpv_enter_ns) in milliseconds,
plus count and pass/fail against the 30 ms p95 gate from spec §A2.
"""
import struct, sys, statistics

def main(path):
    with open(path, "rb") as f:
        data = f.read()
    magic = data[0:4]
    if magic != b"TTML":
        sys.exit(f"bad magic: {magic!r}")
    version, record_size = struct.unpack_from("<HH", data, 4)
    if version != 1 or record_size != 48:
        sys.exit(f"unsupported: version={version} size={record_size}")
    n_records = (len(data) - 32) // record_size
    print(f"records: {n_records}")

    tpv_ms = []
    for i in range(n_records):
        off = 32 + i * 48
        # frame_idx, status, cls, camera, jni_in, tpv_in, tpv_out
        fields = struct.unpack_from("<iiiQQQQ", data, off)
        tpv_in = fields[5] ; tpv_out = fields[6]
        tpv_ms.append((tpv_out - tpv_in) / 1_000_000.0)

    if not tpv_ms:
        sys.exit("no records")
    tpv_ms.sort()
    p50 = tpv_ms[int(len(tpv_ms) * 0.50)]
    p95 = tpv_ms[int(len(tpv_ms) * 0.95)]
    p99 = tpv_ms[int(len(tpv_ms) * 0.99)]
    print(f"tpv ms : p50={p50:.2f}  p95={p95:.2f}  p99={p99:.2f}")
    gate = "PASS" if p95 <= 30.0 else "FAIL"
    print(f"A2 gate (p95 ≤ 30 ms): {gate}")

if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit("usage: analyze_timing.py <timing.bin>")
    main(sys.argv[1])
