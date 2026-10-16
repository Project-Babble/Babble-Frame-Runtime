#!/usr/bin/env python3
"""Decrypt an uploaded dataset object (or read a package.tar) and examine all of its parts."""
import argparse
import hashlib
import json
import math
import os
import shutil
import struct
import subprocess
import sys
import tarfile
import tempfile

failures = []


def check(ok, what):
    print(("  ok    " if ok else "  FAIL  ") + what)
    if not ok:
        failures.append(what)
    return ok


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def decrypt(obj, key, out):
    with open(obj, "rb") as f:
        n = struct.unpack("<i", f.read(4))[0]
        wrapped = f.read(n)
        iv = f.read(16)
        body_at = f.tell()
    with tempfile.NamedTemporaryFile() as wk:
        wk.write(wrapped)
        wk.flush()
        aes = subprocess.run(["openssl", "pkeyutl", "-decrypt", "-inkey", key, "-pkeyopt", "rsa_padding_mode:oaep",
                              "-pkeyopt", "rsa_oaep_md:sha256", "-pkeyopt", "rsa_mgf1_md:sha256", "-in", wk.name],
                             capture_output=True, check=True).stdout
    if len(aes) != 32:
        raise RuntimeError("the unwrapped key isn't 32 bytes")
    with open(obj, "rb") as src, open(out, "wb") as dst:
        src.seek(body_at)
        p = subprocess.Popen(["openssl", "enc", "-d", "-aes-256-cbc", "-K", aes.hex(), "-iv", iv.hex()],
                             stdin=src, stdout=dst)
        if p.wait() != 0:
            raise RuntimeError("AES decryption failed (wrong key?)")


def frames(path, w, h):
    """Yields each decoded frame of a video as full-range gray bytes."""
    p = subprocess.Popen(["ffmpeg", "-v", "error", "-nostdin", "-i", path, "-map", "0:v:0", "-f", "rawvideo",
                          "-pix_fmt", "gray", "-fps_mode", "passthrough", "-"], stdout=subprocess.PIPE)
    size = w * h
    while True:
        b = p.stdout.read(size)
        if len(b) < size:
            break
        yield b
    p.wait()


def psnr(a, b):
    se = sum((x - y) ** 2 for x, y in zip(a, b))
    return 100.0 if se == 0 else 10 * math.log10(255 * 255 / (se / len(a)))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("object", help="the uploaded .tar.bin, or a package.tar")
    ap.add_argument("--key", help="the private key (PEM) for a .tar.bin")
    ap.add_argument("--keep", help="unpack here and keep it (default: a temporary folder)")
    ap.add_argument("--min-psnr", type=float, default=40.0)
    a = ap.parse_args()

    work = a.keep or tempfile.mkdtemp(prefix="babble-verify-")
    os.makedirs(work, exist_ok=True)
    try:
        tar = a.object
        if a.object.endswith(".bin"):
            if not a.key:
                sys.exit("a .tar.bin needs --key")
            print(f"decrypting {a.object}")
            tar = os.path.join(work, "package.tar")
            decrypt(a.object, a.key, tar)
            check(True, "decrypted with the private key")
        print(f"unpacking {tar}")
        with tarfile.open(tar) as t:
            names = t.getnames()
            check(all("/" not in n and n not in ("", ".", "..") for n in names), "flat tar, no paths")
            t.extractall(work, filter="data")
        m = json.load(open(os.path.join(work, "session.json")))
        pkg = m.get("package", {})
        print(f"session {m.get('id')}: format {m.get('format')}, routine {m.get('routine')}, "
              f"status {m.get('status')}, stop_reason {m.get('stop_reason')!r}")
        check(m.get("status") == "complete", "session complete")
        check(m.get("consent") is not None, "consent recorded")
        if m.get("format", 1) >= 2:
            check(isinstance(m.get("device"), dict) and "model" in m["device"], "device recorded")

        print("files")
        listed = {f["name"]: f for f in pkg.get("files", [])}
        check(set(listed) | {"session.json"} == set(names), "tar holds exactly the listed files")
        for name, f in sorted(listed.items()):
            p = os.path.join(work, name)
            check(os.path.getsize(p) == f["bytes"] and sha256(p) == f["sha256"], f"{name}: size and sha256")

        started, ended = m.get("started_ns") or 0, m.get("ended_ns") or 0
        for name, s in pkg.get("sources", {}).items():
            print(f"source {name}")
            rec = m["sources"][name]
            w, h = rec["width"], rec["height"]
            stamps = open(os.path.join(work, s["stamps"]), "rb").read()
            n_st = len(stamps) // 24
            check(len(stamps) % 24 == 0, f"{s['stamps']}: whole 24-byte records ({n_st})")
            caps = [struct.unpack_from("<Q", stamps, 24 * i)[0] for i in range(n_st)]
            check(all(b > a for a, b in zip(caps, caps[1:])), "capture times increase")
            rng = subprocess.run(["ffprobe", "-v", "error", "-select_streams", "v:0", "-show_entries",
                                  "stream=color_range", "-of", "default=nw=1:nk=1", os.path.join(work, s["file"])],
                                 capture_output=True, text=True).stdout.strip()
            check(rng == "pc", f"{s['file']}: tagged full range ({rng})")
            stills = {}
            every = s["still_every"]
            for i, fr in enumerate(frames(os.path.join(work, s["stills"]), w, h)):
                stills[i * every] = fr
            check(len(stills) == s["still_count"], f"{s['stills']}: {len(stills)} stills")
            n, worst, total = 0, 1000.0, 0.0
            for fr in frames(os.path.join(work, s["file"]), w, h):
                if n in stills:
                    v = psnr(fr, stills[n])
                    worst = min(worst, v)
                    total += v
                n += 1
            check(n == n_st == s["frames"], f"{s['file']}: {n} frames = {n_st} stamps")
            if stills:
                check(worst >= a.min_psnr, f"stills vs H.265: min {worst:.2f}, mean {total / len(stills):.2f} dB")

        print("labels")
        labels = [json.loads(l) for l in open(os.path.join(work, "labels.jsonl"))] \
            if "labels.jsonl" in listed else []
        check(len(labels) > 0, f"{len(labels)} labels")
        if labels and started and ended:
            check(all(started <= l["start_ns"] <= l["end_ns"] <= ended for l in labels), "labels inside the session")
        if m.get("format", 1) >= 2 and labels:
            check(all("step" in l and "phase" in l for l in labels), "every label has step and phase")

        if m.get("routine_sha256"):
            print("routine")
            check(sha256(os.path.join(work, "routine.json")) == m["routine_sha256"], "routine.json matches its hash")
        elif m.get("format", 1) >= 2:
            check(False, "format 2 session without routine.json")

        if "valve_eye" in m:
            print("valve eye samples")
            v = m["valve_eye"]
            data = open(os.path.join(work, v["file"]), "rb").read() if v["file"] in listed else b""
            check(len(data) % 88 == 0 and len(data) // 88 == v["records"], f"{len(data) // 88} whole records")
            check(v["records"] > 0, "Valve eye samples recorded")
            if data:
                first = struct.unpack_from("<Q", data, 0)[0]
                last = struct.unpack_from("<Q", data, len(data) - 88)[0]
                check(started <= first and (not ended or last <= ended), "samples inside the session")
                span = (last - first) / 1e9
                dur = (ended - started) / 1e9 if ended else 0
                check(dur == 0 or span > 0.9 * dur, f"samples span {span:.1f} of {dur:.1f} s")
                print(f"        about {len(data) // 88 / max(span, 1e-9):.0f} samples/s")
    finally:
        if not a.keep:
            shutil.rmtree(work, ignore_errors=True)

    print("PASS" if not failures else f"FAIL: {len(failures)} check(s)")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
