from __future__ import annotations

import json
from pathlib import Path
import tempfile

import numpy as np

from . import CLASS_NAMES, SAMPLE_RATE
from .audio import Audio, load_pcm_wav, to_stereo_48k, write_pcm16_wav
from .features import log_mel


COLORS = {"gunshot": "#ff5555", "footstep": "#55dd88"}
DURATIONS = {"gunshot": 0.05, "footstep": 0.05}


def _load_jsonl(path: Path | None) -> list[dict]:
    if path is None or not path.exists():
        return []
    events = []
    for line in path.read_text(encoding="utf-8").splitlines():
        if not line:
            continue
        event = json.loads(line)
        if "class" not in event and event.get("sound_class") in CLASS_NAMES:
            event["class"] = event.pop("sound_class")
        if event.get("class") in CLASS_NAMES:
            events.append(event)
    return events


class TimelineReviewer:
    def __init__(self, wav_path: Path, labels_path: Path, predictions_path: Path | None = None):
        try:
            import tkinter as tk
        except ImportError as error:
            raise RuntimeError("timeline review requires Python's tkinter module") from error
        self.tk = tk
        self.wav_path = wav_path
        self.labels_path = labels_path
        self.audio = to_stereo_48k(load_pcm_wav(wav_path))
        existing = _load_jsonl(labels_path)
        seeded = _load_jsonl(predictions_path) if not existing else []
        self.events = existing or [
            {**event, "source": "model", "reviewed": False, "uncertain": False}
            for event in seeded
        ]
        self.cursor = min(len(self.audio.samples), 5 * SAMPLE_RATE)
        self.window_seconds = 10.0
        self.root = tk.Tk()
        self.root.title(f"EchoRadar Timeline Reviewer - {wav_path.name}")
        self.status = tk.StringVar()
        self.canvas = tk.Canvas(self.root, width=1100, height=620, bg="#101218", highlightthickness=0)
        self.canvas.pack(fill="both", expand=True)
        tk.Label(self.root, textvariable=self.status, anchor="w").pack(fill="x")
        self.canvas.bind("<Button-1>", self._click)
        self.root.bind("<Left>", lambda event: self._move(-0.10, event))
        self.root.bind("<Right>", lambda event: self._move(0.10, event))
        self.root.bind("<space>", self._play)
        self.root.bind("1", lambda event: self._add("gunshot"))
        self.root.bind("2", lambda event: self._add("footstep"))
        self.root.bind("u", self._toggle_uncertain)
        self.root.bind("l", lambda event: self._set_source("self"))
        self.root.bind("r", lambda event: self._set_source("remote"))
        self.root.bind("n", lambda event: self._set_source("unknown"))
        self.root.bind("<Delete>", self._delete_nearest)
        self.root.bind("<BackSpace>", self._delete_nearest)
        self.root.bind("s", lambda event: self.save())
        self.root.protocol("WM_DELETE_WINDOW", self._close)
        self.draw()

    def _window(self) -> tuple[int, int]:
        width = int(self.window_seconds * SAMPLE_RATE)
        start = max(0, min(self.cursor - width // 2, max(0, len(self.audio.samples) - width)))
        return start, min(len(self.audio.samples), start + width)

    def _move(self, seconds: float, event=None):
        if event is not None and (event.state & 0x0001):
            seconds *= 10
        self.cursor = int(np.clip(self.cursor + seconds * SAMPLE_RATE, 0, len(self.audio.samples)))
        self.draw()

    def _click(self, event):
        start, end = self._window()
        width = max(1, self.canvas.winfo_width())
        self.cursor = start + int(np.clip(event.x / width, 0.0, 1.0) * (end - start))
        self.draw()

    def _nearest(self, maximum_seconds: float = 0.30):
        candidates = [(abs(int(event["onset_sample"]) - self.cursor), index)
                      for index, event in enumerate(self.events)]
        if not candidates:
            return None
        distance, index = min(candidates)
        return index if distance <= maximum_seconds * SAMPLE_RATE else None

    def _add(self, name: str):
        end = min(len(self.audio.samples), self.cursor + int(DURATIONS[name] * SAMPLE_RATE))
        self.events.append({
            "class": name, "onset_sample": self.cursor, "end_sample": end,
            "source": "manual", "source_hint": "unknown", "reviewed": True,
            "uncertain": False, "scene_mode": "unknown",
        })
        self.events.sort(key=lambda event: int(event["onset_sample"]))
        self.draw()

    def _toggle_uncertain(self, _event=None):
        index = self._nearest()
        if index is not None:
            self.events[index]["uncertain"] = not bool(self.events[index].get("uncertain", False))
            self.events[index]["reviewed"] = True
            self.draw()

    def _set_source(self, source_hint: str):
        index = self._nearest()
        if index is not None:
            self.events[index]["source_hint"] = source_hint
            self.events[index]["reviewed"] = True
            self.draw()

    def _delete_nearest(self, _event=None):
        index = self._nearest()
        if index is not None:
            del self.events[index]
            self.draw()

    def _play(self, _event=None):
        if not self.audio.samples.size:
            return
        try:
            import winsound
        except ImportError:
            self.status.set("Playback is available on Windows only")
            return
        half = SAMPLE_RATE
        start = max(0, self.cursor - half)
        end = min(len(self.audio.samples), self.cursor + half)
        path = Path(tempfile.gettempdir()) / "echoradar_timeline_preview.wav"
        write_pcm16_wav(path, Audio(self.audio.samples[start:end], SAMPLE_RATE))
        winsound.PlaySound(str(path), winsound.SND_FILENAME | winsound.SND_ASYNC)

    def draw(self):
        canvas = self.canvas
        canvas.delete("all")
        width = max(800, canvas.winfo_width())
        start, end = self._window()
        segment = self.audio.samples[start:end]
        if len(segment):
            columns = min(width, len(segment))
            edges = np.linspace(0, len(segment), columns + 1, dtype=np.int64)
            mono = segment.mean(axis=1)
            center_y = 110
            scale = 95
            points = []
            for column in range(columns):
                block = mono[edges[column] : max(edges[column] + 1, edges[column + 1])]
                points.extend((column * width / columns, center_y - float(np.max(block)) * scale))
            if len(points) >= 4:
                canvas.create_line(*points, fill="#8ad", width=1)

            features = log_mel(segment)
            if len(features):
                time_bins = min(260, len(features))
                indices = np.linspace(0, len(features) - 1, time_bins).astype(np.int64)
                display = features[indices]
                display = np.clip(display / max(1e-6, float(np.percentile(display, 99))), 0.0, 1.0)
                top, bottom = 230, 570
                cell_width = width / time_bins
                cell_height = (bottom - top) / display.shape[1]
                for x, column in enumerate(display):
                    for mel, value in enumerate(column):
                        intensity = int(value * 255)
                        color = f"#{intensity:02x}{int(intensity * 0.65):02x}{255-intensity:02x}"
                        y = bottom - (mel + 1) * cell_height
                        canvas.create_rectangle(x * cell_width, y, (x + 1) * cell_width,
                                                y + cell_height + 1, fill=color, outline="")

        for event in self.events:
            onset = int(event["onset_sample"])
            if onset < start or onset > end:
                continue
            x = (onset - start) * width / max(1, end - start)
            color = "#ffee55" if event.get("uncertain") else COLORS[event["class"]]
            canvas.create_line(x, 0, x, 610, fill=color, width=2)
            source = event.get("source_hint", "unknown")
            canvas.create_text(x + 3, 205, text=f"{event['class']}:{source}", fill=color, anchor="sw")
        cursor_x = (self.cursor - start) * width / max(1, end - start)
        canvas.create_line(cursor_x, 0, cursor_x, 610, fill="white", width=2)
        self.status.set(
            f"{self.cursor / SAMPLE_RATE:.3f}s / {len(self.audio.samples) / SAMPLE_RATE:.1f}s | "
            f"events={len(self.events)} | 1 gunshot  2 footstep  L self  R remote  N unknown  "
            "U uncertain  Delete remove  Space play  S save  Shift+Arrow fast"
        )

    def save(self):
        self.labels_path.parent.mkdir(parents=True, exist_ok=True)
        temporary = self.labels_path.with_suffix(self.labels_path.suffix + ".tmp")
        with temporary.open("w", encoding="utf-8") as stream:
            for event in sorted(self.events, key=lambda item: int(item["onset_sample"])):
                stream.write(json.dumps(event, sort_keys=True) + "\n")
        temporary.replace(self.labels_path)
        self.status.set(f"Saved {len(self.events)} events to {self.labels_path}")

    def _close(self):
        self.save()
        self.root.destroy()

    def run(self):
        self.root.mainloop()


def review_timeline(wav: str | Path, labels: str | Path, predictions: str | Path | None = None) -> None:
    TimelineReviewer(Path(wav), Path(labels), Path(predictions) if predictions else None).run()
