# Reed lab tools (WIP)

`run_profiles.py` sets candidate profiles over USB-MIDI CCs, plays one MIDI song,
records the FM-1's P2 analog output (or optional USB audio) with ffmpeg, and
writes WAV + 320 kbps MP3 + CSV/JSON results.

Status: **skeleton**. The hardware, the song and the capture input are not chosen
yet; no result is published.

```sh
pip install pyserial mido python-rtmidi
# ffmpeg on PATH

python3 tools/lab/run_profiles.py \
  --midi-out "FM-1 Reed LAB" \
  --audio-input "-f avfoundation -i :2" \
  --song assets/lab/song.mid \
  --profiles tools/lab/profiles.json \
  --out captures/lab --repeats 3 --blind
```

Dry run (no hardware/song needed):

```sh
python3 tools/lab/run_profiles.py --dry-run \
  --profiles tools/lab/profiles.json --only HQ_8 --out /tmp/reed-lab-dry
```

The profile format is one JSON object per candidate:

```json
{"name":"HQ_8","voices":8,"modes":7,"pickup":0,"noise":1,"drag":1,"chain":0,"master_db":0}
```

`pickup`/`chain`: 0 full, 1 lite. The runner sends CC20..26 then CC27 to commit;
`--control cdc` uses the serial console instead. It never writes flash or touches
clock/rails. Add `--serial /dev/cu.usbmodemXXXX` for CPU/underrun telemetry.

See [../../docs/LAB.md](../../docs/LAB.md) and
[../../web/profiles.html](../../web/profiles.html).
