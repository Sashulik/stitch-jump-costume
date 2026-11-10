# Jump model

The sketch uses the six-axis ±8g FFT 1s model through the Edge Impulse Arduino library `AP8G1s` (v1.0.5). The generated SDK/model archive is not bundled in this public copy.

If you have access to the original export, install `AP8G1s_20261009_v01.zip` via **Sketch → Include Library → Add .ZIP Library**. Otherwise train an equivalent model on your own recordings and export an Arduino library from Edge Impulse. Match the input dimensions, sampling rate and labels below, and update the include in `AIEngine.h` to match your generated header. A replacement model will need its own validation. The firmware requires a compatible model library to compile.

| Setting | Value |
|---|---|
| Inputs | `ax, ay, az` in g; `gx, gy, gz` in degrees/second |
| Sensor ranges | ±8 g and ±2000 degrees/second |
| Sampling | 100 Hz, 100 samples per axis per window |
| Window / stride | 1 s / 200 ms |
| Spectral processing | FFT length 64, log spectrum, overlapping FFT frames, no filter, no decimation, no feature normalization |
| Features | 37 per axis (3 time statistics + 2 spectral statistics + 32 power bins), 222 total |
| Classifier | Dense 20 → 10 → 2, `jump` and `not_jump` |
| Firmware trigger | Jump score ≥ 0.50 when idle and eligible |

The adult-mounted experiment used 35 jump clips, 10 negative training recordings, and separate continuous test recordings. Reported int8 test accuracy was **80.39% (373/464 windows)**. This is a window-level result, not the fraction of physical jumps detected or a measurement on a child. Keep entire recordings separate between training and test; neighboring windows share samples.

For a new model, collect mounted-sensor data using the same units and ranges, split by recording, train and test in Edge Impulse, then export an Arduino library. Keep the sensor configuration and classifier window/axis settings aligned with the export. The project [data-to-model workflow](../docs/data_to_model_workflow_20261008_v01.png) illustrates the training path.
