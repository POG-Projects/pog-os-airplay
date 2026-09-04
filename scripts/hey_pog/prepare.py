"""Build a synthetic-only French wake-word corpus outside the source tree.

Uses Piper's fr_FR-mls-medium (125 speakers, model card: CC-BY-4.0 data).
Speaker IDs, not augmented clips, define the train/validation/test split.
Never opens a microphone or connects to the satellite.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path

import numpy as np
import onnxruntime as ort
from piper import PiperConfig, PiperVoice, SynthesisConfig
from pymicro_features import MicroFrontend
from scipy import signal
from scipy.io import wavfile

POSITIVES = ["Hey Pog !", "Hé Pog !", "Hey, Pog.", "Hé, Pog."]
NEGATIVES = [
    "Hey Bob", "Hé Paul", "Hey Pop", "Hé Pôle", "Hey Poke", "Hey Dog",
    "Hey Bog", "Hé Pierre", "Hey Jarvis", "Hé Google", "Hey Pod", "Hey Park",
    "Pog", "Hé", "Hey", "Pog Pog", "Époque", "Hip hop", "Et Paul", "Un pog",
    "À cette époque", "Le programme", "C'est pas grave", "Il est parti",
    "Allume la lumière", "Éteins le salon", "Quel temps fait-il demain ?",
    "Quelle heure est-il ?", "Monte le volume", "Baisse la musique",
    "Ouvre les volets", "Ferme la porte", "La télévision est allumée",
    "Bonjour tout le monde", "Merci beaucoup", "Bonne nuit", "À tout à l'heure",
    "Je vais préparer le repas", "Est-ce que tu peux venir ici ?",
    "Il y a du pain sur la table", "Demain nous allons nous promener",
    "Je ne sais pas encore", "C'est vraiment une bonne idée",
    "Un deux trois quatre cinq six sept huit neuf dix", "Le chat dort sur le canapé",
    "Mets de la musique dans le bureau", "Ce soir nous regardons un film",
    "N'oublie pas les clés", "Le téléphone sonne", "Nous sommes à la maison",
    "Je te rappelle plus tard", "Le soleil se couche", "Comment ça va ?",
    "Pourquoi est-ce que ça ne fonctionne pas ?", "La fenêtre est ouverte",
    "Cette lampe est très jolie", "Je travaille dans le bureau", "J'ai un peu froid",
    "Le chauffage est réglé", "L'ordinateur a terminé", "Pardon, je n'ai pas entendu",
    "Une pomme et une poire", "On peut partir maintenant", "Ne touche pas à ça",
]


def features(pcm):
    frontend = MicroFrontend()
    raw = np.asarray(pcm, np.int16).tobytes()
    output, offset = [], 0
    while offset + 320 <= len(raw):
        part = frontend.process_samples(raw[offset:offset + 320])
        if not part.samples_read:
            break
        offset += part.samples_read * 2
        if part.features:
            output.append(part.features)
    return np.asarray(output, np.float32)


def augment(audio, rng, positive=False, length=200):
    # Rate, gain, delayed reflections and colored noise vary independently.
    speed = rng.uniform(.88, 1.12)
    audio = signal.resample(audio, max(1, int(len(audio) / speed))).astype(np.float32)
    if len(audio) > 28000:
        audio = signal.resample(audio, 28000).astype(np.float32)
    if rng.random() < .7:
        delay = int(rng.uniform(.012, .1) * 16000)
        audio[delay:] += rng.uniform(.05, .35) * audio[:-delay]
    audio *= rng.uniform(.08, .9)
    noise = rng.normal(0, rng.uniform(.0002, .012), len(audio))
    if rng.random() < .5:
        noise = signal.lfilter([.2], [1, -.8], noise)
    audio = np.clip(audio + noise, -1, 1)
    window = 32320  # 2.02 s -> exactly 200 frontend frames.
    front = int(rng.uniform(.2, 1.5) * 16000)
    tail = int(rng.uniform(.1, 1.2) * 16000)
    padded = np.pad(audio, (front, tail))
    if len(padded) < window:
        padded = np.pad(padded, (0, window - len(padded)))
    if positive:
        low = max(0, front + len(audio) + 1600 - window)
        high = min(front, len(padded) - window)
        start = low if high <= low else int(rng.integers(low, high + 1))
    else:
        start = int(rng.integers(0, len(padded) - window + 1))
    # Keep the frontend state established before the selected window. The
    # micro-speech frontend contains noise reduction, PCAN gain control and a
    # rolling analysis window; restarting it at the crop boundary produces
    # features unlike the continuously running frontend on the satellite.
    all_features = features(padded * 32767)
    feature_start = start // 160  # one feature every 10 ms at 16 kHz
    f = all_features[feature_start:feature_start + length]
    assert f.shape == (length, 40), f.shape
    return f


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--work", type=Path, required=True)
    parser.add_argument("--seed", type=int, default=29142026)
    args = parser.parse_args()
    work = args.work.resolve()
    work.mkdir(parents=True, exist_ok=True)
    voices = work / "voices"
    model = voices / "fr_FR-mls-medium.onnx"
    cfg = PiperConfig.from_dict(json.loads(model.with_suffix(".onnx.json").read_text()))
    options = ort.SessionOptions()
    options.intra_op_num_threads = 8
    options.inter_op_num_threads = 1
    voice = PiperVoice(ort.InferenceSession(str(model), sess_options=options,
                                           providers=["CPUExecutionProvider"]), cfg)
    rng = np.random.default_rng(args.seed)
    speakers = rng.permutation(cfg.num_speakers)
    groups = {"training": speakers[:100], "validation": speakers[100:112],
              "testing": speakers[112:]}
    manifest = {"seed": args.seed, "phrase": "Hey Pog", "window_step_ms": 10,
                "spectrogram_length": 200, "source": "rhasspy/piper-voices",
                "model_sha256": hashlib.sha256(model.read_bytes()).hexdigest(),
                "model_card": (voices / "MODEL_CARD").read_text(),
                "positive_texts": POSITIVES, "negative_texts": NEGATIVES,
                "speakers": {k: list(map(int, v)) for k, v in groups.items()}}
    (work / "corpus.json").write_text(json.dumps(manifest, indent=2, ensure_ascii=False))
    for split, ids in groups.items():
        x, y = [], []
        clips = work / "samples-v2" / split
        clips.mkdir(parents=True, exist_ok=True)
        for sid in ids:
            examples = [(p, 1) for p in POSITIVES] * 2
            examples += [(NEGATIVES[i], 0) for i in rng.choice(12, 4, replace=False)]
            examples += [(NEGATIVES[i], 0) for i in rng.choice(np.arange(24, len(NEGATIVES)), 12, replace=False)]
            for index, (text, label) in enumerate(examples):
                path = clips / f"{sid:03d}_{index:02d}_{label}.wav"
                if path.exists():
                    rate, pcm = wavfile.read(path)
                    assert rate == 16000
                    audio = pcm.astype(np.float32) / 32768
                else:
                    chunks = list(voice.synthesize(text, SynthesisConfig(
                        speaker_id=int(sid), length_scale=float(rng.uniform(.85, 1.1)),
                        noise_scale=float(rng.uniform(.55, .85)), noise_w_scale=.8)))
                    audio = np.concatenate([c.audio_float_array for c in chunks])
                    g = math.gcd(cfg.sample_rate, 16000)
                    audio = signal.resample_poly(audio, 16000 // g, cfg.sample_rate // g)
                    wavfile.write(path, 16000, np.clip(audio * 32767, -32768, 32767).astype(np.int16))
                for _ in range(10 if label else 2):
                    x.append(augment(audio, rng, bool(label)))
                    y.append(label)
            # Synthetic quiet room / hum / static negatives, without speech.
            for _ in range(4):
                t = np.arange(20000) / 16000
                noise = rng.normal(0, .015, len(t))
                noise += .01 * np.sin(2 * np.pi * rng.choice([50, 100, 440, 1000]) * t)
                x.append(augment(noise, rng, False)); y.append(0)
            print(f"{split}: speaker={sid} examples={len(y)}", flush=True)
        np.savez(work / f"{split}.npz", x=np.asarray(x, np.float32), y=np.asarray(y, np.float32))
        print(f"Saved {split}: {len(y)} examples, positive={sum(y)}", flush=True)


if __name__ == "__main__":
    main()
