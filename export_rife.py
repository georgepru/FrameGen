"""
export_rife.py — Download RIFE 4.6 weights and export to ONNX (FP16, fixed resolution).

Usage:
    pip install torch requests
    python export_rife.py                        # 1280x720 → rife.onnx
    python export_rife.py --width 1920 --height 1080
    python export_rife.py --out build/Release/rife.onnx

Requires Python 3.10+, PyTorch (CPU-only build is fine for export).
"""

import argparse
import os
import math
import urllib.request

import torch
import torch.nn as nn
import torch.nn.functional as F


# ── Architecture (IFNet_HDv3 v4.6, inlined to avoid vs-rife dependency) ───────

def _conv(in_planes, out_planes, kernel_size=3, stride=1, padding=1, dilation=1):
    return nn.Sequential(
        nn.Conv2d(in_planes, out_planes, kernel_size=kernel_size, stride=stride,
                  padding=padding, dilation=dilation, bias=True),
        nn.LeakyReLU(0.2, True),
    )


class _ResConv(nn.Module):
    def __init__(self, c, dilation=1):
        super().__init__()
        self.conv = nn.Conv2d(c, c, 3, 1, dilation, dilation=dilation)
        self.beta = nn.Parameter(torch.ones((1, c, 1, 1)))
        self.relu = nn.LeakyReLU(0.2, True)

    def forward(self, x):
        return self.relu(self.conv(x) * self.beta + x)


class _IFBlock(nn.Module):
    def __init__(self, in_planes, c=64):
        super().__init__()
        self.conv0 = nn.Sequential(
            _conv(in_planes, c // 2, 3, 2, 1),
            _conv(c // 2, c, 3, 2, 1),
        )
        self.convblock = nn.Sequential(*[_ResConv(c) for _ in range(8)])
        self.lastconv = nn.Sequential(
            nn.ConvTranspose2d(c, 4 * 6, 4, 2, 1),
            nn.PixelShuffle(2),
        )

    def forward(self, x, flow=None, scale=1):
        x = F.interpolate(x, scale_factor=1.0 / scale, mode="bilinear",
                          align_corners=False, recompute_scale_factor=False)
        if flow is not None:
            flow = F.interpolate(flow, scale_factor=1.0 / scale, mode="bilinear",
                                 align_corners=False, recompute_scale_factor=False) / scale
            x = torch.cat((x, flow), 1)
        feat = self.conv0(x)
        feat = self.convblock(feat)
        tmp = self.lastconv(feat)
        tmp = F.interpolate(tmp, scale_factor=scale, mode="bilinear",
                            align_corners=False, recompute_scale_factor=False)
        return tmp[:, :4] * scale, tmp[:, 4:5]


def _warp(tenInput, tenFlow, tenFlow_div, backwarp_tenGrid):
    dtype = tenInput.dtype
    tenInput = tenInput.to(torch.float32)
    tenFlow = tenFlow.to(torch.float32)
    tenFlow = torch.cat(
        [tenFlow[:, 0:1] / tenFlow_div[0], tenFlow[:, 1:2] / tenFlow_div[1]], 1
    )
    g = (backwarp_tenGrid + tenFlow).permute(0, 2, 3, 1)
    return F.grid_sample(tenInput, g, mode="bilinear",
                         padding_mode="border", align_corners=True).to(dtype)


class _IFNet(nn.Module):
    def __init__(self, scale=1.0):
        super().__init__()
        self.block0 = _IFBlock(7,      c=192)
        self.block1 = _IFBlock(8 + 4,  c=128)
        self.block2 = _IFBlock(8 + 4,  c=96)
        self.block3 = _IFBlock(8 + 4,  c=64)
        self.scale_list = [8 / scale, 4 / scale, 2 / scale, 1 / scale]

    def forward(self, img0, img1, timestep, tenFlow_div, backwarp_tenGrid):
        img0 = img0.clamp(0.0, 1.0)
        img1 = img1.clamp(0.0, 1.0)
        warped_img0, warped_img1 = img0, img1
        flow = mask = None
        blocks = [self.block0, self.block1, self.block2, self.block3]
        for i, blk in enumerate(blocks):
            if flow is None:
                flow, mask = blk(torch.cat((img0, img1, timestep), 1),
                                 None, scale=self.scale_list[i])
            else:
                f0, m0 = blk(torch.cat((warped_img0, warped_img1, timestep, mask), 1),
                              flow, scale=self.scale_list[i])
                flow = flow + f0
                mask = mask + m0
            warped_img0 = _warp(img0, flow[:, :2], tenFlow_div, backwarp_tenGrid)
            warped_img1 = _warp(img1, flow[:, 2:4], tenFlow_div, backwarp_tenGrid)
        mask = torch.sigmoid(mask)
        return warped_img0 * mask + warped_img1 * (1 - mask)


# ── ONNX wrapper — bakes in timestep=0.5, grid, and flow_div ─────────────────

class _RifeONNX(nn.Module):
    """Wraps IFNet — inputs: frame0, frame1 [1,3,H,W] fp16; output: mid [1,3,H,W] fp16."""

    def __init__(self, flownet: _IFNet, h: int, w: int):
        super().__init__()
        self.flownet = flownet

        tenFlow_div = torch.tensor(
            [(w - 1) / 2.0, (h - 1) / 2.0], dtype=torch.float32
        )
        tenH = torch.linspace(-1.0, 1.0, w).view(1, 1, 1, w).expand(1, 1, h, w)
        tenV = torch.linspace(-1.0, 1.0, h).view(1, 1, h, 1).expand(1, 1, h, w)
        backwarp_tenGrid = torch.cat([tenH, tenV], 1).contiguous()
        timestep = torch.full((1, 1, h, w), 0.5, dtype=torch.float32)

        # Buffers stay float32; warp() handles internal dtype conversion
        self.register_buffer("_tenFlow_div", tenFlow_div)
        self.register_buffer("_backwarp_tenGrid", backwarp_tenGrid)
        self.register_buffer("_timestep", timestep)

    def forward(self, frame0: torch.Tensor, frame1: torch.Tensor) -> torch.Tensor:
        ts = self._timestep.to(frame0.dtype)
        return self.flownet(frame0, frame1, ts,
                            self._tenFlow_div, self._backwarp_tenGrid)


# ── Helpers ───────────────────────────────────────────────────────────────────

WEIGHTS_URL = "https://github.com/HolyWu/vs-rife/releases/download/model/flownet_v4.6.pkl"
WEIGHTS_CACHE = "flownet_v4.6.pkl"


def _download_weights():
    if os.path.exists(WEIGHTS_CACHE) and os.path.getsize(WEIGHTS_CACHE) > 1_000_000:
        print(f"[export] using cached {WEIGHTS_CACHE}")
        return
    print(f"[export] downloading {WEIGHTS_URL} ...")
    urllib.request.urlretrieve(WEIGHTS_URL, WEIGHTS_CACHE + ".tmp",
                               reporthook=lambda b, bs, t:
                               print(f"  {min(b*bs, t)}/{t} bytes", end="\r"))
    os.replace(WEIGHTS_CACHE + ".tmp", WEIGHTS_CACHE)
    print(f"\n[export] saved {WEIGHTS_CACHE}")


def _load_weights(path: str) -> dict:
    sd = torch.load(path, map_location="cpu", weights_only=True)
    # Strip 'module.' prefix that vs-rife saves with
    return {k.replace("module.", ""): v
            for k, v in sd.items() if "module." in k}


def _pad32(v: int) -> int:
    return math.ceil(v / 32) * 32


# ── Main ──────────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--width",  type=int, default=1280,
                        help="Input frame width  (default 1280 for 720p)")
    parser.add_argument("--height", type=int, default=720,
                        help="Input frame height (default 720 for 720p)")
    parser.add_argument("--out",    default="rife.onnx",
                        help="Output ONNX path")
    parser.add_argument("--opset",  type=int, default=17)
    args = parser.parse_args()

    # Pad to multiple of 32 (matches C++ pipeline)
    pw = _pad32(args.width)
    ph = _pad32(args.height)
    print(f"[export] resolution {args.width}x{args.height} → padded {pw}x{ph}")

    _download_weights()

    # Build model
    flownet = _IFNet(scale=1.0)
    sd = _load_weights(WEIGHTS_CACHE)
    missing, unexpected = flownet.load_state_dict(sd, strict=False)
    if missing:
        print(f"[export] WARNING missing keys: {missing}")
    flownet.eval()

    wrapper = _RifeONNX(flownet, ph, pw)
    wrapper.half().eval()

    # Example inputs (FP16)
    frame0 = torch.zeros(1, 3, ph, pw, dtype=torch.float16)
    frame1 = torch.zeros(1, 3, ph, pw, dtype=torch.float16)

    out_dir = os.path.dirname(args.out)
    if out_dir:
        os.makedirs(out_dir, exist_ok=True)

    print(f"[export] exporting to {args.out} (opset {args.opset}) ...")
    with torch.no_grad():
        torch.onnx.export(
            wrapper,
            (frame0, frame1),
            args.out,
            opset_version=args.opset,
            input_names=["frame0", "frame1"],
            output_names=["mid"],
            dynamic_axes=None,   # fixed resolution
        )
    size_mb = os.path.getsize(args.out) / 1e6
    print(f"[export] done — {args.out} ({size_mb:.1f} MB)")
    print(f"\nNext step: copy {args.out} to build\\Release\\rife.onnx")


if __name__ == "__main__":
    main()
