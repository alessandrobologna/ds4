#!/usr/bin/env python3
import argparse
import json
import struct
from collections import Counter, defaultdict
from pathlib import Path


GGUF_VALUE_TYPES = {
    0: "uint8",
    1: "int8",
    2: "uint16",
    3: "int16",
    4: "uint32",
    5: "int32",
    6: "float32",
    7: "bool",
    8: "string",
    9: "array",
    10: "uint64",
    11: "int64",
    12: "float64",
}

GGML_TYPES = {
    0: "F32",
    1: "F16",
    2: "Q4_0",
    3: "Q4_1",
    6: "Q5_0",
    7: "Q5_1",
    8: "Q8_0",
    10: "Q2_K",
    11: "Q3_K",
    12: "Q4_K",
    13: "Q5_K",
    14: "Q6_K",
    15: "Q8_K",
    16: "IQ2_XXS",
    17: "IQ2_XS",
    18: "IQ3_XXS",
    19: "IQ1_S",
    20: "IQ4_NL",
    21: "IQ3_S",
    22: "IQ2_S",
    23: "IQ4_XS",
    24: "I8",
    25: "I16",
    26: "I32",
    27: "I64",
    28: "F64",
    29: "IQ1_M",
    30: "BF16",
    31: "Q4_0_4_4",
    32: "Q4_0_4_8",
    33: "Q4_0_8_8",
    34: "TQ1_0",
    35: "TQ2_0",
}


def read_u32(f):
    return struct.unpack("<I", f.read(4))[0]


def read_u64(f):
    return struct.unpack("<Q", f.read(8))[0]


def read_i64(f):
    return struct.unpack("<q", f.read(8))[0]


def read_string(f):
    size = read_u64(f)
    return f.read(size).decode("utf-8")


def skip_gguf_value(f, value_type):
    if value_type in (0, 1, 7):
        f.seek(1, 1)
    elif value_type in (2, 3):
        f.seek(2, 1)
    elif value_type in (4, 5, 6):
        f.seek(4, 1)
    elif value_type in (10, 11, 12):
        f.seek(8, 1)
    elif value_type == 8:
        f.seek(read_u64(f), 1)
    elif value_type == 9:
        item_type = read_u32(f)
        length = read_u64(f)
        for _ in range(length):
            skip_gguf_value(f, item_type)
    else:
        raise ValueError(f"unsupported GGUF metadata value type {value_type}")


def read_gguf(path):
    tensors = []
    metadata_keys = []
    with open(path, "rb") as f:
        magic = f.read(4)
        if magic != b"GGUF":
            raise ValueError(f"{path} is not a GGUF file")
        version = read_u32(f)
        tensor_count = read_u64(f)
        kv_count = read_u64(f)
        for _ in range(kv_count):
            key = read_string(f)
            value_type = read_u32(f)
            metadata_keys.append((key, GGUF_VALUE_TYPES.get(value_type, str(value_type))))
            skip_gguf_value(f, value_type)
        for _ in range(tensor_count):
            name = read_string(f)
            n_dims = read_u32(f)
            dims = [read_u64(f) for _ in range(n_dims)]
            tensor_type = read_u32(f)
            offset = read_u64(f)
            tensors.append(
                {
                    "name": name,
                    "shape": dims,
                    "dtype": GGML_TYPES.get(tensor_type, str(tensor_type)),
                    "offset": offset,
                }
            )
    return {"format": "gguf", "version": version, "metadata": metadata_keys, "tensors": tensors}


def read_safetensors(path):
    with open(path, "rb") as f:
        header_len = struct.unpack("<Q", f.read(8))[0]
        header = json.loads(f.read(header_len))
    tensors = []
    for name, info in header.items():
        if name == "__metadata__":
            continue
        tensors.append(
            {
                "name": name,
                "shape": info.get("shape", []),
                "dtype": info.get("dtype", "?"),
                "offset": info.get("data_offsets", [0])[0],
            }
        )
    return {"format": "safetensors", "metadata": list(header.get("__metadata__", {}).items()), "tensors": tensors}


def read_file(path):
    with open(path, "rb") as f:
        prefix = f.read(8)
    if prefix[:4] == b"GGUF":
        return read_gguf(path)
    return read_safetensors(path)


def prefix_for(name):
    parts = name.split(".")
    if len(parts) >= 4 and parts[0] == "mtp" and parts[2] == "ffn":
        return ".".join(parts[:4])
    if len(parts) >= 3:
        return ".".join(parts[:3])
    return name


def summarize(path, data, sample_count):
    tensors = data["tensors"]
    dtype_counts = Counter(t["dtype"] for t in tensors)
    prefix_counts = Counter(prefix_for(t["name"]) for t in tensors)
    print(f"path: {path}")
    print(f"format: {data['format']}")
    if data["format"] == "gguf":
        print(f"version: {data['version']}")
        print(f"metadata_keys: {len(data['metadata'])}")
    print(f"tensors: {len(tensors)}")
    print("dtype_counts:")
    for dtype, count in sorted(dtype_counts.items()):
        print(f"  {dtype}: {count}")
    print("top_prefixes:")
    for prefix, count in prefix_counts.most_common(20):
        print(f"  {prefix}: {count}")
    print("samples:")
    for t in tensors[:sample_count]:
        print(f"  {t['name']} dtype={t['dtype']} shape={t['shape']}")


def compare(left_path, right_path, sample_count):
    left = read_file(left_path)
    right = read_file(right_path)
    left_by_name = {t["name"]: t for t in left["tensors"]}
    right_by_name = {t["name"]: t for t in right["tensors"]}
    left_names = set(left_by_name)
    right_names = set(right_by_name)
    common = sorted(left_names & right_names)
    only_left = sorted(left_names - right_names)
    only_right = sorted(right_names - left_names)
    shape_mismatch = [
        name for name in common
        if list(left_by_name[name]["shape"]) != list(right_by_name[name]["shape"])
    ]
    print(f"left: {left_path} ({left['format']}, tensors={len(left_names)})")
    print(f"right: {right_path} ({right['format']}, tensors={len(right_names)})")
    print(f"common: {len(common)}")
    print(f"only_left: {len(only_left)}")
    for name in only_left[:sample_count]:
        t = left_by_name[name]
        print(f"  L {name} dtype={t['dtype']} shape={t['shape']}")
    print(f"only_right: {len(only_right)}")
    for name in only_right[:sample_count]:
        t = right_by_name[name]
        print(f"  R {name} dtype={t['dtype']} shape={t['shape']}")
    print(f"shape_mismatch: {len(shape_mismatch)}")
    for name in shape_mismatch[:sample_count]:
        l = left_by_name[name]
        r = right_by_name[name]
        print(f"  {name}: left={l['shape']} right={r['shape']}")


def main():
    parser = argparse.ArgumentParser(description="Inspect DS4 MTP GGUF/safetensors headers.")
    parser.add_argument("paths", nargs="+")
    parser.add_argument("--compare", action="store_true", help="compare exactly two files by tensor name")
    parser.add_argument("--samples", type=int, default=24)
    args = parser.parse_args()

    if args.compare:
        if len(args.paths) != 2:
            parser.error("--compare requires exactly two paths")
        compare(Path(args.paths[0]), Path(args.paths[1]), args.samples)
        return

    for i, path in enumerate(args.paths):
        if i:
            print()
        summarize(Path(path), read_file(Path(path)), args.samples)


if __name__ == "__main__":
    main()
