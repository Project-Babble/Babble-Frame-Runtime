"""Eager rebuilds of the TorchScript models (verified max|diff| = 0 against the .pt files)."""
import torch, torch.nn as nn, timm
# The TorchScript .pt files from the PC reference package (models/ next to eye_osc_pc.py).

class FaceNet(nn.Module):  # face_uet3APL_160: shared convnext_nano trunk, per-view LayerNorm, MLP head
    def __init__(self, n_out):
        super().__init__()
        self.trunk = timm.create_model("convnext_nano", pretrained=False, in_chans=1, num_classes=0)
        self.norms = nn.ModuleList([nn.LayerNorm(640), nn.LayerNorm(640)])
        self.head = nn.Sequential(nn.Linear(1280, 512), nn.GELU(), nn.Dropout(0.0), nn.Linear(512, n_out))
    def forward(self, x):                       # x: [1,2,160,160]
        f = self.trunk(x.reshape(2, 1, 160, 160)).reshape(1, 2, -1)
        return torch.clamp(self.head(torch.cat([self.norms[0](f[:, 0]), self.norms[1](f[:, 1])], 1)), 0, 1)

class EyeNet(nn.Module):  # lid / pupil: timm trunk + Linear(feat,1), clamp
    def __init__(self, arch, feat):
        super().__init__()
        self.trunk = timm.create_model(arch, pretrained=False, in_chans=1, num_classes=0)
        self.head = nn.Linear(feat, 1)
    def forward(self, x):                       # x: [2,1,160,160]
        return torch.clamp(torch.squeeze(self.head(self.trunk(x)), 1), 0, 1)

def load(ts_path, eager):
    ts = torch.jit.load(ts_path, map_location="cpu").eval()
    sd = {k[2:]: v for k, v in ts.state_dict().items() if k.startswith("m.")}
    missing, _ = eager.load_state_dict(sd, strict=False)
    assert not missing, missing
    return ts, eager.eval()
