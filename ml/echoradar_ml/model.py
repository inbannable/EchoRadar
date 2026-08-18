from __future__ import annotations

from . import CLASS_NAMES, INPUT_CHANNELS, SOURCE_NAMES


def require_torch():
    try:
        import torch
    except ImportError as error:
        raise RuntimeError("training requires: pip install -r ml/requirements-lock.txt") from error
    return torch


def build_model(
    input_channels: int = INPUT_CHANNELS,
    class_count: int = len(CLASS_NAMES),
    source_count: int = len(SOURCE_NAMES),
):
    """Build the compact causal onset/source network.

    The acoustic branch sees only channel-order-invariant energy.  Spatial
    planes join after their own small front-end, preventing phase or ILD from
    canceling recognition evidence while still supporting self suppression.
    """
    if input_channels != 5:
        raise ValueError("stereo-onset-v4 requires five input planes")
    torch = require_torch()
    nn = torch.nn

    class CausalConv(nn.Module):
        def __init__(self, in_channels: int, out_channels: int, kernel_time: int, kernel_freq: int):
            super().__init__()
            self.left = kernel_time - 1
            self.freq = kernel_freq // 2
            self.depthwise = nn.Conv2d(
                in_channels, in_channels, (kernel_time, kernel_freq),
                groups=in_channels, bias=False,
            )
            self.pointwise = nn.Conv2d(in_channels, out_channels, 1, bias=False)
            self.norm = nn.BatchNorm2d(out_channels)

        def forward(self, values):
            values = torch.nn.functional.pad(values, (self.freq, self.freq, self.left, 0))
            values = self.depthwise(values)
            return torch.nn.functional.silu(self.norm(self.pointwise(values)))

    class CausalOnsetNet(nn.Module):
        def __init__(self):
            super().__init__()
            self.acoustic_front = nn.Sequential(
                CausalConv(2, 24, 5, 5),
                CausalConv(24, 32, 5, 3),
                CausalConv(32, 32, 3, 3),
            )
            self.spatial_front = nn.Sequential(
                CausalConv(3, 12, 5, 5),
                CausalConv(12, 16, 3, 3),
            )
            self.acoustic_projection = nn.Linear(32 * 8, 96)
            self.spatial_projection = nn.Linear(16 * 8, 32)
            self.acoustic_gru = nn.GRU(96, 96, num_layers=1, batch_first=True)
            self.spatial_gru = nn.GRU(32, 32, num_layers=1, batch_first=True)
            self.onset_head = nn.Linear(96, class_count)
            self.source_head = nn.Linear(96 + 32, class_count * source_count)

        def forward(self, values):
            acoustic = torch.cat((values[:, :1], (values[:, 1:2] + 100.0) / 100.0), dim=1)
            spatial = values[:, 2:]
            acoustic = self.acoustic_front(acoustic)
            spatial = self.spatial_front(spatial)
            acoustic = torch.nn.functional.avg_pool2d(
                acoustic, kernel_size=(1, 8), stride=(1, 8)
            ).permute(0, 2, 1, 3).flatten(2)
            spatial = torch.nn.functional.avg_pool2d(
                spatial, kernel_size=(1, 8), stride=(1, 8)
            ).permute(0, 2, 1, 3).flatten(2)
            acoustic = torch.nn.functional.silu(self.acoustic_projection(acoustic))
            spatial = torch.nn.functional.silu(self.spatial_projection(spatial))
            acoustic, _ = self.acoustic_gru(acoustic)
            spatial, _ = self.spatial_gru(spatial)
            onset_logits = self.onset_head(acoustic)
            source_values = torch.cat((acoustic, spatial), dim=-1)
            source_logits = self.source_head(source_values).reshape(
                source_values.shape[0], source_values.shape[1], class_count, source_count
            )
            return onset_logits, source_logits

    return CausalOnsetNet()
