#!/usr/bin/env python3
"""Compiles the knowledge base in knowledge/kb/ into files/guesser.dat.

    python knowledge/build_kb.py                  # from source/
    python knowledge/build_kb.py --confusable 2   # list near-identical things


Knowledge base format (plain text, one record per line, '#' starts a comment):

  patch NAME | ATTRS     adds/overrides answers of an entity defined elsewhere
  drop NAME              removes an entity (duplicates, synonyms)

  questions.txt
      key | family | fallback | question text
      fallback = 4 value letters for the categories Animal, Vegetable,
      Mineral, Other: what an entity answers when nothing sets the key.

  every other *.txt
      class NAME | PARENTS | ATTRS          a reusable bundle of answers
      NAME | PARENTS | PRIOR | ATTRS        a thing the player can think of

  PARENTS  comma-separated class names, applied left to right (later wins),
           then the line's own ATTRS win over all of them.
  PRIOR    1..100, how likely a player is to pick it.
  ATTRS    whitespace-separated tokens:
             key      yes            -key     no
             key?     probably       -key?    probably not
             key~     sometimes / it depends
             key=0.7  exact P(yes)
             sz=0..10 size class (derives the size questions)
             col=a,b/c  main colours / secondary colours (or col=any)
             cat=A|V|M|O  category override

Size classes: 0 no body, 1 < 1 cm, 2 1-5 cm, 3 hand, 4 shoebox,
5 up to 1 m, 6 person, 7 car, 8 house / bus, 9 big building / ship,
10 mountain / country / planet.
"""

from __future__ import annotations

import argparse
import os
import re
import sys
import time
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from pack2 import PYES_LUT, quantise, write_dataset  # noqa: E402

ROOT = Path(__file__).resolve().parent
KB = ROOT / "kb"
SOURCE = ROOT.parent

FAMILIES = (
    "taxonomy", "animal", "size", "material", "colour", "location",
    "function", "food", "sensory", "motion", "social", "misc",
    "person", "place", "tech", "nature",
)

VALUES = {
    "Y": 0.95, "P": 0.75, "S": 0.50, "U": 0.25, "N": 0.05,
    "y": 0.90, "p": 0.67, "s": 0.50, "u": 0.33, "n": 0.10,
    "X": 0.98, "Z": 0.02,
}
CATS = {"A": 0, "V": 1, "M": 2, "O": 3}

# Topic domains: which kind of thing an entity is, decided by its root class.
# A question is only "on topic" for the domains where its answers vary; the
# device then avoids asking e.g. "Is it male?" while the suspects are chairs.
DOMAINS = ("animal", "plant", "food", "object", "building", "material", "job",
           "famous", "character", "place", "media", "other")
DOMAIN_ROOTS = (  # first match wins: specific classes before general ones
    ("animal_base", 0), ("dino_c", 0),
    ("plant_base", 1), ("fungus_c", 1),
    ("food_base", 2),
    ("building_c", 4), ("room_c", 4), ("buildpart_c", 4),
    ("material_c", 5),
    ("job_c", 6),
    ("famous_c", 7),
    ("fict_c", 8), ("myth_c", 8),
    ("person_c", 6),
    ("place_c", 9),
    ("brand_base", 10), ("media_base", 10), ("club_c", 10),
    ("object_base", 3),
)

COLOURS = ("red", "green", "blue", "yellow", "orange", "brown", "black",
           "white", "grey", "pink", "purple")
COLOUR_ALIAS = {"gold": "yellow", "golden": "yellow", "silver": "grey", "gray": "grey"}

# size class -> P(yes) for each derived size question
SIZE_RULES = {
    "s_coin":    {1: "Y", 2: "S"},
    "s_hand":    {1: "Y", 2: "Y", 3: "S"},
    "s_hold":    {1: "Y", 2: "Y", 3: "Y", 4: "P", 5: "U"},
    "s_pocket":  {1: "Y", 2: "Y", 3: "P", 4: "U"},
    "s_shoebox": {4: "S", 5: "Y", 6: "Y", 7: "Y", 8: "Y", 9: "Y", 10: "Y"},
    "s_person":  {6: "U", 7: "Y", 8: "Y", 9: "Y", 10: "Y"},
    "s_car":     {7: "S", 8: "Y", 9: "Y", 10: "Y"},
    "s_house":   {8: "U", 9: "Y", 10: "Y"},
    "s_door":    {1: "Y", 2: "Y", 3: "Y", 4: "Y", 5: "Y", 6: "P", 7: "U"},
    "heavy":     {4: "U", 5: "S", 6: "P", 7: "Y", 8: "Y", 9: "Y", 10: "Y"},
    "lift":      {1: "Y", 2: "Y", 3: "Y", 4: "Y", 5: "P", 6: "U"},
}

TOKEN = re.compile(r"^(-?)([a-z0-9_]+)([?~]?)$")


class KbError(Exception):
    pass


UNKNOWN = {}  # unknown attribute key -> where it was used
EXPLICIT = []  # (category, vector, explicitly set keys) per entity, for --fallbacks


def parse_questions(path):
    qs = []
    seen = set()
    for ln, line in enumerate(path.read_text().splitlines(), 1):
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        parts = [p.strip() for p in line.split("|")]
        if len(parts) != 4:
            raise KbError(f"{path.name}:{ln}: want 4 fields: {line}")
        key, fam, fb, text = parts
        if key in seen:
            raise KbError(f"{path.name}:{ln}: duplicate key {key}")
        if fam not in FAMILIES:
            raise KbError(f"{path.name}:{ln}: unknown family {fam}")
        if len(fb) != 4 or any(c not in VALUES for c in fb):
            raise KbError(f"{path.name}:{ln}: bad fallback {fb}")
        if len(text) > 72 or not text.isascii():
            raise KbError(f"{path.name}:{ln}: bad text ({len(text)} chars)")
        seen.add(key)
        qs.append({"key": key, "family": FAMILIES.index(fam),
                   "fb": [VALUES[c] for c in fb], "text": text})
    return qs


def parse_attrs(s, where, qkeys):
    out = {}
    for tok in s.split():
        if tok.startswith("sz="):
            out["@sz"] = int(tok[3:])
            if not 0 <= out["@sz"] <= 10:
                raise KbError(f"{where}: bad size {tok}")
            continue
        if tok.startswith("col="):
            out["@col"] = tok[4:]
            continue
        if tok.startswith("cat="):
            if tok[4:] not in CATS:
                raise KbError(f"{where}: bad category {tok}")
            out["@cat"] = CATS[tok[4:]]
            continue
        if "=" in tok:
            k, v = tok.split("=", 1)
            if k not in qkeys:
                UNKNOWN.setdefault(k, []).append(where)
                continue
            out[k] = float(v) if v not in VALUES else VALUES[v]
            continue
        m = TOKEN.match(tok)
        if not m:
            raise KbError(f"{where}: bad token {tok!r}")
        neg, key, mod = m.groups()
        if key not in qkeys:
            if not neg and not mod:  # may be a class used as a mixin
                out.setdefault("@mix", []).append(key)
            UNKNOWN.setdefault(key, []).append(where)
            continue
        if mod == "~":
            v = "S"
        elif neg:
            v = "U" if mod == "?" else "N"
        else:
            v = "P" if mod == "?" else "Y"
        out[key] = VALUES[v]
    return out


def parse_kb(qkeys):
    classes, ents = {}, []
    patches, drops = [], set()
    files = sorted(p for p in KB.glob("*.txt") if p.name != "questions.txt")
    for path in files:
        for ln, line in enumerate(path.read_text().splitlines(), 1):
            line = line.split("#", 1)[0].strip()
            if not line:
                continue
            where = f"{path.name}:{ln}"
            parts = [p.strip() for p in line.split("|")]
            if parts[0].startswith("patch "):
                if len(parts) != 2:
                    raise KbError(f"{where}: patch wants 2 fields")
                patches.append((parts[0][6:].strip(), parse_attrs(parts[1], where, qkeys), where))
                continue
            if parts[0].startswith("drop "):
                drops.add(parts[0][5:].strip())
                continue
            if parts[0].startswith("class "):
                if len(parts) != 3:
                    raise KbError(f"{where}: class wants 3 fields")
                name = parts[0][6:].strip()
                if name in classes:
                    raise KbError(f"{where}: duplicate class {name}")
                parents = [p.strip() for p in parts[1].split(",") if p.strip()]
                classes[name] = {"parents": parents, "attrs": parse_attrs(parts[2], where, qkeys),
                                 "where": where}
            else:
                if len(parts) == 3:
                    parts.append("")
                if len(parts) != 4:
                    raise KbError(f"{where}: entity wants 4 fields: {line}")
                name = parts[0]
                if name != name.lower() or not name.isascii() or len(name) > 22:
                    raise KbError(f"{where}: bad entity name {name!r}")
                parents = [p.strip() for p in parts[1].split(",") if p.strip()]
                ents.append({"name": name, "parents": parents, "prior": float(parts[2]),
                             "attrs": parse_attrs(parts[3], where, qkeys), "where": where,
                             "file": path.stem})
    by_name = {e["name"]: e for e in ents}
    missing = []
    for name, attrs, where in patches:
        if name in drops:
            continue  # patched earlier, dropped later: fine
        if name not in by_name:
            missing.append(f"{name} ({where})")
            continue
        mix = attrs.pop("@mix", [])
        by_name[name]["attrs"].update(attrs)
        if mix:
            by_name[name]["attrs"].setdefault("@mix", []).extend(mix)
    if missing:
        raise KbError("patch for unknown entity: " + ", ".join(missing))
    for name in drops:
        if name not in by_name:
            raise KbError(f"drop of unknown entity {name!r}")
    ents = [e for e in ents if e["name"] not in drops]
    return classes, ents


def resolve(name, classes, memo, stack=()):
    if name in memo:
        return memo[name]
    if name not in classes:
        raise KbError(f"unknown class {name} (via {' > '.join(stack)})")
    if name in stack:
        raise KbError(f"class cycle: {' > '.join(stack + (name,))}")
    c = classes[name]
    out = {}
    for p in c["parents"] + c["attrs"].get("@mix", []):
        out.update(resolve(p, classes, memo, stack + (name,)))
    out.update({k: v for k, v in c["attrs"].items() if k != "@mix"})
    memo[name] = out
    return out


def apply_colours(attrs, out):
    spec = attrs.get("@col")
    if spec is None:
        return
    if "any" in spec.replace("/", ",").split(","):
        vals = {f"c_{c}": VALUES["U"] for c in COLOURS}
        vals["multicol"] = VALUES["S"]
        vals["anycol"] = VALUES["Y"]
    else:
        main, _, sec = spec.partition("/")
        mains = [COLOUR_ALIAS.get(c, c) for c in main.split(",") if c]
        secs = [COLOUR_ALIAS.get(c, c) for c in sec.split(",") if c]
        for c in mains + secs:
            if c not in COLOURS:
                raise KbError(f"unknown colour {c}")
        # Colour answers are softer than most: people disagree about whether a
        # stapler is "black" or "grey", so neither yes nor no is ever certain.
        vals = {}
        for c in COLOURS:
            vals[f"c_{c}"] = VALUES["y"] if c in mains else VALUES["p"] if c in secs else VALUES["n"]
        k = len(mains) + len(secs)
        vals["multicol"] = VALUES["y"] if k >= 3 else VALUES["p"] if k == 2 else VALUES["n"]
        vals["anycol"] = VALUES["N"]
    for k, v in vals.items():
        out.setdefault(k, v)


def ancestors(names, classes, seen=None):
    seen = set() if seen is None else seen
    for n in names:
        if n in seen or n not in classes:
            continue
        seen.add(n)
        c = classes[n]
        ancestors(c["parents"] + c["attrs"].get("@mix", []), classes, seen)
    return seen


def entity_domain(e, classes):
    anc = ancestors(e["parents"] + e["attrs"].get("@mix", []), classes)
    for root, d in DOMAIN_ROOTS:
        if root in anc:
            return d
    return 11


def question_domains(P, doms):
    """Bitmask of domains in which question j is worth asking."""
    masks = []
    n_dom = len(DOMAINS)
    groups = [np.where(doms == d)[0] for d in range(n_dom)]
    for j in range(P.shape[1]):
        col = P[:, j]
        mask = 0
        for d, idx in enumerate(groups):
            if len(idx) < 15:
                mask |= 1 << d  # too few to judge: never block
                continue
            v = col[idx]
            # on topic where the answers vary, or where they are typically
            # "yes" ("Is it alive?" belongs to animals and plants); never
            # where everything simply says "no" ("Is it male?" for chairs)
            varies = (v >= 0.65).mean() >= 0.02 and (v <= 0.35).mean() >= 0.02
            if varies or v.mean() >= 0.65:
                mask |= 1 << d
        if mask == 0:
            mask = (1 << n_dom) - 1
        masks.append(mask)
    return masks


def entity_vector(e, classes, memo, qindex, qs):
    attrs = {}
    for p in e["parents"] + e["attrs"].get("@mix", []):
        attrs.update(resolve(p, classes, memo, (e["name"],)))
    attrs.update({k: v for k, v in e["attrs"].items() if k != "@mix"})
    cat = attrs.get("@cat")
    if cat is None:
        raise KbError(f"{e['where']}: {e['name']} has no category (cat=)")
    explicit = {k: v for k, v in attrs.items() if not k.startswith("@")}
    sz = attrs.get("@sz")
    if sz is not None:
        for key, rule in SIZE_RULES.items():
            if key in qindex:
                explicit.setdefault(key, VALUES[rule.get(sz, "N")])
    apply_colours(attrs, explicit)
    vec = [qs[j]["fb"][cat] for j in range(len(qs))]
    nset = 0
    for k, v in explicit.items():
        if k in qindex:
            vec[qindex[k]] = v
            nset += 1
    return cat, vec, nset, sz, set(k for k in explicit if k in qindex)


def adjust_fallbacks(rows, qs):
    """Where most entities of a category answer a question explicitly and
    mostly 'no', an entity that says nothing is probably a 'no' as well:
    lower its fallback towards the explicit mean. Never raises a fallback --
    people write down what is true, so explicit answers lean towards yes."""
    lowered = 0
    for j, qq in enumerate(qs):
        for c in range(4):
            idx = [k for k, x in enumerate(EXPLICIT) if x[0] == c]
            if len(idx) < 20:
                continue
            ex = [EXPLICIT[k][1][j] for k in idx if qq["key"] in EXPLICIT[k][2]]
            if len(ex) < 0.4 * len(idx):
                continue
            m = sum(ex) / len(ex)
            new = max(0.05, min(qq["fb"][c], m + 0.05))
            if new >= qq["fb"][c] - 1e-9:
                continue
            for k in idx:
                if qq["key"] not in EXPLICIT[k][2]:
                    rows[k][3][j] = new
                    lowered += 1
    return lowered


def rank_questions(P, prior):
    b = prior / prior.sum()
    y = b[:, None] * P
    s_yes = y.sum(0)
    a = (y * y).sum(0)
    no = b[:, None] - y
    bb = (no * no).sum(0)
    s_no = 1.0 - s_yes
    purity = a / np.maximum(s_yes, 1e-6) + bb / np.maximum(s_no, 1e-6)
    return [int(j) for j in np.argsort(-purity, kind="stable")]


def confusable(P, names, limit):
    hi = (P >= 0.70).astype(np.float32)
    lo = (P <= 0.30).astype(np.float32)
    D = hi @ lo.T
    D = D + D.T
    np.fill_diagonal(D, 999)
    best = D.min(1)
    arg = D.argmin(1)
    pairs = {}
    for i in np.argsort(best):
        if best[i] >= limit:
            break
        j = int(arg[i])
        key = tuple(sorted((i, j)))
        pairs[key] = int(best[i])
    return [(names[a], names[b], d) for (a, b), d in sorted(pairs.items(), key=lambda t: t[1])]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-o", "--out", default=str(SOURCE / "files" / "guesser.dat"))
    ap.add_argument("--confusable", type=int, default=0,
                    help="list entity pairs with fewer than N strong differences")
    ap.add_argument("--entity", default=None, help="print one entity's answers")
    ap.add_argument("--question", default=None, help="print one question's yes-list")
    ap.add_argument("--epoch", type=int, default=None)
    ap.add_argument("--no-write", action="store_true")
    ap.add_argument("--fallbacks", action="store_true",
                    help="list fallbacks that disagree with the explicit answers")
    args = ap.parse_args()

    qs = parse_questions(KB / "questions.txt")
    qindex = {q["key"]: j for j, q in enumerate(qs)}
    classes, ents = parse_kb(set(qindex))
    for k in [k for k in UNKNOWN if k in classes]:
        del UNKNOWN[k]  # a class name used as a mixin token
    if UNKNOWN:
        for k, w in sorted(UNKNOWN.items()):
            print(f"ERROR unknown key {k!r}: {', '.join(w[:4])}{' ...' if len(w) > 4 else ''}")
        raise KbError(f"{len(UNKNOWN)} unknown keys")
    for name, c in classes.items():
        for p in c["parents"]:
            if p not in classes:
                raise KbError(f"{c['where']}: unknown parent {p}")
    names = [e["name"] for e in ents]
    dupes = sorted({n for n in names if names.count(n) > 1})
    if dupes:
        where = {n: [e["where"] for e in ents if e["name"] == n] for n in dupes}
        raise KbError(f"duplicate entities: {where}")

    memo = {}
    rows = []
    for e in ents:
        for p in e["parents"]:
            if p not in classes:
                raise KbError(f"{e['where']}: unknown class {p}")
        cat, vec, nset, sz, keys = entity_vector(e, classes, memo, qindex, qs)
        EXPLICIT.append((cat, vec, keys))
        if sz is None:
            raise KbError(f"{e['where']}: {e['name']} has no size (sz=)")
        rows.append((e["name"], cat, e["prior"], vec, nset, e["file"], entity_domain(e, classes)))
    lowered = adjust_fallbacks(rows, qs)
    rows.sort(key=lambda r: r[0])
    names = [r[0] for r in rows]
    cats = [r[1] for r in rows]
    priors = np.array([r[2] for r in rows], dtype=np.float64)
    P = np.array([r[3] for r in rows], dtype=np.float64)
    doms = np.array([r[6] for r in rows])
    n, q = P.shape
    qmasks = question_domains(P, doms)

    if args.entity:
        i = names.index(args.entity)
        for j in range(q):
            print(f"  {P[i, j]:.2f}  {qs[j]['key']:<12} {qs[j]['text']}")
        return 0
    if args.question:
        j = qindex[args.question]
        print("on topic for: " + ", ".join(d for k, d in enumerate(DOMAINS) if qmasks[j] >> k & 1))
        print(", ".join(names[i] for i in range(n) if P[i, j] >= 0.7))
        return 0

    print(f"entities {n}  questions {q}  classes {len(classes)}")
    for c, label in enumerate(("animal", "vegetable", "mineral", "other")):
        print(f"  {label:<10} {cats.count(c)}")
    print("  domains: " + ", ".join(f"{d} {int((doms == k).sum())}" for k, d in enumerate(DOMAINS)))
    files = {}
    for r in rows:
        files[r[5]] = files.get(r[5], 0) + 1
    print("  files: " + ", ".join(f"{k} {v}" for k, v in sorted(files.items())))
    nsets = np.array([r[4] for r in rows])
    print(f"  explicit answers per entity: mean {nsets.mean():.0f}, min {nsets.min()} "
          f"({names[int(nsets.argmin())]})")
    strong = ((P >= 0.7) | (P <= 0.3)).mean()
    print(f"  confident cells {100 * strong:.1f}%  (fallbacks lowered in {lowered} cells)")
    yes = (P >= 0.7).sum(0)
    dead = [qs[j]["key"] for j in range(q) if yes[j] == 0]
    if dead:
        print(f"  WARNING questions nobody answers yes: {dead}")

    if args.fallbacks:
        print("\nkey          cat  fallback  explicit-mean  explicit/total")
        for j, qq in enumerate(qs):
            for c in range(4):
                tot = [x for x in EXPLICIT if x[0] == c]
                ex = [x[1][j] for x in tot if qq["key"] in x[2]]
                if len(tot) < 30 or len(ex) < 15:
                    continue
                m = sum(ex) / len(ex)
                fb = qq["fb"][c]
                unset = len(tot) - len(ex)
                if unset < 10:
                    continue
                if (fb >= 0.3 and m < 0.2) or (fb <= 0.12 and m > 0.55) or abs(fb - m) > 0.45:
                    print(f"{qq['key']:<12} {'AVMO'[c]}    {fb:.2f}      {m:.2f}         {len(ex)}/{len(tot)}")
    if args.confusable:
        pairs = confusable(P.astype(np.float32), names, args.confusable)
        print(f"\n{len(pairs)} confusable pairs (< {args.confusable} strong differences):")
        for a, b, d in pairs:
            print(f"  {d:>2}  {a}  ~  {b}")

    if args.no_write:
        return 0
    qrank = rank_questions(P, priors)
    nib = [[quantise(float(v)) for v in row] for row in P]
    epoch = args.epoch if args.epoch is not None else int(
        os.environ.get("SOURCE_DATE_EPOCH", time.time()))
    info = write_dataset(
        args.out, names, cats, list(priors), [x["text"] for x in qs],
        [x["family"] for x in qs], nib, qrank,
        dataset_id=f"guesser-{n}x{q}", build_epoch=epoch,
        domains=[int(d) for d in doms], qmasks=qmasks)
    print(f"  wrote {info['path']}  {info['bytes'] / 1024.0:.0f} KB  crc {info['crc32']:#010x}")
    print(f"  best openers: {[qs[j]['text'] for j in qrank[:4]]}")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KbError as exc:
        print("ERROR:", exc)
        sys.exit(1)
