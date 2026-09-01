#!/usr/bin/env python3
"""Export compiled AI models to ONNX for MistServer (YOLO11 / YOLO26 / RT-DETR).

These models must be exported from PyTorch, which needs Python + ultralytics. For
download-only models (Depth Anything, SCRFD, ArcFace, RTMO, Parakeet) use
prepare_models.sh instead — it needs no Python and delegates here for the models below.

Requirements:
    pip install ultralytics onnx onnxslim onnxruntime

Usage:
    python export_models.py                    # Export all compiled models
    python export_models.py yolo11n            # Export a single model
    python export_models.py --list             # List available models
    python export_models.py --dir /path        # Custom output directory
    python export_models.py --category detect  # Only detection models
"""
import sys
import os
import argparse
import shutil

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))

MODELS = {
    # === YOLO11 Detection (export via ultralytics) ===
    'yolo11n':      {'method': 'ultralytics', 'imgsz': 640, 'category': 'detect'},
    'yolo11s':      {'method': 'ultralytics', 'imgsz': 640, 'category': 'detect'},
    'yolo11m':      {'method': 'ultralytics', 'imgsz': 640, 'category': 'detect'},
    'yolo11l':      {'method': 'ultralytics', 'imgsz': 640, 'category': 'detect'},
    'yolo11x':      {'method': 'ultralytics', 'imgsz': 640, 'category': 'detect'},
    # === YOLO11 Segmentation ===
    'yolo11n-seg':  {'method': 'ultralytics', 'imgsz': 640, 'category': 'segment'},
    'yolo11s-seg':  {'method': 'ultralytics', 'imgsz': 640, 'category': 'segment'},
    'yolo11m-seg':  {'method': 'ultralytics', 'imgsz': 640, 'category': 'segment'},
    'yolo11l-seg':  {'method': 'ultralytics', 'imgsz': 640, 'category': 'segment'},
    'yolo11x-seg':  {'method': 'ultralytics', 'imgsz': 640, 'category': 'segment'},
    # === YOLO11 Pose ===
    'yolo11n-pose': {'method': 'ultralytics', 'imgsz': 640, 'category': 'pose'},
    'yolo11s-pose': {'method': 'ultralytics', 'imgsz': 640, 'category': 'pose'},
    'yolo11m-pose': {'method': 'ultralytics', 'imgsz': 640, 'category': 'pose'},
    'yolo11l-pose': {'method': 'ultralytics', 'imgsz': 640, 'category': 'pose'},
    'yolo11x-pose': {'method': 'ultralytics', 'imgsz': 640, 'category': 'pose'},
    # === YOLO11 Classification ===
    'yolo11n-cls':  {'method': 'ultralytics', 'imgsz': 224, 'category': 'classify'},
    'yolo11s-cls':  {'method': 'ultralytics', 'imgsz': 224, 'category': 'classify'},
    'yolo11m-cls':  {'method': 'ultralytics', 'imgsz': 224, 'category': 'classify'},
    'yolo11l-cls':  {'method': 'ultralytics', 'imgsz': 224, 'category': 'classify'},
    'yolo11x-cls':  {'method': 'ultralytics', 'imgsz': 224, 'category': 'classify'},
    # === YOLO11 OBB ===
    'yolo11n-obb':  {'method': 'ultralytics', 'imgsz': 640, 'category': 'obb'},
    'yolo11s-obb':  {'method': 'ultralytics', 'imgsz': 640, 'category': 'obb'},
    'yolo11m-obb':  {'method': 'ultralytics', 'imgsz': 640, 'category': 'obb'},
    'yolo11l-obb':  {'method': 'ultralytics', 'imgsz': 640, 'category': 'obb'},
    'yolo11x-obb':  {'method': 'ultralytics', 'imgsz': 640, 'category': 'obb'},
    # === YOLO26 Detection ===
    'yolo26n':      {'method': 'ultralytics', 'imgsz': 640, 'category': 'detect'},
    'yolo26s':      {'method': 'ultralytics', 'imgsz': 640, 'category': 'detect'},
    'yolo26m':      {'method': 'ultralytics', 'imgsz': 640, 'category': 'detect'},
    'yolo26l':      {'method': 'ultralytics', 'imgsz': 640, 'category': 'detect'},
    'yolo26x':      {'method': 'ultralytics', 'imgsz': 640, 'category': 'detect'},
    # === YOLO26 Segmentation ===
    'yolo26n-seg':  {'method': 'ultralytics', 'imgsz': 640, 'category': 'segment'},
    'yolo26s-seg':  {'method': 'ultralytics', 'imgsz': 640, 'category': 'segment'},
    'yolo26m-seg':  {'method': 'ultralytics', 'imgsz': 640, 'category': 'segment'},
    'yolo26l-seg':  {'method': 'ultralytics', 'imgsz': 640, 'category': 'segment'},
    'yolo26x-seg':  {'method': 'ultralytics', 'imgsz': 640, 'category': 'segment'},
    # === YOLO26 Pose ===
    'yolo26n-pose': {'method': 'ultralytics', 'imgsz': 640, 'category': 'pose'},
    'yolo26s-pose': {'method': 'ultralytics', 'imgsz': 640, 'category': 'pose'},
    'yolo26m-pose': {'method': 'ultralytics', 'imgsz': 640, 'category': 'pose'},
    'yolo26l-pose': {'method': 'ultralytics', 'imgsz': 640, 'category': 'pose'},
    'yolo26x-pose': {'method': 'ultralytics', 'imgsz': 640, 'category': 'pose'},
    # === YOLO26 Classification ===
    'yolo26n-cls':  {'method': 'ultralytics', 'imgsz': 224, 'category': 'classify'},
    'yolo26s-cls':  {'method': 'ultralytics', 'imgsz': 224, 'category': 'classify'},
    'yolo26m-cls':  {'method': 'ultralytics', 'imgsz': 224, 'category': 'classify'},
    'yolo26l-cls':  {'method': 'ultralytics', 'imgsz': 224, 'category': 'classify'},
    'yolo26x-cls':  {'method': 'ultralytics', 'imgsz': 224, 'category': 'classify'},
    # === YOLO26 OBB ===
    'yolo26n-obb':  {'method': 'ultralytics', 'imgsz': 640, 'category': 'obb'},
    'yolo26s-obb':  {'method': 'ultralytics', 'imgsz': 640, 'category': 'obb'},
    'yolo26m-obb':  {'method': 'ultralytics', 'imgsz': 640, 'category': 'obb'},
    'yolo26l-obb':  {'method': 'ultralytics', 'imgsz': 640, 'category': 'obb'},
    'yolo26x-obb':  {'method': 'ultralytics', 'imgsz': 640, 'category': 'obb'},
    # === RT-DETR (export via ultralytics, simplify=False) ===
    'rtdetr-l':     {'method': 'ultralytics', 'imgsz': 640, 'category': 'detect',
                     'extra': {'simplify': False}},
    'rtdetr-x':     {'method': 'ultralytics', 'imgsz': 640, 'category': 'detect',
                     'extra': {'simplify': False}},
    # Download-only models (Depth Anything, SCRFD, ArcFace, RTMO, Parakeet) are handled
    # by prepare_models.sh — they need no Python. This file only covers models that must
    # be compiled/exported from PyTorch via ultralytics.
}


def export_ultralytics(name, info, output_dir):
    """Export model using ultralytics package."""
    try:
        from ultralytics import YOLO
    except ImportError:
        print("    ERROR: 'ultralytics' package not installed.")
        print("    Install with: pip install ultralytics onnx onnxslim onnxruntime")
        raise

    out_path = os.path.join(output_dir, f"{name}.onnx")
    print(f"    Exporting via ultralytics (imgsz={info['imgsz']})...")
    # ultralytics downloads the .pt and writes the .onnx into the current directory, which
    # under the process auto-provisioner may be non-writable or surprising. Run in
    # output_dir so all artifacts land where we want them.
    prev_cwd = os.getcwd()
    os.makedirs(output_dir, exist_ok=True)
    os.chdir(output_dir)
    try:
        model = YOLO(f"{name}.pt")
        extra = info.get('extra', {})
        model.export(format='onnx', imgsz=info['imgsz'], **extra)
        src_abs = os.path.abspath(f"{name}.onnx")
        if os.path.exists(src_abs) and src_abs != os.path.abspath(out_path):
            shutil.move(src_abs, out_path)
    finally:
        os.chdir(prev_cwd)


def provision_model(name, output_dir):
    """Export a single (compiled) model via ultralytics."""
    info = MODELS[name]
    os.makedirs(output_dir, exist_ok=True)
    filename = info.get('filename', f"{name}.onnx")
    out_path = os.path.join(output_dir, filename)
    if os.path.exists(out_path):
        print(f"  Skip {filename} (already exists)")
        return
    print(f"  Provisioning {name}...")
    export_ultralytics(name, info, output_dir)
    print(f"  Done: {out_path}")


def main():
    parser = argparse.ArgumentParser(
        description='Download/export AI models to ONNX for MistServer')
    parser.add_argument('models', nargs='*', help='Model names to provision (default: all)')
    parser.add_argument('--list', action='store_true', help='List available models')
    parser.add_argument('--dir', default=SCRIPT_DIR,
                        help=f'Output directory (default: {SCRIPT_DIR})')
    parser.add_argument('--category',
                        help='Only models in this category (detect, segment, pose, classify, obb)')
    args = parser.parse_args()

    if args.list:
        categories = {}
        for name in sorted(MODELS):
            info = MODELS[name]
            cat = info['category']
            if cat not in categories:
                categories[cat] = []
            method = 'export' if info['method'] == 'ultralytics' else 'download'
            categories[cat].append((name, method))
        for cat in sorted(categories):
            print(f"\n  {cat}:")
            for name, method in categories[cat]:
                print(f"    {name:30s}  [{method}]")
        print(f"\n  Total: {len(MODELS)} models")
        return

    if args.models:
        targets = args.models
    elif args.category:
        targets = sorted(n for n, i in MODELS.items() if i['category'] == args.category)
    else:
        targets = sorted(MODELS.keys())

    print(f"Output directory: {args.dir}")
    print(f"Models to provision: {len(targets)}")
    succeeded = 0
    failed = 0
    for name in targets:
        if name not in MODELS:
            print(f"  Unknown model: {name} (use --list to see available)")
            failed += 1
            continue
        try:
            provision_model(name, args.dir)
            succeeded += 1
        except Exception as e:
            print(f"  FAILED {name}: {e}")
            failed += 1
    print(f"\nDone! {succeeded} succeeded, {failed} failed.")
    # Non-zero exit on any failure so callers (prepare_models.sh under `set -e`, CI) can tell.
    sys.exit(1 if failed else 0)


if __name__ == '__main__':
    main()
