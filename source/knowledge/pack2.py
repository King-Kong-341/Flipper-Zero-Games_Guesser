"""Writes the v2 guesser.dat (SPEC section 5 plus the v2 additions below).

v2 keeps the v1 header layout and every v1 block, and adds three blocks in
the old reserved words of the header:

  0x9C  u32  row_off      entity-major copy of the matrix (N rows of
                          ceil(Q/2) bytes, even question = low nibble).
                          Lets the device score EVERY question against the
                          current front-runners with a few small reads.
  0xA0  u32  row_stride   bytes per entity row = (Q + 1) // 2
  0xA4  u32  catmean_off  u8[Q][4]: mean P(yes) per category, 0..255.
                          Replaces the per-turn category-mean scan that the
                          learned-word overlay needed.

Flags: bit 3 = has rows, bit 4 = has category means.
"""

from __future__ import annotations

import struct
import zlib
from pathlib import Path

MAGIC = b"F20Q"
FORMAT_VERSION = 2
HEADER_SIZE = 0xC0

FLAG_HAS_PRIORS = 1 << 0
FLAG_HAS_CATEGORIES = 1 << 1
FLAG_HAS_FAMILIES = 1 << 2
FLAG_HAS_ROWS = 1 << 3
FLAG_HAS_CATMEAN = 1 << 4
FLAG_HAS_DOMAINS = 1 << 5  # 0xA8 u8[N] entity domain, 0xAC u16[Q] question domain mask

PYES_LUT = [
    0.02, 0.05, 0.10, 0.17, 0.25, 0.33, 0.42, 0.48,
    0.52, 0.58, 0.67, 0.75, 0.83, 0.90, 0.95, 0.98,
]

MAX_ENTITY_NAME = 22
MAX_QUESTION_TEXT = 72


class PackError(Exception):
    pass


def quantise(p: float) -> int:
    best_i, best_d = 0, 9.9
    for i, v in enumerate(PYES_LUT):
        d = abs(v - p)
        if d < best_d - 1e-12:
            best_i, best_d = i, d
    return best_i


def _ascii(s: str, limit: int, what: str) -> bytes:
    if not s:
        raise PackError(f"empty {what}")
    if len(s) > limit:
        raise PackError(f"{what} too long ({len(s)} > {limit}): {s!r}")
    raw = s.encode("ascii")
    for b in raw:
        if b < 0x20 or b > 0x7E:
            raise PackError(f"{what} has a non-printable byte: {s!r}")
    return raw


def _text_block(strings, limit, what):
    text = bytearray()
    index = bytearray()
    for s in strings:
        index += struct.pack("<I", len(text))
        text += _ascii(s, limit, what)
    index += struct.pack("<I", len(text))
    return bytes(index), bytes(text)


def write_dataset(path, names, cats, priors, qtexts, qfamilies, nib, qrank,
                  dataset_id="20q-v2", build_epoch=0, domains=None, qmasks=None):
    """nib[i][j] = LUT index for entity i, question j (entity-major lists)."""
    n, q = len(names), len(qtexts)
    if n == 0 or q == 0:
        raise PackError("empty dataset")
    if n > 16384 or q > 1024:
        raise PackError(f"exceeds device guard rails: N={n} Q={q}")
    if names != sorted(names):
        raise PackError("entity names must be sorted")
    if len(set(names)) != n:
        raise PackError("duplicate entity names")
    for nm in names:
        if nm != nm.lower():
            raise PackError(f"entity names must be lower-case: {nm!r}")

    ent_index, ent_text = _text_block(names, MAX_ENTITY_NAME, "entity name")
    qst_index, qst_text = _text_block(qtexts, MAX_QUESTION_TEXT, "question text")

    maxprior = max(max(priors), 1e-9)
    prior_blk = b"".join(
        struct.pack("<H", max(1, min(65535, round(p / maxprior * 65535.0)))) for p in priors
    )
    cat_blk = bytes(cats)
    if sorted(qrank) != list(range(q)):
        raise PackError("qrank must be a permutation")
    qrank_blk = b"".join(struct.pack("<H", j) for j in qrank)
    family_blk = bytes(qfamilies)

    stride = (n + 1) // 2
    matrix = bytearray(stride * q)
    for i in range(n):
        row = nib[i]
        half, odd = i >> 1, i & 1
        for j in range(q):
            off = j * stride + half
            if odd:
                matrix[off] |= row[j] << 4
            else:
                matrix[off] |= row[j]

    row_stride = (q + 1) // 2
    rows = bytearray(row_stride * n)
    for i in range(n):
        row = nib[i]
        base = i * row_stride
        for j in range(q):
            off = base + (j >> 1)
            if j & 1:
                rows[off] |= row[j] << 4
            else:
                rows[off] |= row[j]

    catmean = bytearray(q * 4)
    for j in range(q):
        for c in range(4):
            vals = [PYES_LUT[nib[i][j]] for i in range(n) if cats[i] == c]
            m = sum(vals) / len(vals) if vals else 0.5
            catmean[j * 4 + c] = max(0, min(255, round(m * 255.0)))

    blocks = []
    cursor = HEADER_SIZE
    offsets = {}
    for key, blk in (
        ("ent_index", ent_index), ("ent_text", ent_text),
        ("qst_index", qst_index), ("qst_text", qst_text),
        ("prior", prior_blk), ("category", cat_blk),
        ("qrank", qrank_blk), ("family", family_blk),
        ("catmean", bytes(catmean)),
        ("domain", bytes(domains or [15] * n)),
        ("qdomain", b"".join(struct.pack("<H", m) for m in (qmasks or [0xFFFF] * q))),
        ("matrix", bytes(matrix)), ("rows", bytes(rows)),
    ):
        pad = (-cursor) % 4
        if pad:
            blocks.append(b"\0" * pad)
            cursor += pad
        offsets[key] = cursor
        blocks.append(blk)
        cursor += len(blk)

    body = b"".join(blocks)
    crc = zlib.crc32(body) & 0xFFFFFFFF
    did = _ascii(dataset_id, 32, "dataset_id")
    header = bytearray(HEADER_SIZE)
    flags = (FLAG_HAS_PRIORS | FLAG_HAS_CATEGORIES | FLAG_HAS_FAMILIES
             | FLAG_HAS_ROWS | FLAG_HAS_CATMEAN | FLAG_HAS_DOMAINS)
    struct.pack_into(
        "<4sHHIIIIIIIIII", header, 0,
        MAGIC, FORMAT_VERSION, flags, n, q,
        offsets["matrix"], offsets["ent_index"], offsets["ent_text"],
        offsets["qst_index"], offsets["qst_text"],
        offsets["prior"], offsets["category"], offsets["qrank"],
    )
    struct.pack_into("<16f", header, 0x30, *PYES_LUT)
    struct.pack_into("<II", header, 0x70, crc, build_epoch)
    header[0x78:0x78 + len(did)] = did
    struct.pack_into("<IIIIII", header, 0x98,
                     offsets["family"], offsets["rows"], row_stride, offsets["catmean"],
                     offsets["domain"], offsets["qdomain"])

    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(bytes(header) + body)
    return {"path": str(path), "entities": n, "questions": q,
            "bytes": HEADER_SIZE + len(body), "crc32": crc}
