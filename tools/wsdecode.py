#!/usr/bin/env python3
"""wsdecode.py - decode a wscap.py capture: HTTP upgrade, WebSocket frames, CBOR.

    wsdecode.py <capture-file> [--conn N] [--frames-dir DIR] [--redact TOKEN]

Prints the upgrade exchange, then one line per frame (time, direction, opcode,
flags, length) followed by the CBOR payload in diagnostic notation.  With
--frames-dir, writes each binary payload as <conn>-<seq>-<dir>.cbor (golden
fixtures).  --redact replaces a token string in the printed headers and payloads.
"""
import argparse
import json
import os
import struct
import sys
import zlib


class CborError(Exception):
    pass


def cbor_decode(buf, i=0):
    """Return (value, next_index).  Major types 0-7; tags are kept as ('tag', n, v)."""
    if i >= len(buf):
        raise CborError("truncated")
    ib = buf[i]
    mt, ai = ib >> 5, ib & 31
    i += 1
    if mt == 7:
        if ai == 20: return False, i
        if ai == 21: return True, i
        if ai == 22: return None, i
        if ai == 23: return ("undefined",), i
        if ai == 25:
            return ("f16", struct.unpack(">e", buf[i:i + 2])[0]), i + 2
        if ai == 26:
            return ("f32", struct.unpack(">f", buf[i:i + 4])[0]), i + 4
        if ai == 27:
            return ("f64", struct.unpack(">d", buf[i:i + 8])[0]), i + 8
        raise CborError("simple %d" % ai)
    if ai < 24:
        n = ai
    elif ai in (24, 25, 26, 27):
        w = 1 << (ai - 24)
        n = int.from_bytes(buf[i:i + w], "big")
        i += w
    elif ai == 31:
        n = None
    else:
        raise CborError("reserved ai %d" % ai)
    if mt == 0: return n, i
    if mt == 1: return -1 - n, i
    if mt in (2, 3):
        if n is None:
            raise CborError("indefinite string not supported")
        s = buf[i:i + n]
        return (bytes(s) if mt == 2 else s.decode("utf-8")), i + n
    if mt == 4:
        out = []
        for _ in range(n if n is not None else 1 << 30):
            if n is None and buf[i] == 0xff:
                return ("indef-array", out), i + 1
            v, i = cbor_decode(buf, i)
            out.append(v)
        return out, i
    if mt == 5:
        out = []
        for _ in range(n if n is not None else 1 << 30):
            if n is None and buf[i] == 0xff:
                return ("indef-map", out), i + 1
            k, i = cbor_decode(buf, i)
            v, i = cbor_decode(buf, i)
            out.append((k, v))
        return dict_or_pairs(out), i
    if mt == 6:
        v, i = cbor_decode(buf, i)
        return ("tag", n, v), i
    raise CborError("major type %d" % mt)


def dict_or_pairs(pairs):
    return {"__map__": pairs}


def diag(v, ind=0, redact=None):
    """CBOR diagnostic-ish notation, showing encoded widths of floats."""
    pad = "  " * ind
    if isinstance(v, dict) and "__map__" in v:
        if not v["__map__"]:
            return "{}"
        lines = ["{"]
        for k, x in v["__map__"]:
            lines.append("%s  %s: %s," % (pad, diag(k, ind + 1, redact), diag(x, ind + 1, redact)))
        lines.append(pad + "}")
        return "\n".join(lines)
    if isinstance(v, list):
        if all(not isinstance(x, (list, dict, tuple)) for x in v) and len(v) < 40:
            return "[" + ", ".join(diag(x, ind, redact) for x in v) + "]"
        return "[\n" + ",\n".join(pad + "  " + diag(x, ind + 1, redact) for x in v) + "\n" + pad + "]"
    if isinstance(v, tuple):
        if v[0] in ("f16", "f32", "f64"):
            return "%r_%s" % (v[1], v[0])
        if v[0] == "tag":
            return "%d(%s)" % (v[1], diag(v[2], ind, redact))
        return repr(v)
    if isinstance(v, bytes):
        return "h'%s'" % v.hex()
    if isinstance(v, str):
        if redact and redact in v:
            v = v.replace(redact, "<token>")
        return json.dumps(v)
    return repr(v)


def parse_frames(stream):
    """Yield (fin, rsv1, opcode, masked, payload, consumed_upto) for complete frames."""
    i = 0
    while i + 2 <= len(stream):
        b0, b1 = stream[i], stream[i + 1]
        fin, rsv1, op = b0 >> 7, (b0 >> 6) & 1, b0 & 15
        masked, n = b1 >> 7, b1 & 127
        j = i + 2
        if n == 126:
            if j + 2 > len(stream): return
            n = struct.unpack(">H", stream[j:j + 2])[0]; j += 2
        elif n == 127:
            if j + 8 > len(stream): return
            n = struct.unpack(">Q", stream[j:j + 8])[0]; j += 8
        key = b""
        if masked:
            if j + 4 > len(stream): return
            key = stream[j:j + 4]; j += 4
        if j + n > len(stream): return
        p = bytearray(stream[j:j + n])
        if masked:
            for k in range(n):
                p[k] ^= key[k & 3]
        yield fin, rsv1, op, masked, bytes(p), j + n
        i = j + n


OPS = {0: "cont", 1: "text", 2: "binary", 8: "close", 9: "ping", 10: "pong"}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("capture")
    ap.add_argument("--conn", type=int)
    ap.add_argument("--frames-dir")
    ap.add_argument("--redact")
    a = ap.parse_args()
    chunks = [json.loads(l) for l in open(a.capture)]
    conns = sorted({c["c"] for c in chunks})
    for cid in conns:
        if a.conn and cid != a.conn:
            continue
        print("=" * 20, "connection", cid)
        bufs = {"a2h": bytearray(), "h2a": bytearray()}
        hdr_done = {"a2h": False, "h2a": False}
        deflate = False
        seq = 0
        events = []
        for c in chunks:
            if c["c"] != cid:
                continue
            d = c["d"]
            bufs[d] += bytes.fromhex(c["x"])
            if not hdr_done[d]:
                end = bufs[d].find(b"\r\n\r\n")
                if end < 0:
                    continue
                head = bufs[d][:end].decode("latin-1")
                if a.redact:
                    head = head.replace(a.redact, "<token>")
                print("--- %s HTTP (t=%s)\n%s" % (d, c["t"], head))
                if d == "h2a" and "permessage-deflate" in head.lower():
                    deflate = True
                del bufs[d][:end + 4]
                hdr_done[d] = True
            used = 0
            for fin, rsv1, op, masked, p, upto in parse_frames(bufs[d]):
                used = upto
                seq += 1
                if rsv1 and deflate:
                    p = zlib.decompressobj(-15).decompress(p + b"\x00\x00\xff\xff")
                print("--- t=%-8s %s #%d %s fin=%d rsv1=%d mask=%d len=%d" %
                      (c["t"], d, seq, OPS.get(op, op), fin, rsv1, masked, len(p)))
                if op == 2:
                    if a.frames_dir:
                        os.makedirs(a.frames_dir, exist_ok=True)
                        with open(os.path.join(a.frames_dir, "%d-%03d-%s.cbor" % (cid, seq, d)), "wb") as f:
                            f.write(p)
                    try:
                        v, end = cbor_decode(p)
                        print(diag(v, 0, a.redact))
                        if end != len(p):
                            print("!! %d trailing bytes" % (len(p) - end))
                    except (CborError, IndexError, UnicodeDecodeError) as e:
                        print("!! CBOR error: %s; hex %s" % (e, p[:64].hex()))
                elif op == 8:
                    code = struct.unpack(">H", p[:2])[0] if len(p) >= 2 else None
                    print("close code=%s reason=%r" % (code, p[2:].decode("utf-8", "replace")))
                elif p:
                    print("payload %r" % p[:64])
            del bufs[d][:used]


if __name__ == "__main__":
    main()
