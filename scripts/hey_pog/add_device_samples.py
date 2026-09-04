"""Merge opt-in device WAV samples into the synthetic feature corpus."""
import argparse
import json
import math
from pathlib import Path

import numpy as np
from scipy import signal
from scipy.io import wavfile

from prepare import augment


def load_audio(path):
    rate, pcm = wavfile.read(path)
    if pcm.ndim != 1 or pcm.dtype != np.int16:
        raise ValueError(f"{path}: expected mono int16 PCM")
    audio = pcm.astype(np.float32) / 32768
    if rate != 16000:
        divisor = math.gcd(rate, 16000)
        audio = signal.resample_poly(audio, 16000 // divisor, rate // divisor)
    # Keep the complete bounded capture. `augment` fits it into the model
    # window and, crucially, runs the stateful frontend before selecting that
    # window. Energy-only trimming proved unsafe for a two-syllable phrase: a
    # loud consonant could become the anchor and remove either "Hey" or "Pog".
    return audio.astype(np.float32)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--work", type=Path, required=True)
    parser.add_argument("--samples", type=Path, action="append", required=True,
                        help="capture directory; may be supplied more than once")
    parser.add_argument("--seed", type=int, default=29142026)
    args = parser.parse_args()
    work = args.work.resolve()
    report_path = work / "device-samples.json"
    if report_path.exists():
        raise SystemExit(f"{report_path} already exists; rebuild the base corpus before merging again")
    rng = np.random.default_rng(args.seed)
    clips = {0: [], 1: []}
    for folder in args.samples:
        folder = folder.resolve()
        manifest = json.loads((folder / "manifest.json").read_text())
        for clip in manifest["clips"]:
            label = int(clip["label"])
            clips[label].append((folder / clip["file"], label))
    splits = {"training": [], "validation": [], "testing": []}
    for labeled in clips.values():
        rng.shuffle(labeled)
        for index, clip in enumerate(labeled):
            # Distinct utterances, not augmented copies, cross split boundaries.
            bucket = index % 5
            split = "validation" if bucket == 3 else "testing" if bucket == 4 else "training"
            splits[split].append(clip)

    report = {"seed": args.seed, "source": "explicit-device-capture",
              "samples": [str(p) for p in args.samples], "splits": {}}
    for split, selected in splits.items():
        archive = np.load(work / f"{split}.npz")
        x = [*archive["x"]]
        y = [*archive["y"]]
        added = {"positive": 0, "negative": 0}
        for path, label in selected:
            audio = load_audio(path)
            # A single real speaker must remain visible among thousands of
            # synthetic voices. Each repeat gets independent room/noise/rate
            # augmentation; distinct utterances still define every split.
            repeats = (64 if label else 48) if split == "training" else 12
            for _ in range(repeats):
                x.append(augment(audio, rng, bool(label)))
                y.append(label)
            added["positive" if label else "negative"] += repeats
        np.savez(work / f"{split}.npz", x=np.asarray(x, np.float32),
                 y=np.asarray(y, np.float32))
        report["splits"][split] = added
        print(f"{split}: added {added}")
    report_path.write_text(
        json.dumps(report, ensure_ascii=False, indent=2) + "\n")


if __name__ == "__main__":
    main()
