"""
export_rife.py — Download RIFE weights and export to ONNX (FP16, fixed resolution).

Supported versions: 4.4, 4.6 (default), 4.8

Usage:
    pip install torch
    python export_rife.py                                    # v4.6, 720p
    python export_rife.py --version 4.4                      # v4.4, 720p
    python export_rife.py --version 4.8 --width 1920 --height 1080
    python export_rife.py --out build/Release/rife_720p.onnx # explicit output path

Requires Python 3.10+, PyTorch (CPU-only build is fine for export).
"""

import argparse
import os
import math
import urllib.request

import torch
import torch.nn as nn
import torch.nn.functional as F


# ── Shared helpers ─────────────────────────────────────────────────────────────

def _conv(in_planes, out_planes, kernel_size=3, stride=1, padding=1, dilation=1):
    return nn.Sequential(
        nn.Conv2d(in_planes, out_planes, kernel_size=kernel_size, stride=stride,
                  padding=padding, dilation=dilation, bias=True),
        nn.LeakyReLU(0.2, True),
    )


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


# ── v4.4 architecture (IFNet_HDv3 v4.4) ───────────────────────────────────────
# IFBlock uses plain conv blocks (not ResConv) and a single ConvTranspose2d lastconv.

class _IFBlock_v44(nn.Module):
    def __init__(self, in_planes, c=64):
        super().__init__()
        self.conv0 = nn.Sequential(
            _conv(in_planes, c // 2, 3, 2, 1),
            _conv(c // 2, c, 3, 2, 1),
        )
        self.convblock = nn.Sequential(*[_conv(c, c) for _ in range(8)])
        self.lastconv = nn.ConvTranspose2d(c, 5, 4, 2, 1)

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
        tmp = F.interpolate(tmp, scale_factor=scale * 2, mode="bilinear",
                            align_corners=False, recompute_scale_factor=False)
        return tmp[:, :4] * scale * 2, tmp[:, 4:5]


class _IFNet_v44(nn.Module):
    def __init__(self, scale=1.0):
        super().__init__()
        self.block0 = _IFBlock_v44(7,         c=192)
        self.block1 = _IFBlock_v44(8 + 4,     c=128)
        self.block2 = _IFBlock_v44(8 + 4,     c=96)
        self.block3 = _IFBlock_v44(8 + 4,     c=64)
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


# ── v4.6 architecture (IFNet_HDv3 v4.6) ───────────────────────────────────────
# IFBlock uses ResConv blocks and PixelShuffle lastconv.

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


# ── v4.8 architecture (IFNet_HDv3 v4.8) ───────────────────────────────────────
# Adds a learned encode network that produces 4-channel feature maps per frame.
# These features are concatenated to the block inputs alongside the raw pixels.

class _IFNet_v48(nn.Module):
    def __init__(self, scale=1.0):
        super().__init__()
        # encode: produces 4-ch feature maps per frame (3→16 stride-2 → 4 back to full res)
        self.encode = nn.Sequential(
            nn.Conv2d(3, 16, 3, 2, 1),
            nn.ConvTranspose2d(16, 4, 4, 2, 1),
        )
        # block0: img0(3)+img1(3)+f0(4)+f1(4)+timestep(1) = 15 = 7+8
        self.block0 = _IFBlock(7 + 8,     c=192)
        # block1+: warped0(3)+warped1(3)+wf0(4)+wf1(4)+timestep(1)+mask(1) = 16, + flow(4) in IFBlock = 20
        self.block1 = _IFBlock(8 + 4 + 8, c=128)
        self.block2 = _IFBlock(8 + 4 + 8, c=96)
        self.block3 = _IFBlock(8 + 4 + 8, c=64)
        self.scale_list = [8 / scale, 4 / scale, 2 / scale, 1 / scale]

    def forward(self, img0, img1, timestep, tenFlow_div, backwarp_tenGrid):
        img0 = img0.clamp(0.0, 1.0)
        img1 = img1.clamp(0.0, 1.0)
        f0 = self.encode(img0)
        f1 = self.encode(img1)
        warped_img0, warped_img1 = img0, img1
        wf0, wf1 = f0, f1
        flow = mask = None
        blocks = [self.block0, self.block1, self.block2, self.block3]
        for i, blk in enumerate(blocks):
            if flow is None:
                flow, mask = blk(torch.cat((img0, img1, f0, f1, timestep), 1),
                                 None, scale=self.scale_list[i])
            else:
                wf0 = _warp(f0, flow[:, :2], tenFlow_div, backwarp_tenGrid)
                wf1 = _warp(f1, flow[:, 2:4], tenFlow_div, backwarp_tenGrid)
                fd, m0 = blk(
                    torch.cat((warped_img0, warped_img1, wf0, wf1, timestep, mask), 1),
                    flow, scale=self.scale_list[i])
                flow = flow + fd
                mask = mask + m0
            warped_img0 = _warp(img0, flow[:, :2], tenFlow_div, backwarp_tenGrid)
            warped_img1 = _warp(img1, flow[:, 2:4], tenFlow_div, backwarp_tenGrid)
        mask = torch.sigmoid(mask)
        return warped_img0 * mask + warped_img1 * (1 - mask)


# ── ONNX wrappers — bake in timestep=0.5, grid, and flow_div ──────────────────

class _RifeONNX(nn.Module):
    """Wraps v4.4 or v4.6 IFNet. Inputs: frame0, frame1 [1,3,H,W] fp16; output: mid fp16."""

    def __init__(self, flownet, h: int, w: int):
        super().__init__()
        self.flownet = flownet
        tenFlow_div = torch.tensor([(w - 1) / 2.0, (h - 1) / 2.0], dtype=torch.float32)
        tenH = torch.linspace(-1.0, 1.0, w).view(1, 1, 1, w).expand(1, 1, h, w)
        tenV = torch.linspace(-1.0, 1.0, h).view(1, 1, h, 1).expand(1, 1, h, w)
        backwarp_tenGrid = torch.cat([tenH, tenV], 1).contiguous()
        timestep = torch.full((1, 1, h, w), 0.5, dtype=torch.float32)
        self.register_buffer("_tenFlow_div", tenFlow_div)
        self.register_buffer("_backwarp_tenGrid", backwarp_tenGrid)
        self.register_buffer("_timestep", timestep)

    def forward(self, frame0: torch.Tensor, frame1: torch.Tensor) -> torch.Tensor:
        ts = self._timestep.to(frame0.dtype)
        return self.flownet(frame0, frame1, ts,
                            self._tenFlow_div, self._backwarp_tenGrid)


# ── Version registry ───────────────────────────────────────────────────────────

_BASE = "https://github.com/HolyWu/vs-rife/releases/download/model/"

VERSIONS = {
    "4.4": {
        "url":     _BASE + "flownet_v4.4.pkl",
        "cache":   "flownet_v4.4.pkl",
        "net_cls": _IFNet_v44,
    },
    "4.6": {
        "url":     _BASE + "flownet_v4.6.pkl",
        "cache":   "flownet_v4.6.pkl",
        "net_cls": _IFNet,
    },
    "4.8": {
        "url":     _BASE + "flownet_v4.8.pkl",
        "cache":   "flownet_v4.8.pkl",
        "net_cls": _IFNet_v48,
    },
}


# ── Helpers ───────────────────────────────────────────────────────────────────

def _download_weights(url: str, cache: str):
    if os.path.exists(cache) and os.path.getsize(cache) > 1_000_000:
        print(f"[export] using cached {cache}")
        return
    print(f"[export] downloading {url} ...")
    urllib.request.urlretrieve(url, cache + ".tmp",
                               reporthook=lambda b, bs, t:
                               print(f"  {min(b*bs, t)}/{t} bytes", end="\r"))
    os.replace(cache + ".tmp", cache)
    print(f"\n[export] saved {cache}")


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
    parser.add_argument("--version", default="4.6", choices=list(VERSIONS),
                        help="RIFE version to export (default: 4.6)")
    parser.add_argument("--width",   type=int, default=1280,
                        help="Input frame width  (default 1280 for 720p)")
    parser.add_argument("--height",  type=int, default=720,
                        help="Input frame height (default 720 for 720p)")
    parser.add_argument("--out",     default=None,
                        help="Output ONNX path (default: build/Release/rife_v{ver}_{res}.onnx)")
    parser.add_argument("--opset",   type=int, default=17)
    args = parser.parse_args()

    ver = VERSIONS[args.version]

    # Pad to multiple of 32 (matches C++ pipeline)
    pw = _pad32(args.width)
    ph = _pad32(args.height)
    print(f"[export] RIFE v{args.version}  resolution {args.width}x{args.height} → padded {pw}x{ph}")

    if args.out is None:
        res_tag = "720p" if args.height <= 720 else "1080p" if args.height <= 1080 else f"{args.height}p"
        ver_tag = args.version.replace(".", "")
        args.out = f"build/Release/rife_v{ver_tag}_{res_tag}.onnx"

    _download_weights(ver["url"], ver["cache"])

    # Build model
    flownet = ver["net_cls"](scale=1.0)
    sd = _load_weights(ver["cache"])
    missing, unexpected = flownet.load_state_dict(sd, strict=False)
    if missing:
        print(f"[export] WARNING missing keys: {missing}")
    if unexpected:
        print(f"[export] WARNING unexpected keys: {unexpected}")
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


if __name__ == "__main__":
    main()
