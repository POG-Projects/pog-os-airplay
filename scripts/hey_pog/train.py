"""Train and export the reproducible Hey Pog candidate built by prepare.py."""
import argparse
import json
import sys
from pathlib import Path
from types import SimpleNamespace

import numpy as np
import tensorflow as tf


def rates(labels, scores, threshold):
    predicted = scores >= threshold
    positive = labels == 1
    return (float(np.mean(predicted[positive])), float(np.mean(predicted[~positive])))


def source_rates(labels, scores, threshold, device_start):
    """Return recalls/FPRs without allowing the larger synthetic set to hide
    a failure on the opt-in physical-device corpus."""
    result = {}
    slices = {"synthetic": slice(0, device_start),
              "device": slice(device_start, None)} if device_start is not None else {
                  "all": slice(None)}
    for name, selected in slices.items():
        y, s = labels[selected], scores[selected]
        result[name] = rates(y, s, threshold)
    return result


def select_threshold(labels, scores, device_start):
    choices = []
    for threshold in np.linspace(.5, .999, 500):
        split = source_rates(labels, scores, threshold, device_start)
        recalls = [value[0] for value in split.values()]
        false_rates = [value[1] for value in split.values()]
        viable = max(false_rates) <= .01
        # First prefer a threshold that meets the false-wake gate on every
        # source. If none does, retain the least-bad useful checkpoint.
        objective = (1 if viable else 0, min(recalls) - 4 * sum(false_rates),
                     sum(recalls) / len(recalls))
        choices.append((objective, threshold, split))
    return max(choices)


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--work", type=Path, required=True)
    p.add_argument("--framework", type=Path, required=True)
    p.add_argument("--steps", type=int, default=5000)
    p.add_argument("--channels", type=int, default=40)
    p.add_argument("--negative-weight", type=float, default=8.0,
                   help="penalty for false accepts; official recipes commonly use 20")
    args = p.parse_args()
    sys.path.insert(0, str(args.framework.resolve()))
    from microwakeword import mixednet, utils
    from microwakeword.layers import modes

    tf.keras.utils.set_random_seed(29142026)
    tf.config.threading.set_intra_op_parallelism_threads(8)
    tf.config.threading.set_inter_op_parallelism_threads(2)
    data = {}
    device_starts = {}
    device_report_path = args.work / "device-samples.json"
    device_report = json.loads(device_report_path.read_text()) if device_report_path.exists() else None
    for split in ("training", "validation", "testing"):
        with np.load(args.work / f"{split}.npz") as z:
            data[split] = (z["x"], z["y"])
        added = sum(device_report["splits"][split].values()) if device_report else 0
        device_starts[split] = len(data[split][1]) - added if added else None
    channels = args.channels
    flags = SimpleNamespace(pointwise_filters=",".join([str(channels)] * 4),
        repeat_in_block="1,1,1,1", mixconv_kernel_sizes="[5],[7,11],[9,15],[23]",
        residual_connection="0,0,0,0", first_conv_filters=max(16, channels // 2),
        first_conv_kernel_size=5, stride=3, max_pool=0, pooled=0,
        spatial_attention=0)
    model = mixednet.model(flags, shape=(200, 40), batch_size=None)
    model.compile(optimizer=tf.keras.optimizers.Adam(6e-4),
                  loss=tf.keras.losses.BinaryCrossentropy(label_smoothing=.03))
    rng = np.random.default_rng(29142026)
    x, y = data["training"]
    best = ((-1, -1.0, -1.0), None, None)
    best_path = args.work / "hey_pog_best.weights.h5"
    train_start = device_starts["training"]
    synthetic = np.arange(0, train_start if train_start is not None else len(y))
    device = np.arange(train_start, len(y)) if train_start is not None else np.empty(0, int)
    synth_positive = synthetic[y[synthetic] == 1]
    synth_negative = synthetic[y[synthetic] == 0]
    device_positive = device[y[device] == 1]
    device_negative = device[y[device] == 0]
    for step in range(args.steps):
        # Give the real device its own batch quota; uniform sampling over the
        # much larger TTS corpus otherwise makes a small opt-in corpus inert.
        if len(device_positive) and len(device_negative):
            selected = np.concatenate((
                rng.choice(synth_positive, 32, replace=True),
                rng.choice(device_positive, 32, replace=True),
                rng.choice(synth_negative, 24, replace=True),
                rng.choice(device_negative, 24, replace=True)))
            xb = np.concatenate((x[selected], np.zeros((16, 200, 40), np.float32)))
            yb = np.concatenate((np.ones(64, np.float32), np.zeros(64, np.float32)))
        else:
            positive = rng.choice(np.flatnonzero(y == 1), 64, replace=True)
            negative = rng.choice(np.flatnonzero(y == 0), 48, replace=True)
            xb = np.concatenate((x[positive], x[negative],
                                 np.zeros((16, 200, 40), np.float32)))
            yb = np.concatenate((np.ones(64, np.float32), np.zeros(64, np.float32)))
        # Frontend floor and low stationary room bands during an idle stream.
        xb[-16:] = rng.exponential(.2, xb[-16:].shape).astype(np.float32)
        order = rng.permutation(128); xb, yb = xb[order], yb[order]
        # Small feature masks make the trigger less dependent on one band/frame.
        for sample in xb:
            if rng.random() < .5:
                start = rng.integers(0, 38); sample[:, start:start + 2] = 0
            if rng.random() < .25:
                start = rng.integers(0, 197); sample[start:start + 3] = 0
        if step == int(args.steps * .65):
            model.optimizer.learning_rate.assign(1.5e-4)
        weights = np.where(yb == 0, args.negative_weight, 1.0)
        loss = float(model.train_on_batch(xb, yb, sample_weight=weights))
        if step % 100 == 99 or step + 1 == args.steps:
            vx, vy = data["validation"]
            scores = model.predict(vx, batch_size=128, verbose=0).reshape(-1)
            result = select_threshold(vy, scores, device_starts["validation"])
            detail = " ".join(f"{name}_recall={value[0]:.3f} {name}_fpr={value[1]:.3f}"
                              for name, value in result[2].items())
            print(f"step={step+1} loss={loss:.5f} threshold={result[1]:.3f} {detail}",
                  flush=True)
            if result[0] > best[0]:
                best = result
                model.save_weights(best_path)
    model.load_weights(best_path)
    config = {"spectrogram_length": 200, "stride": 3,
              "train_dir": str(args.work / "export")}
    export = Path(config["train_dir"]) / "stream"
    utils.convert_model_saved(model, config, str(export),
                              modes.Modes.STREAM_INTERNAL_STATE_INFERENCE)
    converter = tf.lite.TFLiteConverter.from_saved_model(str(export))
    converter.optimizations = {tf.lite.Optimize.DEFAULT}
    converter._experimental_variable_quantization = True
    converter.target_spec.supported_ops = {tf.lite.OpsSet.TFLITE_BUILTINS_INT8}
    converter.inference_input_type = tf.int8
    converter.inference_output_type = tf.uint8
    def representative():
        calibration = (np.concatenate((x[:250], x[-250:])) if train_start is not None
                       else x[:500]).copy()
        # Pin the documented micro-frontend range. Omitting the endpoints can
        # make an otherwise sound streaming model saturate after int8 export.
        calibration[0, 0, 0] = 0.0
        calibration[0, 0, 1] = 26.0
        for spectrogram in calibration:
            for i in range(0, 198, 3):
                yield [spectrogram[i:i + 3].astype(np.float32)[None, ...]]
    converter.representative_dataset = representative
    tflite = converter.convert()
    candidate = args.work / "hey_pog.tflite"
    candidate.write_bytes(tflite)

    # Evaluate the exported stateful integer model on voices excluded from training.
    interpreter = tf.lite.Interpreter(model_path=str(candidate))
    interpreter.allocate_tensors()
    input_info, output_info = interpreter.get_input_details()[0], interpreter.get_output_details()[0]
    def score(spectrogram):
        interpreter.reset_all_variables()
        scale, zero = input_info["quantization"]
        ambient = np.clip(np.rint(spectrogram[:3] / scale + zero), -128, 127).astype(np.int8)
        for _ in range(100):
            interpreter.set_tensor(input_info["index"], ambient[None, ...])
            interpreter.invoke()
        values = []
        for i in range(0, 198, 3):
            q = np.clip(np.rint(spectrogram[i:i+3] / scale + zero), -128, 127).astype(np.int8)
            interpreter.set_tensor(input_info["index"], q[None, ...])
            interpreter.invoke()
            values.append(int(interpreter.get_tensor(output_info["index"]).reshape(-1)[0]) / 255)
        return float(max(np.convolve(values, np.ones(5) / 5, "valid")))
    all_scores = {}
    for split in ("validation", "testing"):
        all_scores[split] = np.asarray([score(s) for s in data[split][0]])
    vy = data["validation"][1]
    deployed = select_threshold(vy, all_scores["validation"], device_starts["validation"])
    metrics = {"threshold": float(deployed[1]), "model_bytes": len(tflite), "splits": {}}
    for split in ("validation", "testing"):
        sx, sy = data[split]
        scores = all_scores[split]
        recall, fpr = rates(sy, scores, deployed[1])
        source = source_rates(sy, scores, deployed[1], device_starts[split])
        metrics["splits"][split] = {"examples": len(sy), "positive": int(sy.sum()),
            "recall": recall, "false_positive_rate": fpr,
            "positive_p05": float(np.quantile(scores[sy == 1], .05)),
            "negative_p995": float(np.quantile(scores[sy == 0], .995)),
            "sources": {name: {"recall": value[0], "false_positive_rate": value[1]}
                        for name, value in source.items()}}
    metrics["accepted"] = all(
        values["recall"] >= .85 and values["false_positive_rate"] <= .01
        for split in metrics["splits"].values() for values in split["sources"].values())
    (args.work / "metrics.json").write_text(json.dumps(metrics, indent=2) + "\n")
    print(json.dumps(metrics, indent=2), flush=True)


if __name__ == "__main__":
    main()
