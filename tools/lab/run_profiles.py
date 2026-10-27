#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Reed lab capture runner — WIP.

Loops candidate voice/quality profiles on an FM-1 running the Reed LAB image:
sets the profile over USB-MIDI CCs, plays the same MIDI file through the same
MIDI port, records the FM-1's P2/analog output (or USB audio) with ffmpeg, and
writes WAV/MP3 plus a CSV/JSON result set.

CC map (LAB firmware):
  20 voices 1..32        21 modes 1..7       22 pickup 0/127
  23 noise 0/127         24 drag 0/127        25 chain 0/127
  26 master 0..30 (-24..+6 dB)               27 commit profile (>=64)
  120 all sound off      123 all notes off
  CC1 tremolo, CC7 volume, CC11 drive, CC64 sustain already exist.

The runner never writes flash or changes clock/rails. It is a skeleton until the
hardware, the song and the audio input are chosen.

Requirements:
  pip install pyserial mido python-rtmidi
  ffmpeg on PATH
  an audio interface (or line-in) connected to the FM-1 P2 output.

Example (MIDI CC control + analog P2 capture):
  python3 tools/lab/run_profiles.py \
    --midi-out "FM-1 Reed LAB" \
    --audio-input "-f avfoundation -i :2" \
    --song assets/lab/song.mid \
    --profiles tools/lab/profiles.json \
    --out captures/2026-10-27 --repeats 3

Optional CDC telemetry (same USB device, second port):
  --serial /dev/cu.usbmodemXXXX
"""
import argparse
import csv
import json
import pathlib
import shlex
import struct
import subprocess
import sys
import time
import wave

ROOT = pathlib.Path(__file__).resolve().parents[2]
DEFAULT_PROFILES = ROOT / "tools" / "lab" / "profiles.json"

CC_VOICES = 20
CC_MODES = 21
CC_PICKUP = 22
CC_NOISE = 23
CC_DRAG = 24
CC_CHAIN = 25
CC_MASTER = 26
CC_COMMIT = 27
CC_ALL_SOUND_OFF = 120
CC_ALL_NOTES_OFF = 123


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
        if not (-24 <= p["master_db"] <= 6):
            raise SystemExit(f"invalid master_db in {p['name']}")
    return profiles


def profile_line(p):
    return "P %d %d %d %d %d %d %d" % (
        p["voices"], p["modes"], p["pickup"], p["noise"], p["drag"],
        p["chain"], p["master_db"],
    )


def cc_sequence(p):
    """(controller, value) pairs; profile is staged then committed."""
    return [
        (CC_VOICES, int(p["voices"])),
        (CC_MODES, int(p["modes"])),
        (CC_PICKUP, 127 if p["pickup"] else 0),
        (CC_NOISE, 127 if p["noise"] else 0),
        (CC_DRAG, 127 if p["drag"] else 0),
        (CC_CHAIN, 127 if p["chain"] else 0),
        (CC_MASTER, max(0, min(30, int(p["master_db"]) + 24))),
        (CC_COMMIT, 127),
    ]


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


def open_midi(port):
    try:
        import mido
    except ImportError:
        raise SystemExit("mido is required: pip install mido python-rtmidi")
    return mido.open_output(port)


def send_cc(out, control, value, channel):
    import mido
    out.send(mido.Message("control_change", channel=channel, control=control, value=value))


def apply_cc_profile(out, p, channel, settle):
    """All notes off, stage every field, commit, let the engine settle."""
    send_cc(out, CC_ALL_SOUND_OFF, 0, channel)
    send_cc(out, CC_ALL_NOTES_OFF, 0, channel)
    for control, value in cc_sequence(p):
        send_cc(out, control, value, channel)
    time.sleep(settle)


def apply_cdc_profile(ser, p):
    ser.write((profile_line(p) + "\n").encode("ascii"))
    return wait_line(ser, "OK")


def clear_stats(ser):
    if not ser:
        return
    ser.write(b"C\n")
    try:
        wait_line(ser, "OK", 1.0)
    except TimeoutError:
        pass


def stats(ser):
    if not ser:
        return {}
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


def midi_duration(path):
    try:
        import mido
    except ImportError:
        raise SystemExit("mido is required: pip install mido python-rtmidi")
    return sum(msg.time for msg in mido.MidiFile(path))


def play_midi(path, out):
    import mido
    mf = mido.MidiFile(path)
    for msg in mf:
        if msg.time:
            time.sleep(msg.time)
        if not msg.is_meta:
            out.send(msg)


def wav_level(path):
    with wave.open(str(path)) as w:
        n = w.getnframes()
        data = w.readframes(n)
    samples = struct.unpack("<%dh" % (len(data) // 2), data)
    peak = max((abs(v) for v in samples), default=0) / 32768.0
    clipped = sum(1 for v in samples if abs(v) >= 32767)
    return peak, clipped


def record_and_render(args, song_seconds, midi_out, wav_path, mp3_path):
    input_args = shlex.split(args.audio_input)
    total = song_seconds + args.preroll + args.tail + 0.5
    ff = [
        "ffmpeg", "-y", "-hide_banner", "-loglevel", "error",
        *input_args, "-t", f"{total:.2f}", "-ac", "2", "-ar", "44100",
        str(wav_path),
    ]
    proc = subprocess.Popen(ff)
    time.sleep(args.preroll)
    play_midi(args.song, midi_out)
    proc.wait(timeout=total + 15)
    if proc.returncode != 0:
        raise RuntimeError("ffmpeg recording failed")
    peak, clipped = wav_level(wav_path)
    if peak > 0.99:
        print(f"  WARNING: input clips ({clipped} samples at full scale)", file=sys.stderr)
    if peak < 0.001:
        print("  WARNING: recording is silent; check the P2 input", file=sys.stderr)
    # Loudness-match the MP3 (the raw WAV is kept) so the blind test judges
    # timbre, not which profile happened to be hotter.
    subprocess.run(
        ["ffmpeg", "-y", "-hide_banner", "-loglevel", "error", "-i", str(wav_path),
         "-af", "loudnorm=I=-18:TP=-1.5:LRA=11",
         "-codec:a", "libmp3lame", "-b:a", args.mp3_bitrate, str(mp3_path)],
        check=True,
    )
    return peak, clipped


def main():
    ap = argparse.ArgumentParser(description="Reed LAB profile capture runner (WIP)")
    ap.add_argument("--control", choices=("midi", "cdc"), default="midi",
                    help="profile control transport (default: MIDI CC)")
    ap.add_argument("--channel", type=int, default=1, help="MIDI channel 1..16")
    ap.add_argument("--midi-out", help="MIDI output port name (profile CCs + song)")
    ap.add_argument("--serial", help="optional FM-1 USB-CDC port for telemetry")
    ap.add_argument("--audio-input", help='ffmpeg input args, e.g. "-f avfoundation -i :2"')
    ap.add_argument("--song", help="MIDI file to play (not chosen yet)")
    ap.add_argument("--profiles", default=str(DEFAULT_PROFILES))
    ap.add_argument("--only", nargs="*", help="profile names to run")
    ap.add_argument("--out", default="captures/lab")
    ap.add_argument("--repeats", type=int, default=1)
    ap.add_argument("--preroll", type=float, default=0.8)
    ap.add_argument("--tail", type=float, default=2.0)
    ap.add_argument("--settle", type=float, default=0.8)
    ap.add_argument("--mp3-bitrate", default="320k")
    ap.add_argument("--blind", action="store_true",
                    help="name files takeNNN and keep the profile key in results only")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--force", action="store_true")
    args = ap.parse_args()

    if not (1 <= args.channel <= 16):
        raise SystemExit("--channel must be 1..16")
    channel = args.channel - 1
    profiles = load_profiles(args.profiles, args.only)
    out = pathlib.Path(args.out)
    if out.exists() and any(out.iterdir()) and not args.force:
        raise SystemExit(f"{out} is not empty; use --force")
    out.mkdir(parents=True, exist_ok=True)

    if not args.dry_run:
        required = ["midi_out", "audio_input", "song"]
        if args.control == "cdc":
            required.append("serial")
        missing = [n for n in required if not getattr(args, n)]
        if missing:
            raise SystemExit("missing: " + ", ".join("--" + m.replace("_", "-") for m in missing))
        if not pathlib.Path(args.song).is_file():
            raise SystemExit(f"song not found: {args.song}")
        midi_out = open_midi(args.midi_out)
        ser = open_serial(args.serial) if args.serial else None
    else:
        midi_out = None
        ser = None

    if args.song:
        song_seconds = midi_duration(args.song)
    else:
        song_seconds = 0.0
        print("WIP: no song chosen; duration is a placeholder", file=sys.stderr)

    rows = []
    for repeat in range(1, args.repeats + 1):
        for p in profiles:
            if args.blind:
                stem = f"take{repeat:02d}_{len(rows) + 1:03d}"
            else:
                stem = f"{p['name']}_r{repeat}"
            wav = out / (stem + ".wav")
            mp3 = out / (stem + ".mp3")
            print(f"[{stem}] {profile_line(p)}")
            if args.dry_run:
                print("  CC", cc_sequence(p))
                print("  ffmpeg", args.audio_input, "->", wav)
                print("  play", args.song, "on", args.midi_out)
                rows.append({**p, "repeat": repeat, "file": str(mp3),
                             "blind_code": stem if args.blind else "",
                             "control": args.control, "dry_run": 1})
                continue
            if args.control == "midi":
                apply_cc_profile(midi_out, p, channel, args.settle)
                device_ok = "CC"
            else:
                device_ok = apply_cdc_profile(ser, p)
                time.sleep(args.settle)
            clear_stats(ser)
            before = stats(ser)
            peak, clipped = record_and_render(args, song_seconds, midi_out, wav, mp3)
            after = stats(ser)
            # all notes off between runs
            if args.control == "midi":
                send_cc(midi_out, CC_ALL_SOUND_OFF, 0, channel)
                send_cc(midi_out, CC_ALL_NOTES_OFF, 0, channel)
            elif ser:
                ser.write(b"X\n")
                try:
                    wait_line(ser, "OK", 1.0)
                except TimeoutError:
                    pass
            rows.append({
                **p, "repeat": repeat, "file": str(mp3), "control": args.control,
                "blind_code": stem if args.blind else "",
                "peak": round(peak, 4), "clipped_samples": clipped,
                "cpu_before": before.get("cpu"), "cpu_after": after.get("cpu"),
                "max_us": max(before.get("max_us", 0), after.get("max_us", 0)),
                "late": max(before.get("late", 0), after.get("late", 0)),
                "active_after": after.get("active"), "device_ok": device_ok,
            })

    (out / "results.json").write_text(json.dumps(rows, indent=2) + "\n")
    with (out / "results.csv").open("w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=sorted({k for r in rows for k in r}))
        w.writeheader()
        w.writerows(rows)
    print(f"wrote {out}/results.json and results.csv")
    if not args.dry_run:
        if ser:
            ser.close()
        if midi_out:
            midi_out.close()


if __name__ == "__main__":
    main()
