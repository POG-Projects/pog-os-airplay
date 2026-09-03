"""Add synthetic macOS French voices; does not use a microphone."""
import argparse
import json
import subprocess
from pathlib import Path

import numpy as np
from scipy.io import wavfile

from prepare import POSITIVES, augment

VOICES = {
    "training": ["Eddy (Français (France))", "Flo (Français (France))",
                 "Grandma (Français (France))", "Grandpa (Français (France))",
                 "Jacques", "Rocko (Français (France))"],
    "validation": ["Sandy (Français (France))"],
    "testing": ["Shelley (Français (France))", "Thomas"],
}


def main():
    p = argparse.ArgumentParser(); p.add_argument("--work", type=Path, required=True)
    args = p.parse_args(); rng = np.random.default_rng(29142027)
    root = args.work / "mac-voice-samples"; root.mkdir(exist_ok=True)
    for split, voices in VOICES.items():
        z = np.load(args.work / f"{split}.npz"); xs, ys = [z["x"]], [z["y"]]
        added = []
        for voice in voices:
            safe = "".join(c if c.isalnum() else "_" for c in voice)
            for phrase_index, phrase in enumerate(POSITIVES):
                for rate in (135, 175, 215):
                    wav = root / f"{safe}_{phrase_index}_{rate}.wav"
                    if not wav.exists():
                        aiff = wav.with_suffix(".aiff")
                        subprocess.run(["/usr/bin/say", "-v", voice, "-r", str(rate),
                                        "-o", str(aiff), phrase], check=True)
                        subprocess.run(["/opt/homebrew/bin/ffmpeg", "-v", "error", "-y",
                                        "-i", str(aiff), "-ar", "16000", "-ac", "1",
                                        "-c:a", "pcm_s16le", str(wav)], check=True)
                        aiff.unlink()
                    sample_rate, pcm = wavfile.read(wav); assert sample_rate == 16000
                    audio = pcm.astype(np.float32) / 32768
                    added.extend(augment(audio, rng, True) for _ in range(8))
            print(split, voice, len(added), flush=True)
        xs.append(np.asarray(added, np.float32)); ys.append(np.ones(len(added), np.float32))
        np.savez(args.work / f"{split}.npz", x=np.concatenate(xs), y=np.concatenate(ys))
    manifest = json.loads((args.work / "corpus.json").read_text())
    manifest["mac_fr_FR_voices"] = VOICES
    (args.work / "corpus.json").write_text(json.dumps(manifest, indent=2, ensure_ascii=False))


if __name__ == "__main__": main()
