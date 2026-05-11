#!/usr/bin/env python3
"""Convert DeepSeek-V4-Flash MTP safetensors shard to DS4's compact GGUF.

This is intentionally scoped to the MTP sidecar: it reads the official
`model-00046-of-00046.safetensors` shard, repacks the per-expert tensors into
DS4's 3D expert layout, and writes a GGUF sidecar with Q8_0 routed experts.
"""

from __future__ import annotations

import argparse
import tempfile
from pathlib import Path

import numpy as np
import torch
from gguf import GGMLQuantizationType, GGUFValueType, GGUFWriter, quantize
from safetensors import safe_open


N_EXPERT = 256
FP4_E2M1 = np.array(
    [
        0.0, 0.5, 1.0, 1.5, 2.0, 3.0, 4.0, 6.0,
        -0.0, -0.5, -1.0, -1.5, -2.0, -3.0, -4.0, -6.0,
    ],
    dtype=np.float32,
)


def to_f32(st, name: str) -> np.ndarray:
    return st.get_tensor(name).to(torch.float32).cpu().numpy().astype(np.float32, copy=False)


def dequant_fp8_block(st, weight_name: str, scale_name: str) -> np.ndarray:
    w = st.get_tensor(weight_name).to(torch.float32).cpu().numpy().astype(np.float32, copy=False)
    s = st.get_tensor(scale_name).to(torch.float32).cpu().numpy().astype(np.float32, copy=False)
    if s.ndim != 2:
        raise ValueError(f"{scale_name}: expected 2D scale, got {s.shape}")
    out, in_dim = w.shape
    if s.shape != ((out + 127) // 128, (in_dim + 127) // 128):
        raise ValueError(f"{scale_name}: unexpected FP8 scale shape {s.shape} for weight {w.shape}")
    row_scale = np.repeat(np.arange(s.shape[0]), 128)[:out]
    col_scale = np.repeat(np.arange(s.shape[1]), 128)[:in_dim]
    return w * s[row_scale[:, None], col_scale[None, :]]


def dequant_fp4_e2m1(st, weight_name: str, scale_name: str) -> np.ndarray:
    packed = st.get_tensor(weight_name).cpu().numpy().view(np.uint8)
    scales = st.get_tensor(scale_name).to(torch.float32).cpu().numpy().astype(np.float32, copy=False)
    out, packed_cols = packed.shape
    in_dim = packed_cols * 2
    if scales.shape != (out, in_dim // 32):
        raise ValueError(f"{scale_name}: unexpected FP4 scale shape {scales.shape} for packed {packed.shape}")

    vals = np.empty((out, in_dim), dtype=np.float32)
    vals[:, 0::2] = FP4_E2M1[packed & 0x0F]
    vals[:, 1::2] = FP4_E2M1[packed >> 4]
    vals = vals.reshape(out, in_dim // 32, 32)
    vals *= scales[:, :, None]
    return vals.reshape(out, in_dim)


def q8_0(matrix: np.ndarray) -> np.ndarray:
    if matrix.dtype != np.float32:
        matrix = matrix.astype(np.float32, copy=False)
    return quantize(np.ascontiguousarray(matrix), GGMLQuantizationType.Q8_0)


def add_q8(writer: GGUFWriter, name: str, matrix: np.ndarray) -> None:
    writer.add_tensor(name, q8_0(matrix), raw_dtype=GGMLQuantizationType.Q8_0)


def add_f32(writer: GGUFWriter, name: str, array: np.ndarray) -> None:
    writer.add_tensor(name, np.ascontiguousarray(array.astype(np.float32, copy=False)))


def add_meta(writer: GGUFWriter) -> None:
    writer.add_key_value("general.architecture", "deepseek4_mtp_support", GGUFValueType.STRING)
    writer.add_name("DeepSeek V4 Flash MTP support upstream Q8_0")
    writer.add_key_value("deepseek4.nextn_predict_layers", 1, GGUFValueType.UINT32)
    writer.add_key_value("deepseek4.mtp_layer_count", 1, GGUFValueType.UINT32)
    writer.add_key_value("deepseek4.expert_count", N_EXPERT, GGUFValueType.UINT32)


def add_simple_tensors(writer: GGUFWriter, st) -> None:
    # HC tensors are stored in PyTorch row-major [out, in]; GGUF writes shape
    # reversed, which gives DS4 the expected [in, out] dimensions.
    for dst, src in [
        ("mtp.0.hc_head_base.weight", "mtp.0.hc_head_base"),
        ("mtp.0.hc_head_fn.weight", "mtp.0.hc_head_fn"),
        ("mtp.0.hc_head_scale.weight", "mtp.0.hc_head_scale"),
        ("mtp.0.hc_attn_base.weight", "mtp.0.hc_attn_base"),
        ("mtp.0.hc_ffn_base.weight", "mtp.0.hc_ffn_base"),
        ("mtp.0.hc_attn_fn.weight", "mtp.0.hc_attn_fn"),
        ("mtp.0.hc_attn_scale.weight", "mtp.0.hc_attn_scale"),
        ("mtp.0.hc_ffn_fn.weight", "mtp.0.hc_ffn_fn"),
        ("mtp.0.hc_ffn_scale.weight", "mtp.0.hc_ffn_scale"),
        ("mtp.0.attn_sinks.weight", "mtp.0.attn.attn_sink"),
        ("mtp.0.attn_q_a_norm.weight", "mtp.0.attn.q_norm.weight"),
        ("mtp.0.attn_kv_a_norm.weight", "mtp.0.attn.kv_norm.weight"),
        ("mtp.0.attn_norm.weight", "mtp.0.attn_norm.weight"),
        ("mtp.0.ffn_norm.weight", "mtp.0.ffn_norm.weight"),
        ("mtp.0.exp_probs_b.bias", "mtp.0.ffn.gate.bias"),
        ("mtp.0.enorm.weight", "mtp.0.enorm.weight"),
        ("mtp.0.hnorm.weight", "mtp.0.hnorm.weight"),
        ("mtp.0.norm.weight", "mtp.0.norm.weight"),
    ]:
        add_f32(writer, dst, to_f32(st, src))

    add_f32(writer, "mtp.0.ffn_gate_inp.weight", to_f32(st, "mtp.0.ffn.gate.weight"))


def add_fp8_q8_tensors(writer: GGUFWriter, st) -> None:
    for dst, weight, scale in [
        ("mtp.0.attn_q_a.weight", "mtp.0.attn.wq_a.weight", "mtp.0.attn.wq_a.scale"),
        ("mtp.0.attn_q_b.weight", "mtp.0.attn.wq_b.weight", "mtp.0.attn.wq_b.scale"),
        ("mtp.0.attn_output_a.weight", "mtp.0.attn.wo_a.weight", "mtp.0.attn.wo_a.scale"),
        ("mtp.0.attn_kv.weight", "mtp.0.attn.wkv.weight", "mtp.0.attn.wkv.scale"),
        ("mtp.0.attn_output_b.weight", "mtp.0.attn.wo_b.weight", "mtp.0.attn.wo_b.scale"),
        ("mtp.0.ffn_gate_shexp.weight", "mtp.0.ffn.shared_experts.w1.weight", "mtp.0.ffn.shared_experts.w1.scale"),
        ("mtp.0.ffn_up_shexp.weight", "mtp.0.ffn.shared_experts.w3.weight", "mtp.0.ffn.shared_experts.w3.scale"),
        ("mtp.0.ffn_down_shexp.weight", "mtp.0.ffn.shared_experts.w2.weight", "mtp.0.ffn.shared_experts.w2.scale"),
        ("mtp.0.e_proj.weight", "mtp.0.e_proj.weight", "mtp.0.e_proj.scale"),
        ("mtp.0.h_proj.weight", "mtp.0.h_proj.weight", "mtp.0.h_proj.scale"),
    ]:
        add_q8(writer, dst, dequant_fp8_block(st, weight, scale))


def build_expert_tensor(st, tmpdir: Path, out_name: str, src_w: str, in_dim: int, out_dim: int) -> np.memmap:
    row_bytes = (in_dim // 32) * 34
    path = tmpdir / f"{out_name}.q8_0.bytes"
    mm = np.memmap(path, mode="w+", dtype=np.uint8, shape=(N_EXPERT, out_dim, row_bytes))
    for expert in range(N_EXPERT):
        prefix = f"mtp.0.ffn.experts.{expert}.{src_w}"
        matrix = dequant_fp4_e2m1(st, f"{prefix}.weight", f"{prefix}.scale")
        if matrix.shape != (out_dim, in_dim):
            raise ValueError(f"{prefix}: expected {(out_dim, in_dim)}, got {matrix.shape}")
        mm[expert, :, :] = q8_0(matrix)
        if expert % 16 == 0:
            print(f"{out_name}: expert {expert}/{N_EXPERT}", flush=True)
    mm.flush()
    return mm


def add_expert_tensors(writer: GGUFWriter, st, tmpdir: Path) -> None:
    for dst, src_w, in_dim, out_dim in [
        ("mtp.0.ffn_gate_exps.weight", "w1", 4096, 2048),
        ("mtp.0.ffn_up_exps.weight", "w3", 4096, 2048),
        ("mtp.0.ffn_down_exps.weight", "w2", 2048, 4096),
    ]:
        mm = build_expert_tensor(st, tmpdir, dst.replace(".", "_"), src_w, in_dim, out_dim)
        writer.add_tensor(dst, mm, raw_dtype=GGMLQuantizationType.Q8_0)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--tmpdir", type=Path)
    parser.add_argument("--skip-experts", action="store_true")
    args = parser.parse_args()

    tmp_parent = args.tmpdir
    tmp_parent.mkdir(parents=True, exist_ok=True) if tmp_parent else None
    args.output.parent.mkdir(parents=True, exist_ok=True)

    with tempfile.TemporaryDirectory(prefix="ds4-mtp-convert-", dir=tmp_parent) as td:
        tmpdir = Path(td)
        writer = GGUFWriter(args.output, "deepseek4_mtp_support", use_temp_file=False)
        add_meta(writer)
        with safe_open(str(args.input), framework="pt", device="cpu") as st:
            add_simple_tensors(writer, st)
            add_fp8_q8_tensors(writer, st)
            if not args.skip_experts:
                add_expert_tensors(writer, st, tmpdir)
        writer.write_header_to_file()
        writer.write_kv_data_to_file()
        writer.write_tensors_to_file(progress=True)
        writer.close()
    print(args.output)


if __name__ == "__main__":
    main()
