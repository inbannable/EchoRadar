# Recognition Baseline v1 Report

## Verdict

**Not production-ready.** The baseline verifies the complete training-to-native-inference path, but it fails the agreed recognition gates. No real CS2 gameplay accuracy is claimed.

## Reproducibility

- Model: `cs2-recognizer-20260720`
- Model SHA-256: `09acb0b0fc838cc5b3cf2f0845a8542e6ad1b1b0cc8a95ec543703fde5a89ba6`
- Model size: 80,148 bytes
- Training: 640 seconds of generated audio
- Development: 260 seconds, used for event-level threshold selection
- Locked synthetic test: 260 seconds total; 100 simple and 160 complex
- Locked synthetic support: 26 gunshots, 27 footsteps, and 18 mechanical events
- PyTorch/ONNX maximum absolute difference: `1.78813934e-7`
- Native ONNX Runtime evaluation completed successfully on Windows CPU
- Native 20-second replay: 356 inferences, 1.292 ms p95 and 1.980 ms maximum model inference time

Every class has fewer than 30 locked-test events, so the evaluator marks all aggregate confidence intervals **inconclusive**. The numbers below are useful engineering signals, not accuracy claims.

## Locked synthetic results

| Stratum | Class | Precision | Recall | F1 | False alerts/min |
|---|---|---:|---:|---:|---:|
| Simple | Gunshot | 1.000 | 0.800 | 0.889 | 0.00 |
| Simple | Footstep | 0.308 | 0.800 | 0.444 | 5.40 |
| Simple | Mechanical | 0.308 | 0.800 | 0.444 | 5.40 |
| Complex | Gunshot | 0.583 | 0.333 | 0.424 | 1.88 |
| Complex | Footstep | 0.357 | 0.455 | 0.400 | 6.75 |
| Complex | Mechanical | 0.296 | 0.615 | 0.400 | 7.12 |
| All | Gunshot | 0.688 | 0.423 | 0.524 | 1.15 |
| All | Footstep | 0.341 | 0.519 | 0.412 | 6.23 |
| All | Mechanical | 0.300 | 0.667 | 0.414 | 6.46 |

## Interpretation

The model distinguishes some clean gunshots with few false positives, but it misses one of five simple held-out weapon-family examples and most gunshots in complex scenes. Footstep and mechanical outputs strongly confuse other transient sounds, causing unacceptable alert rates.

Increasing the synthetic corpus alone did not solve the domain and taxonomy problems. The next training iteration should use reviewed gameplay, especially the exported false-positive and miss clips, and should reconsider whether the broad `mechanical` class needs hierarchical sublabels during training even if the UI continues to show one category.

The detailed local JSON report and review clips are generated under the ignored model/run directories. Test sessions remain locked and must not be copied into the training split.
