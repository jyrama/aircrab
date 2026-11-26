#!/usr/bin/env python3
import argparse
import mmap
import os
import re
import socket
import struct
import subprocess
import sys
import time

VERSION_OFF = 0
MAIN_OFF = 8


def parse_header(path):
    text = open(path).read()

    def define(name):
        m = re.search(r"#define\s+" + name + r"\s+(\S+)", text)
        if not m:
            sys.exit("cannot find #define " + name + " in " + path)
        return m.group(1)

    smi = define("NQPTP_INTERFACE_NAME").strip('"')
    version = int(define("NQPTP_SHM_STRUCTURES_VERSION"))
    port = int(define("NQPTP_CONTROL_PORT"))
    m = re.search(r"typedef struct\s*\{(.*?)\}\s*shm_structure_set\s*;", text, re.S)
    if not m:
        sys.exit("cannot parse shm_structure_set in " + path)
    fields = re.findall(r"uint64_t\s+(\w+)\s*;", m.group(1))
    return smi, version, port, fields


class Harness:
    def __init__(self):
        self.failures = []

    def check(self, ok, description):
        print("  %s  %s" % ("ok  " if ok else "FAIL", description))
        if not ok:
            self.failures.append(description)
        return ok


def shm_path(name):
    return "/dev/shm/" + name.lstrip("/")


def read_region(name, size):
    with open(shm_path(name), "rb") as f:
        return f.read(size)


def poison_master_clock(name, value, main_off, secondary_off):
    fd = os.open(shm_path(name), os.O_RDWR)
    try:
        region = mmap.mmap(fd, 0)
        struct.pack_into("<Q", region, main_off, value)
        struct.pack_into("<Q", region, secondary_off, value)
        region.flush()
        region.close()
    finally:
        os.close(fd)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--mode", choices=["capture", "check"], default="check")
    ap.add_argument("--binary", required=True)
    ap.add_argument("--header", required=True)
    ap.add_argument("--golden", required=True)
    ap.add_argument("--ptp-ports", default="1234,1235")
    args = ap.parse_args()

    smi_default, version, control_port, fields = parse_header(args.header)
    n = len(fields)
    set_size = 8 * n
    main_off = MAIN_OFF
    secondary_off = MAIN_OFF + set_size
    size = MAIN_OFF + 2 * set_size
    print("header: default smi=%s version=%d control port=%d fields=%s" %
          (smi_default, version, control_port, fields))
    print("layout: main@%d secondary@%d size=%d" % (main_off, secondary_off, size))

    h = Harness()
    os.makedirs(args.golden, exist_ok=True)

    v = subprocess.run([args.binary, "-V"], capture_output=True, text=True)
    v_out = re.sub(r"Version: \S+", "Version: <ver>", v.stdout)
    golden_v = os.path.join(args.golden, "nqptp_-V.txt")
    if args.mode == "capture":
        open(golden_v, "w").write(v_out)
        print("  captured golden: nqptp_-V.txt")
    else:
        want = open(golden_v).read() if os.path.exists(golden_v) else None
        h.check(want is not None and want == v_out,
                "-V output matches golden (smi version in output: smi%d)" % version)

    suffix = "%d" % os.getpid()
    smi_a = "/nqptp-harness-%s-a" % suffix
    smi_b = "/nqptp-harness-%s-b" % suffix
    for smi in (smi_a, smi_b):
        if os.path.exists(shm_path(smi)):
            os.unlink(shm_path(smi))

    daemon = subprocess.Popen([args.binary, "--ptp-ports=" + args.ptp_ports, "-vv"],
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

    def send(message):
        sock.sendto(message.encode() + b"\0", ("127.0.0.1", control_port))
        time.sleep(0.3)

    try:
        time.sleep(0.8)
        if daemon.poll() is not None:
            sys.exit("nqptp exited immediately:\n" + daemon.stdout.read().decode())

        send("%s T 192.168.1.209" % smi_a)
        send("%s T 192.168.1.209" % smi_b)
        h.check(os.path.exists(shm_path(smi_a)) and os.path.exists(shm_path(smi_b)),
                "each client control message created its own shm region")

        da = read_region(smi_a, size)
        db = read_region(smi_b, size)
        h.check(len(da) == size and len(db) == size,
                "regions are %d bytes (expected from header layout)" % size)
        h.check(struct.unpack_from("<H", da, VERSION_OFF)[0] == version and
                struct.unpack_from("<H", db, VERSION_OFF)[0] == version,
                "both regions carry SMI version %d from the header" % version)

        poison_master_clock(smi_a, 0xAAAAAAAAAAAAAAAA, main_off, secondary_off)
        poison_master_clock(smi_b, 0xBBBBBBBBBBBBBBBB, main_off, secondary_off)
        ra = struct.unpack_from("<Q", read_region(smi_a, size), main_off)[0]
        rb = struct.unpack_from("<Q", read_region(smi_b, size), main_off)[0]
        h.check(ra == 0xAAAAAAAAAAAAAAAA and rb == 0xBBBBBBBBBBBBBBBB,
                "the two regions are distinct memory")

        send("%s T" % smi_b)
        ra2 = struct.unpack_from("<Q", read_region(smi_a, size), main_off)[0]
        h.check(ra2 == 0xAAAAAAAAAAAAAAAA,
                "client B's bare T did not disturb client A's region (isolation)")

        for cmd in ("B", "P", "E"):
            send("%s %s" % (smi_a, cmd))
        h.check(daemon.poll() is None, "control commands B/P/E accepted, daemon alive")

        da = read_region(smi_a, size)
        main_set = da[main_off:main_off + set_size]
        secondary_set = da[secondary_off:secondary_off + set_size]
        h.check(main_set == secondary_set,
                "reader consistency: main and secondary records match after stable write")
    finally:
        sock.close()
        daemon.terminate()
        try:
            daemon.wait(timeout=3)
        except subprocess.TimeoutExpired:
            daemon.kill()
        for smi in (smi_a, smi_b):
            if os.path.exists(shm_path(smi)):
                os.unlink(shm_path(smi))

    if h.failures:
        print("FAILED: %d check(s)" % len(h.failures))
        sys.exit(1)
    print("all checks passed")


if __name__ == "__main__":
    main()
