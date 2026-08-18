# Direction validation

Direction promotion requires both packaged synthetic evaluation and held-out real-CS2 evidence. Passing unit tests or Steam Audio scenes alone is not an accuracy claim.

## Candidate checks

1. Generate independent train, development, and locked test scenes with 0–3 sources and at least 15 degrees training separation.
2. Train and package the Multi-ACCDOA model with its checksum, thresholds, and monotonic confidence-to-p90 uncertainty table.
3. Run ONNX parity and `direction-evaluate --model ... --split test --require-gates` without tuning against locked-test labels.
4. Confirm class disablement, source-count behavior, close-source cases, elevation error, catastrophic high-confidence error, and p95 inference time.
5. Run the Windows application against endpoint changes, missing/corrupt packages, recognition-only startup, and dual-model startup.

## Real-CS2 protocol

Use held-out gameplay recordings with synchronized visual ground truth and a fixed, recorded audio profile. Keep maps, capture sessions, rooms/headphones, elevation bands, and source layouts separated from training where applicable. Validate every supported audio profile independently.

Review the schema-2 session JSONL and saved shared scene clips. Check azimuth/elevation error, uncertainty coverage, missed and extra sources, event-to-scene association, end-to-end scene delivery latency, and behavior near the minimum source separation. A candidate remains a candidate until the real-data accuracy and latency gates pass.
