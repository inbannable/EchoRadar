"""P0 offline probes, not a validated CS2 direction engine or event classifier.

Original EchoRadar implementation using the public energy-vector reference:
https://patents.google.com/patent/US9232337B2/en
FFT/hop, bands, sectors and consistency cutoff below are research choices,
not ASUS parameters. Windows convention: front=0, right=90 degrees.
"""
import argparse
from collections import deque
from pathlib import Path

import numpy as np

from p0 import read_wav, write_json

FFT = 1024
HOP = 480
BANDS = ((80, 250), (250, 500), (500, 1000), (1000, 2000), (2000, 4000), (4000, 12000))
ANGLES = {"FL": 330, "FR": 30, "FC": 0, "BL": 210, "BR": 150, "SL": 270, "SR": 90}


def direction_activity(spectrum, roles):
    if len(roles) not in (6, 8) or "LFE" not in roles:
        return None  # No stereo-to-surround inference.
    power = np.abs(spectrum) ** 2
    vector = np.zeros(power.shape[0], dtype=complex)
    total = np.zeros(power.shape[0])
    for channel, role in enumerate(roles):
        if role == "LFE":
            continue
        if role not in ANGLES:
            raise ValueError("unsupported direction role")
        vector += power[:, channel] * np.exp(1j * np.radians(ANGLES[role]))
        total += power[:, channel]
    magnitude = np.abs(vector)
    consistency = np.divide(magnitude, total, out=np.zeros_like(total), where=total > 1e-12)
    bearing = np.degrees(np.angle(vector)) % 360
    position = bearing / 15
    lower = np.floor(position).astype(int) % 24
    fraction = position - np.floor(position)
    weights = np.where(consistency >= 0.15, magnitude, 0)
    sectors = np.bincount(lower, weights=weights * (1 - fraction), minlength=24)
    sectors += np.bincount((lower + 1) % 24, weights=weights * fraction, minlength=24)
    peaks = [i for i in range(24) if sectors[i] > 1e-12 and
             sectors[i] >= sectors[(i - 1) % 24] and sectors[i] > sectors[(i + 1) % 24]]
    return {"sector_energy": sectors.tolist(),
            "peaks": [{"azimuth_deg": i * 15, "energy": float(sectors[i])}
                      for i in sorted(peaks, key=lambda i: (-sectors[i], i))[:4]],
            "validation": "channel_mixture_activity_only_not_verified_game_sources"}


def extract(samples, roles, rate=48000):
    if rate != 48000:
        raise ValueError("requires 48 kHz")
    window = np.hanning(FFT)
    bins = np.fft.rfftfreq(FFT, 1 / rate)
    masks = [(bins >= low) & (bins < high) for low, high in BANDS]
    history = deque(maxlen=8)
    floor = None
    previous = None
    rows = []
    non_lfe = [i for i, role in enumerate(roles) if role != "LFE"]
    if not roles or not non_lfe:
        raise ValueError("requires explicit usable roles")
    for end in range(FFT, len(samples) + 1, HOP):
        spectrum = np.fft.rfft(samples[end - FFT:end] * window[:, None], axis=0)
        power = np.abs(spectrum) ** 2 / np.sum(window ** 2)
        band_energy = np.array([float(power[mask][:, non_lfe].mean()) for mask in masks])
        if floor is None:
            floor = np.maximum(band_energy.copy(), 1e-12)
        snr = 10 * np.log10((band_energy + 1e-12) / (floor + 1e-12))
        flux = np.maximum(band_energy - (previous if previous is not None else band_energy), 0)
        # Slow rising / fast falling causal background tracker, uncalibrated.
        alpha = np.where(band_energy > floor, 0.005, 0.1)
        floor += alpha * (band_energy - floor)
        previous = band_energy
        row = {"window_start_ms": (end - FFT) / 48, "available_at_ms": end / 48,
               "band_energy": band_energy.tolist(), "band_snr_db": snr.tolist(),
               "positive_energy_flux": flux.tolist(),
               "direction_activity": direction_activity(spectrum, roles)}
        if roles == ["FL", "FR"]:
            history.append(spectrum)
            stacked = np.stack(history)
            cross = np.mean(stacked[:, :, 0] * np.conj(stacked[:, :, 1]), axis=0)
            left = np.mean(np.abs(stacked[:, :, 0]) ** 2, axis=0)
            right = np.mean(np.abs(stacked[:, :, 1]) ** 2, axis=0)
            features = []
            for mask in masks:
                l, r = float(left[mask].sum()), float(right[mask].sum())
                active = l > 1e-10 and r > 1e-10
                gcc_spectrum = np.zeros_like(cross)
                gcc_spectrum[mask] = cross[mask] / np.maximum(np.abs(cross[mask]), 1e-12)
                gcc = np.fft.irfft(gcc_spectrum, n=FFT)
                lags = np.arange(-48, 49)  # +/-1 ms, research bound.
                lag = int(lags[np.argmax(gcc[lags % FFT])])
                coherence = float(np.mean(np.abs(cross[mask]) ** 2 /
                                          np.maximum(left[mask] * right[mask], 1e-20)))
                features.append({"ild_right_minus_left_db": float(10 * np.log10((r + 1e-12) / (l + 1e-12))) if active else None,
                                 "lag_left_minus_right_us": lag / rate * 1e6 if active else None,
                                 "coherence": min(1., coherence) if active and len(history) == 8 else None})
            row["binaural_features"] = features
            row["direction_rejection"] = "no_calibrated_CS2_template; ILD/ITD do not resolve front/back"
        rows.append(row)
    return rows


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("wav", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    samples, metadata = read_wav(args.wav)
    write_json(args.output, {"schema": 1, "algorithm": "echoradar-p0-spectral-probe-v1",
                            "input": metadata, "fft": FFT, "hop": HOP, "bands_hz": BANDS,
                            "status": "uncalibrated_features_not_classification_confidence",
                            "frames": extract(samples, metadata["roles"], metadata["sample_rate"])})


if __name__ == "__main__":
    main()
