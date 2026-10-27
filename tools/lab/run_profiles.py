#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Reed lab capture runner — WIP.

Loops candidate voice/quality profiles on an FM-1 running the Reed LAB image:
sets the profile over the USB-CDC console, plays the same MIDI file through
USB-MIDI, records the FM-1's USB audio (UAC) with ffmpeg, and writes WAV/MP3
plus a CSV/JSON result set.

This script is intentionally a skeleton until the hardware, the song and the
audio input are chosen. It never writes flash or changes clock/rails: it only
sends the profile/stats/panic commands implemented by the LAB firmware.

Requirements:
  pip install pyserial mido python-rtmidi
  ffmpeg on PATH

Example:
  python3 tools/lab/run_profiles.py \
    --serial /dev/cu.usbmodemXXXX \
    --midi-out "FM-1 Reed LAB" \
    --audio-input "-f avfoundation -i :2" \
    --song assets/lab/song.mid \
    --profiles tools/lab/profiles.json \
    --out captures/2026-10-27 --repeats 3
"""
import argparse
import csv
import json
import pathlib
import shlex
import subprocess
import sys
import time

ROOT = pathlib.Path(__file__).resolve().parents[2]
DEFAULT_PROFILES = ROOT / "tools" / "lab" / "profiles.json"


def load_profiles(path, only=None):
    data = json.loads(pathlib.Path(path).read_text())
    profiles = data["profiles"] if isinstance(data, dict) else data
    if only:
        wanted = set(only)
        profiles = [p for p in profiles if p["name"] in wanted]
    if not profiles:
        raise SystemExit("no profiles selected")
    for p in profiles:
        if not (1 <= p["voices"] <= 32 and 1 <= p["modes"] <= 7):
            raise SystemExit(f"invalid profile {p}")
        for key in ("pickup", "noise", "drag", "chain"):
            if p[key] not in (0, 1):
                raise SystemExit(f"invalid {key} in {p['name']}")
        p.setdefault("master_db", 0)
    return profiles


def profile_line(p):
    return "P %d %d %d %d %d %d %d\n" % (
        p["voices"], p["modes"], p["pickup"], p["noise"], p["drag"],
        p["chain"], p["master_db"],
    )


def wait_line(ser, prefix, timeout=3.0):
    deadline = time.monotonic() + timeout
    buf = b""
    while time.monotonic() < deadline:
        chunk = ser.read(64)
        if chunk:
            buf += chunk
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                text = line.decode("ascii", "replace").strip()
                if text.startswith(prefix):
                    return text
        else:
            time.sleep(0.02)
    raise TimeoutError(f"no {prefix!r} from device")


def open_serial(port):
    try:
        import serial  # pyserial
    except ImportError:
        raise SystemExit("pyserial is required: pip install pyserial")
    ser = serial.Serial(port, 115200, timeout=0.05)
    ser.reset_input_buffer()
    try:
        wait_line(ser, "REED-LAB", 3.0)
    except TimeoutError:
        pass
    return ser


def set_profile(ser, p):
    ser.write(profile_line(p).encode("ascii"))
    line = wait_line(ser, "OK")
    return line


def stats(ser):
    ser.write(b"S\n")
    line = wait_line(ser, "STAT")
    out = {}
    for token in line.split()[1:]:
        if "=" in token:
            k, v = token.split("=", 1)
            try:
                out[k] = int(v)
            except ValueError:
                out[k] = v
    return out


def midi_duration_and_play(path, out_port, dry_run=False):
    try:
        import mido
    except ImportError:
        raise SystemExit("mido is required: pip install mido python-rtmidi")
    mf = mido.MidiFile(path)
    # MidiFile iteration gives delta times in seconds (tempo handled by mido).
    duration = 0.0
    if dry_run:
        for msg in mf:
            duration += msg.time
        return duration
    out = mido.open_output(out_port)
    try:
        for msg in mf:
            if msg.time:
                time.sleep(msg.time)
            if not msg.is_meta:
                out.send(msg)
    finally:
        out.close()
    return duration


def record_and_render(profile, args, song_seconds, wav_path, mp3_path):
    input_args = shlex.split(args.audio_input)
    total = song_seconds + args.preroll + args.tail + 0.5
    ff = [
        "ffmpeg", "-y", "-hide_banner", "-loglevel", "error",
        *input_args, "-t", f"{total:.2f}", "-ac", "2", "-ar", "44100",
        str(wav_path),
    ]
    proc = subprocess.Popen(ff)
    time.sleep(args.preroll)
    midi_duration_and_play(args.song, args.midi_out, args.dry_run)
    proc.wait(timeout=total + 15)
    if proc.returncode != 0:
        raise RuntimeError("ffmpeg recording failed")
    # Loudness-match the MP3 (the raw WAV is kept) so the blind test judges
    # timbre, not which profile happened to be hotter.
    subprocess.run(
        ["ffmpeg", "-y", "-hide_banner", "-loglevel", "error", "-i", str(wav_path),
         "-af", "loudnorm=I=-18:TP=-1.5:LRA=11",
         "-codec:a", "libmp3lame", "-b:a", args.mp3_bitrate, str(mp3_path)],
        check=True,
    )


def main():
    ap = argparse.ArgumentParser(description="Reed LAB profile capture runner (WIP)")
    ap.add_argument("--serial", help="FM-1 USB-CDC port (LAB image)")
    ap.add_argument("--midi-out", help="MIDI output port name for the song")
    ap.add_argument("--audio-input", help='ffmpeg input arguments, e.g. "-f avfoundation -i :2"')
    ap.add_argument("--song", help="MIDI file to play (not chosen yet)")
    ap.add_argument("--profiles", default=str(DEFAULT_PROFILES))
    ap.add_argument("--only", nargs="*", help="profile names to run")
    ap.add_argument("--out", default="captures/lab")
    ap.add_argument("--repeats", type=int, default=1)
    ap.add_argument("--preroll", type=float, default=0.5)
    ap.add_argument("--tail", type=float, default=2.0)
    ap.add_argument("--mp3-bitrate", default="320k")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--force", action="store_true")
    args = ap.parse_args()

    profiles = load_profiles(args.profiles, args.only)
    out = pathlib.Path(args.out)
    if out.exists() and any(out.iterdir()) and not args.force:
        raise SystemExit(f"{out} is not empty; use --force")
    out.mkdir(parents=True, exist_ok=True)

    if not args.dry_run:
        missing = [n for n in ("serial", "midi_out", "audio_input", "song") if not getattr(args, n)]
        if missing:
            raise SystemExit("missing: " + ", ".join("--" + m.replace("_", "-") for m in missing))
        if not pathlib.Path(args.song).is_file():
            raise SystemExit(f"song not found: {args.song}")
        ser = open_serial(args.serial)
    else:
        ser = None

    if args.song:
        song_seconds = midi_duration_and_play(args.song, args.midi_out, True)
    else:
        song_seconds = 0.0
        print("WIP: no song chosen; duration is a placeholder", file=sys.stderr)

    rows = []
    for repeat in range(1, args.repeats + 1):
        for p in profiles:
            stem = f"{p['name']}_r{repeat}"
            wav = out / (stem + ".wav")
            mp3 = out / (stem + ".mp3")
            print(f"[{stem}] " + profile_line(p).strip())
            if args.dry_run:
                print("  ffmpeg", args.audio_input, "->", wav)
                print("  play", args.song, "on", args.midi_out)
                rows.append({**p, "repeat": repeat, "file": str(mp3), "dry_run": 1})
                continue
            ok = set_profile(ser, p)
            ser.write(b"C\n")  # clear per-profile max/late counters
            try:
                wait_line(ser, "OK", 1.0)
            except TimeoutError:
                pass
            before = stats(ser)
            record_and_render(p, args, song_seconds, wav, mp3)
            after = stats(ser)
            # all notes off between runs
            ser.write(b"X\n")
            try:
                wait_line(ser, "OK", 1.0)
            except TimeoutError:
                pass
            rows.append({
                **p, "repeat": repeat, "file": str(mp3),
                "cpu_before": before.get("cpu"), "cpu_after": after.get("cpu"),
                "max_us": max(before.get("max_us", 0), after.get("max_us", 0)),
                "late": max(before.get("late", 0), after.get("late", 0)),
                "active_after": after.get("active"),
                "device_ok": ok,
            })

    (out / "results.json").write_text(json.dumps(rows, indent=2) + "\n")
    with (out / "results.csv").open("w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=sorted({k for r in rows for k in r}))
        w.writeheader()
        w.writerows(rows)
    print(f"wrote {out}/results.json and results.csv")
    if not args.dry_run:
        ser.close()


if __name__ == "__main__":
    main()
