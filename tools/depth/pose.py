# body keypoints (COCO 17) for every picture, to find chest and hips -> out/pose.json
import glob, os, json, numpy as np
from PIL import Image
from ultralytics import YOLO
m = YOLO('yolov8x-pose.pt'); res = {}
for f in sorted(glob.glob('img/*.png')):
    im = Image.open(f)
    if im.mode == 'RGBA': a = np.asarray(im)[:, :, 3:] / 255.0; rgb = (np.asarray(im)[:, :, :3] * a + 128 * (1 - a)).astype(np.uint8)
    else: rgb = np.asarray(im.convert('RGB'))
    r = m.predict(rgb[:, :, ::-1].copy(), imgsz=640, conf=0.15, verbose=False)[0]
    people = []
    if r.keypoints is not None and len(r.keypoints):
        for kp, cf, box in zip(r.keypoints.xy.cpu().numpy(), r.keypoints.conf.cpu().numpy(), r.boxes.xyxy.cpu().numpy()):
            people.append({'kp': [[round(float(x), 1), round(float(y), 1), round(float(c), 2)] for (x, y), c in zip(kp, cf)], 'box': [round(float(v), 1) for v in box]})
    res[os.path.basename(f)[:-4] + '.GCC'] = {'w': rgb.shape[1], 'h': rgb.shape[0], 'people': people}
    print(f, len(people), flush=True)
os.makedirs('out', exist_ok=True); json.dump(res, open('out/pose.json', 'w'))
