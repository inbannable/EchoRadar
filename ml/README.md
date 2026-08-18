# EchoRadar ML

The Python package contains the supported offline recognition and multi-source direction workflows. It does not contain a single-source direction mapper or model-less direction evaluation.

Create an environment and install the locked dependencies:

```powershell
py -3.12 -m venv ml\.venv
.\ml\.venv\Scripts\python.exe -m pip install -r ml\requirements-lock.txt
.\ml\.venv\Scripts\python.exe -m pip install -e ml
```

Run tests with:

```powershell
.\ml\.venv\Scripts\python.exe -m pytest ml\tests
```

The CLI exposes asset preparation, recognition sessions/training/evaluation, real-session import and annotation, multi-source direction scene generation, caching, training, parity, and packaged evaluation. `direction-evaluate` always requires `--model`.

See [recognition training](recognition-training.md) and [direction training](direction-training.md). Local generated data belongs under `ml/generated/`; run outputs belong under `ml/runs/`. Both, along with caches and checkpoints, are ignored by Git.
