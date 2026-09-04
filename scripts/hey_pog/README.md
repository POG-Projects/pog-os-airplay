# Hey Pog training

`prepare.py` synthesizes French examples with Piper's 125-speaker
`fr_FR-mls-medium` voice and splits by speaker before augmentation. It creates
no recordings and writes all generated data under the supplied `--work` path.
`train.py` trains a small streaming mixed-convolution model, exports an integer
TFLite candidate compatible with the firmware engine, and writes the measured
acceptance result to `metrics.json`. A compatible model is not necessarily an
accepted model.

Run `prepare.py`, then `add_mac_samples.py`, then `train.py`. If the streaming
gate fails, run `mine_streaming_negatives.py` against the rejected candidate
and retrain. Keep the official OHF framework pinned to commit
`4665173cd35f1cff9a61e06fc427f124766c488e`. The validated local environment
uses Python 3.12, TensorFlow 2.19 and `pymicro-features` 2.0.2.

The voice model card names the OpenSLR MLS dataset and CC-BY 4.0. Keep its card,
the pinned framework commit, corpus manifest and metrics with any candidate.
Synthetic held-out metrics are a gate, not proof of acceptable real-room false
wakes. Do not enable a model by default until it passes physical trigger,
near-miss and several-hour ambient tests.

If synthetic voices do not clear that gate, install a firmware containing the
authenticated `/api/microphone/sample` endpoint and collect opt-in samples:

```sh
python collect_device_samples.py --url http://bureau.local \
  --out /private/path/hey-pog-positive --kind positive --count 30
python collect_device_samples.py --url http://bureau.local \
  --out /private/path/hey-pog-negative --kind near-miss --count 30
python add_device_samples.py --work /private/path/training \
  --samples /private/path/hey-pog-positive \
  --samples /private/path/hey-pog-negative
python train.py --work /private/path/training --framework /path/to/micro-wake-word
```

Every clip requires a separate Enter key, records exactly two seconds, and is
downloaded directly from the authenticated device to the operator's Mac. The
password and token are never stored. Keep these voice samples outside the
repository and delete them after the accepted model and its physical tests are
complete.

The audio frontend is run before the two-second training window is selected.
This preserves the noise-reduction and PCAN history that exists on a continuously
running satellite. Device captures remain whole: energy-only trimming can drop
one syllable of a short wake phrase. The exporter pins the documented feature
range (0–26) during int8 calibration.

The exported-model gate reproduces the firmware's five-probability rolling
average and primes the streaming state before each example. Validation and test
speakers and device utterances are disjoint from training. Every source in both
held-out splits must reach 85% recall with at most 1% false accepts before
`accepted` becomes true. `--negative-weight` controls the false-accept penalty;
the default is 8 while the upstream example recipe uses 20.
