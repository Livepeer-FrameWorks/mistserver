#!/usr/bin/env python3
"""Precompute CLIP text embeddings for zero-shot image tagging.

Runs the CLIP text tower over a list of labels and writes text_embeddings.json next
to the CLIP vision model. At runtime MistProcONNX (EmbeddingModel::loadMatchSet) ranks
each frame's image embedding against these by cosine similarity — no C++ tokenizer
needed. Each label is wrapped in the standard CLIP prompt template for better zero-shot
accuracy.

Usage:
  clip_text_embeddings.py --text-dir clip_vitb32_text --out-dir clip_vitb32 --labels "a cat,a dog"
  clip_text_embeddings.py --text-dir clip_vitb32_text --out-dir clip_vitb32 --labels-file labels.txt

--text-dir must contain text_model.onnx + tokenizer.json (provision `clip-vitb32-text`).
--out-dir is the vision model's directory (provision `clip-vitb32-vision`); the runtime
looks for text_embeddings.json next to the vision model.onnx. Defaults to --text-dir.

Needs: pip install onnxruntime tokenizers numpy
"""
import argparse
import json
import sys
from pathlib import Path

import numpy as np
import onnxruntime as ort
from tokenizers import Tokenizer


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--text-dir", required=True, help="dir with text_model.onnx + tokenizer.json")
    ap.add_argument("--out-dir", help="vision model dir to write text_embeddings.json into (default: --text-dir)")
    ap.add_argument("--labels", help="comma-separated label list")
    ap.add_argument("--labels-file", help="file with one label per line")
    ap.add_argument("--template", default="a photo of {}", help="prompt template, {} = label")
    ap.add_argument("--context-length", type=int, default=77)
    args = ap.parse_args()

    d = Path(args.text_dir)
    out_dir = Path(args.out_dir) if args.out_dir else d
    if args.labels:
        labels = [x.strip() for x in args.labels.split(",") if x.strip()]
    elif args.labels_file:
        labels = [x.strip() for x in Path(args.labels_file).read_text().splitlines() if x.strip()]
    else:
        sys.exit("Provide --labels or --labels-file")

    tok = Tokenizer.from_file(str(d / "tokenizer.json"))
    sess = ort.InferenceSession(str(d / "text_model.onnx"), providers=["CPUExecutionProvider"])
    in_names = {i.name for i in sess.get_inputs()}

    embeds = []
    for label in labels:
        prompt = args.template.format(label)
        enc = tok.encode(prompt)
        ids = enc.ids
        # CLIP pools the text embedding at the end-of-text token (highest id). If the
        # prompt is too long, keep the EOT by truncating to context_length-1 and
        # re-appending it, rather than cutting it off.
        if len(ids) > args.context_length:
            ids = ids[: args.context_length - 1] + [ids[-1]]
        ids = ids + [0] * (args.context_length - len(ids))
        feed = {"input_ids": np.array([ids], dtype=np.int64)}
        if "attention_mask" in in_names:
            mask = [1 if t != 0 else 0 for t in ids]
            feed["attention_mask"] = np.array([mask], dtype=np.int64)
        out = sess.run(None, feed)[0][0].astype(np.float32)
        out = out / (np.linalg.norm(out) + 1e-10)
        embeds.append(out.tolist())
        print(f"  embedded: {label!r} -> \"{prompt}\"", file=sys.stderr)

    outpath = out_dir / "text_embeddings.json"
    outpath.write_text(json.dumps({"labels": labels, "template": args.template, "embeddings": embeds}))
    print(f"wrote {outpath} ({len(labels)} labels, dim {len(embeds[0])})")


if __name__ == "__main__":
    main()
