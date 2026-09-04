"""Collect explicit two-second samples from an authenticated POG AirPlay.

Nothing starts until the operator presses Enter for each clip. The password and
session token remain in memory and are never written to the manifest.
"""
import argparse
import getpass
import json
import sys
import urllib.error
import urllib.request
import wave
from datetime import datetime, timezone
from pathlib import Path

NEAR_MISSES = [
    "Hey Bob", "Hé Paul", "Hey Pop", "Hey Dog", "Hey Pod", "Hey Poke",
    "Hey Jarvis", "Hé Google", "Pog", "Hé", "Hey", "À cette époque",
    "Allume la lumière", "Monte le volume", "Quel temps fait-il ?",
]


def request(url, body, headers=None):
    data = json.dumps(body).encode() if body is not None else b""
    req = urllib.request.Request(url, data=data, method="POST", headers={
        "Content-Type": "application/json", **(headers or {})})
    with urllib.request.urlopen(req, timeout=12) as response:
        return response.headers, response.read()


def validate_wav(data):
    import io
    with wave.open(io.BytesIO(data), "rb") as wav:
        if wav.getnchannels() != 1 or wav.getsampwidth() != 2:
            raise ValueError("expected mono 16-bit PCM")
        duration = wav.getnframes() / wav.getframerate()
        if not 1.8 <= duration <= 2.1:
            raise ValueError(f"unexpected duration: {duration:.3f}s")
        return {"rate": wav.getframerate(), "frames": wav.getnframes(),
                "duration_seconds": duration}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--url", default="http://bureau.local")
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--kind", choices=("positive", "near-miss"), required=True)
    parser.add_argument("--count", type=int, default=30)
    args = parser.parse_args()
    if not 1 <= args.count <= 100:
        parser.error("--count must be between 1 and 100")
    password = getpass.getpass("Mot de passe administrateur de l’enceinte : ")
    try:
        _, login = request(args.url.rstrip("/") + "/api/auth/login",
                           {"password": password})
        token = json.loads(login)["token"]
    except (urllib.error.URLError, KeyError, json.JSONDecodeError) as exc:
        raise SystemExit(f"Connexion impossible : {exc}") from exc
    finally:
        password = ""

    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    manifest = {"created_at": datetime.now(timezone.utc).isoformat(),
                "device_url": args.url, "kind": args.kind, "clips": []}
    print("Chaque appui sur Entrée enregistre exactement 2 secondes sur la carte.")
    print("Le WAV est téléchargé sur ce Mac ; aucun extrait n’est envoyé à POG AI.")
    for index in range(args.count):
        phrase = "Hey Pog" if args.kind == "positive" else NEAR_MISSES[index % len(NEAR_MISSES)]
        input(f"[{index + 1}/{args.count}] Entrée, puis dites « {phrase} »… ")
        try:
            headers, audio = request(
                args.url.rstrip("/") + "/api/microphone/sample", None,
                {"X-Auth-Token": token})
            if "audio/wav" not in headers.get_content_type():
                raise ValueError("the device did not return WAV audio")
            meta = validate_wav(audio)
        except (urllib.error.URLError, ValueError, wave.Error) as exc:
            print(f"Échantillon ignoré : {exc}", file=sys.stderr)
            continue
        name = f"{args.kind}-{index + 1:03d}.wav"
        (out / name).write_bytes(audio)
        manifest["clips"].append({"file": name, "label": int(args.kind == "positive"),
                                  "prompt": phrase, **meta})
        print(f"  enregistré : {name}")
    (out / "manifest.json").write_text(
        json.dumps(manifest, ensure_ascii=False, indent=2) + "\n")
    print(f"{len(manifest['clips'])} échantillons dans {out}")


if __name__ == "__main__":
    main()
