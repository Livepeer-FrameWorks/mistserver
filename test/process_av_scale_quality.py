#!/usr/bin/env python3
"""Compare the production CPU scaler with a high-quality downscale reference."""
import math
import subprocess
import sys

ffmpeg, processor = sys.argv[1:]
def run(command, data=None):
    return subprocess.run(command, input=data, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=True).stdout
source = run([ffmpeg, "-v", "error", "-f", "lavfi", "-i", "testsrc2=size=3840x2160:rate=1", "-frames:v", "1", "-pix_fmt", "yuv420p", "-f", "rawvideo", "pipe:1"])
reference = run([ffmpeg, "-v", "error", "-f", "rawvideo", "-pixel_format", "yuv420p", "-video_size", "3840x2160", "-i", "pipe:0", "-vf", "scale=640:360:flags=area", "-frames:v", "1", "-f", "rawvideo", "pipe:1"], source)
actual = run([processor, "scale"], source)
assert len(actual) == len(reference) == 640 * 360 * 3 // 2
mse = sum((a-b)**2 for a,b in zip(actual[:640*360], reference[:640*360])) / (640*360)
psnr = math.inf if mse == 0 else 10 * math.log10(255**2/mse)
print(f"4K to 360p luma PSNR against area reference: {psnr:.2f} dB", flush=True)
assert psnr >= 40, f"CPU downscale aliases fine detail: {psnr:.2f} dB below 40 dB"
