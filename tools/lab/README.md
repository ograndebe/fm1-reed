# Reed lab tools (WIP)

`run_profiles.py` sets candidate profiles on a Reed LAB FM-1, plays one MIDI
song, records the FM-1's USB audio with ffmpeg, and writes WAV + 320 kbps MP3 +
CSV/JSON results.

Status: **skeleton**. The hardware, the song and the capture input are not chosen
yet; no result is published.

```sh
pip install pyserial mido python-rtmidi
# ffmpeg on PATH

python3 tools/lab/run_profiles.py \
  --serial /dev/cu.usbmodemXXXX \
  --midi-out "FM-1 Reed LAB" \
  --audio-input "-f avfoundation -i :2" \
  --song assets/lab/song.mid \
  --profiles tools/lab/profiles.json \
  --out captures/lab --repeats 3
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

`pickup`/`chain`: 0 full, 1 lite. The runner only uses the lab console's `P`,
`S`, `R` and `X` commands; it never writes flash or touches clock/rails.

See [../../docs/LAB.md](../../docs/LAB.md) and
[../../web/profiles.html](../../web/profiles.html).
