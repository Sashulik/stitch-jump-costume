# Jump model

The sketch uses the six-axis ±8g FFT 1s model through the Edge Impulse Arduino library `AP8G1s` (v1.0.5). The generated SDK/model archive is not bundled in this public copy.

If you have access to the original export, install `AP8G1s_20261009_v01.zip` via **Sketch → Include Library → Add .ZIP Library**. Otherwise train an equivalent model on your own recordings and export an Arduino library from Edge Impulse. Match the input dimensions, sampling rate and labels below, and update the include in `AIEngine.h` to match your generated header. A replacement model will need its own validation. The firmware requires a compatible model library to compile.

## Neural network

The classifier is a **fully connected feed-forward neural network**, also called a **multilayer perceptron (MLP)**. It classifies a vector of extracted features rather than raw sensor samples. It has no convolutional or recurrent layers and does not carry a hidden state between windows.

| Layer | Size | Activation |
|---|---:|---|
| Input | 222 features | — |
| Hidden dense layer | 20 neurons | ReLU |
| Hidden dense layer | 10 neurons | ReLU |
| Output dense layer | 2 logits | Linear |
| Output normalization | 2 class scores | Softmax |

This architecture has **4,692 learned weights and biases**: `(222 × 20 + 20) + (20 × 10 + 10) + (10 × 2 + 2)`. The two hidden ReLU activations, linear output and softmax are confirmed by the saved EON-generated inference code; layer sizes are recorded in the saved Edge Impulse training settings. No dropout layer appears in the deployed operator sequence.

The model runs locally as an **int8-quantized Edge Impulse EON export**. Quantization and deployment format are separate from the MLP architecture. Scores are model outputs, not a calibrated guarantee that a physical jump occurred.

## From motion to prediction

| Stage | Configuration |
|---|---|
| Inputs | `ax, ay, az` in g; `gx, gy, gz` in degrees/second |
| Sensor ranges | ±8 g and ±2000 degrees/second |
| Sampling | 100 Hz; 100 samples per axis per window (600 raw values) |
| Window / update | 1 s / 200 ms; neighboring windows share 80 samples per axis |
| Spectral processing | 64-point FFT, log spectrum, overlapping internal FFT frames, scale 1, no filter or decimation |
| Feature normalization | None |
| Class labels | `jump`, `not_jump` |
| Firmware trigger | Jump score ≥ 0.50, only when AI is enabled and no effect/feedback/cooldown blocks a trigger |

Each axis produces **37 features**:

| Feature group | Per axis | Across six axes |
|---|---:|---:|
| Signal RMS, skewness and kurtosis | 3 | 18 |
| Spectral skewness and kurtosis | 2 | 12 |
| Retained FFT power bins | 32 | 192 |
| **Total** | **37** | **222** |

The 222 values describe one window. Each prediction uses a fresh feature vector; overlapping input windows provide continuity, not recurrent-network memory. Internal FFT-frame overlap is a separate setting from the 200 ms inference/window stride.

The firmware turns eligible scores into a complete simultaneous up/down LED sweep, then applies a 500 ms cooldown. Model score, physical jump count and animation count are different measures; a prolonged high-score period can retrigger after the effect and cooldown.

## Recorded training settings

This is the **adult-mounted, six-axis ±8g FFT 1s experiment from October 9, 2026**. It used 35 extracted jump clips and 10 full negative training recordings, producing **1,620 windows: 105 jump and 1,515 not_jump** before the automatic validation split.

| Setting | Saved value |
|---|---|
| Training cycles | 100 |
| Learning rate | 0.0005 |
| Batch size | 32 |
| Training processor | CPU |
| Automatic validation split | 20% |
| Learned optimizer | Disabled |
| Automatic class weighting | Disabled |
| Int8 profiling | Enabled |

The original recordings were imbalanced. Internal validation can include related windows/events from the same source recording; it should not be treated as independent real-world validation. Continuous held-out test recordings were kept separate from training.

## Evaluation and limits

| Evaluation | Recorded result |
|---|---|
| Int8 internal validation accuracy | 100.0% |
| Int8 held-out test accuracy | **80.39% (373/464 windows)** |
| Float32 held-out test accuracy | 80.17% (372/464 windows) |

These are window-level results, **not the fraction of physical jumps detected**, not measured response latency, and not a test on a child. Overlapping windows can describe the same physical event more than once. The trained adult mounting/orientation and movement patterns may differ from another wearer or garment placement.

Earlier debug firmware logs showed nearly 16 minutes without I²C failures or sampling gaps after LED/sensor coordination was added. That establishes communication evidence for that run, not model detection accuracy. The interval counter is measured before acquiring the LED/I²C lock, so it does not directly establish read-acquisition jitter.

## Reproduce or retrain

1. Collect mounted-sensor data using the same axes, units, ranges and frequency, including jumps and realistic non-jump motions.
2. Keep entire recordings/sessions separate between training and held-out testing. Prepare jump training clips without editing the held-out recordings to make them easier.
3. Configure the processing and MLP settings above in Edge Impulse, then train and evaluate on held-out recordings. Evaluate physical events separately if you need missed-jump and false-trigger rates.
4. Export an Arduino model library and align its header, axes, window, frequency and labels with `AIEngine.h`. A new export requires its own validation; reproducing the architecture does not guarantee identical learned weights or results.

See the [data-to-model workflow](../docs/data_to_model_workflow_20261008_v01.png) and the [build and flashing guide](../README.md#flash-the-costume).
