import glob, os
from ultralytics import YOLO

models_dir = os.path.join("build", "Release", "models")
pt_files = glob.glob(os.path.join(models_dir, "*.pt"))

for pt in pt_files:
    onnx_path = os.path.splitext(pt)[0] + ".onnx"
    if os.path.exists(onnx_path):
        print(f"SKIP (exists): {onnx_path}")
        continue
    print(f"Exporting: {pt}")
    try:
        YOLO(pt).export(format="onnx", opset=12, dynamic=False, simplify=True)
        print(f"  -> done")
    except Exception as e:
        print(f"  !! FAILED: {e}")
