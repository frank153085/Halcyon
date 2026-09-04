#!/usr/bin/env python3
"""Convert Halcyon's legacy Suzanne OBJ into the checked-in glTF scene."""

from __future__ import annotations

import argparse
import json
import struct
from pathlib import Path


def parse_index(value: str, count: int) -> int:
    index = int(value)
    return index - 1 if index > 0 else count + index


def convert(source: Path, output: Path) -> None:
    positions: list[tuple[float, float, float]] = []
    normals: list[tuple[float, float, float]] = []
    texcoords: list[tuple[float, float]] = []
    vertices: list[tuple[float, ...]] = []
    indices: list[int] = []
    vertex_map: dict[tuple[int, int, int], int] = {}

    for line in source.read_text(encoding="utf-8").splitlines():
        fields = line.split()
        if not fields or fields[0].startswith("#"):
            continue
        if fields[0] == "v":
            positions.append(tuple(map(float, fields[1:4])))
        elif fields[0] == "vn":
            normals.append(tuple(map(float, fields[1:4])))
        elif fields[0] == "vt":
            texcoords.append(tuple(map(float, fields[1:3])))
        elif fields[0] == "f":
            face: list[int] = []
            for token in fields[1:]:
                parts = token.split("/")
                key = (
                    parse_index(parts[0], len(positions)),
                    parse_index(parts[1], len(texcoords)) if len(parts) > 1 and parts[1] else -1,
                    parse_index(parts[2], len(normals)) if len(parts) > 2 and parts[2] else -1,
                )
                if key not in vertex_map:
                    position = positions[key[0]]
                    normal = normals[key[2]] if key[2] >= 0 else (0.0, 0.0, 1.0)
                    texcoord = texcoords[key[1]] if key[1] >= 0 else (0.0, 0.0)
                    vertex_map[key] = len(vertices)
                    vertices.append(position + normal + texcoord)
                face.append(vertex_map[key])
            for index in range(2, len(face)):
                indices.extend((face[0], face[index - 1], face[index]))

    if not vertices or not indices:
        raise ValueError(f"OBJ contains no renderable triangles: {source}")

    output.mkdir(parents=True, exist_ok=True)
    binary_path = output / "monkey.bin"
    gltf_path = output / "monkey.gltf"
    vertex_bytes = b"".join(struct.pack("<8f", *vertex) for vertex in vertices)
    index_bytes = b"".join(struct.pack("<I", index) for index in indices)
    binary_path.write_bytes(vertex_bytes + index_bytes)

    minimum = [min(vertex[axis] for vertex in vertices) for axis in range(3)]
    maximum = [max(vertex[axis] for vertex in vertices) for axis in range(3)]
    document = {
        "asset": {"version": "2.0", "generator": "Halcyon convert_monkey_to_gltf.py"},
        "scene": 0,
        "scenes": [{"name": "MonkeyScene", "nodes": [0]}],
        "nodes": [{"name": "Suzanne", "mesh": 0}],
        "meshes": [{
            "name": "Suzanne",
            "primitives": [{
                "attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2},
                "indices": 3,
                "material": 0,
                "mode": 4,
            }],
        }],
        "materials": [{
            "name": "MonkeyPbr",
            "pbrMetallicRoughness": {
                "baseColorTexture": {"index": 0},
                "metallicRoughnessTexture": {"index": 2},
                "metallicFactor": 1.0,
                "roughnessFactor": 1.0,
            },
            "normalTexture": {"index": 1},
            "occlusionTexture": {"index": 2},
        }],
        "textures": [{"source": 0}, {"source": 1}, {"source": 2}],
        "images": [
            {"name": "BaseColor", "uri": "color.png"},
            {"name": "Normal", "uri": "normal.png"},
            {"name": "MetallicRoughnessOcclusion", "uri": "metallic_roughness.png"},
        ],
        "samplers": [{}],
        "buffers": [{"uri": "monkey.bin", "byteLength": len(vertex_bytes) + len(index_bytes)}],
        "bufferViews": [
            {
                "buffer": 0,
                "byteOffset": 0,
                "byteLength": len(vertex_bytes),
                "byteStride": 32,
                "target": 34962,
            },
            {
                "buffer": 0,
                "byteOffset": len(vertex_bytes),
                "byteLength": len(index_bytes),
                "target": 34963,
            },
        ],
        "accessors": [
            {
                "bufferView": 0,
                "byteOffset": 0,
                "componentType": 5126,
                "count": len(vertices),
                "type": "VEC3",
                "min": minimum,
                "max": maximum,
            },
            {
                "bufferView": 0,
                "byteOffset": 12,
                "componentType": 5126,
                "count": len(vertices),
                "type": "VEC3",
            },
            {
                "bufferView": 0,
                "byteOffset": 24,
                "componentType": 5126,
                "count": len(vertices),
                "type": "VEC2",
            },
            {
                "bufferView": 1,
                "byteOffset": 0,
                "componentType": 5125,
                "count": len(indices),
                "type": "SCALAR",
                "min": [min(indices)],
                "max": [max(indices)],
            },
        ],
    }
    gltf_path.write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    arguments = parser.parse_args()
    convert(arguments.source.resolve(), arguments.output.resolve())


if __name__ == "__main__":
    main()
