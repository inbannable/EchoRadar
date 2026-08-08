from __future__ import annotations


def require_torch():
    try:
        import torch
    except ImportError as error:
        raise RuntimeError("training requires: pip install -e 'ml[train]'") from error
    return torch


def build_model(input_channels: int = 2):
    torch = require_torch()
    nn = torch.nn

    class CausalConv(nn.Module):
        def __init__(self, in_channels: int, out_channels: int, kernel_time: int, kernel_freq: int):
            super().__init__()
            self.left = kernel_time - 1
            self.freq = kernel_freq // 2
            self.depthwise = nn.Conv2d(
                in_channels, in_channels, (kernel_time, kernel_freq), groups=in_channels, bias=False
            )
            self.pointwise = nn.Conv2d(in_channels, out_channels, 1, bias=False)
            self.norm = nn.BatchNorm2d(out_channels)

        def forward(self, values):
            values = torch.nn.functional.pad(values, (self.freq, self.freq, self.left, 0))
            return torch.nn.functional.silu(self.norm(self.pointwise(self.depthwise(values))))

    class TinyCausalCrnn(nn.Module):
        def __init__(self):
            super().__init__()
            self.front = nn.Sequential(
                CausalConv(input_channels, 16, 5, 5),
                CausalConv(16, 32, 5, 3),
                CausalConv(32, 48, 3, 3),
            )
            self.frequency_projection = nn.Linear(48 * 8, 64)
            self.gru = nn.GRU(64, 64, num_layers=1, batch_first=True)
            self.objectness_head = nn.Linear(64, 1)
            self.class_head = nn.Linear(64, 3)

        def forward(self, values):
            if input_channels == 2:
                # Preserve the package's raw [-100, 0] dB contract while
                # presenting comparable feature scales to the first layer.
                values = torch.cat((values[:, :1], (values[:, 1:2] + 100.0) / 100.0), dim=1)
            values = self.front(values)
            values = torch.nn.functional.avg_pool2d(values, kernel_size=(1, 8), stride=(1, 8))
            values = values.permute(0, 2, 1, 3).flatten(2)
            values = torch.nn.functional.silu(self.frequency_projection(values))
            values, _ = self.gru(values)
            return self.objectness_head(values), self.class_head(values)

    return TinyCausalCrnn()
