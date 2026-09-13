"""Stands in for sa3_mlx.py: proves MLX Metal compute works from inside a
signed .app bundle, launched as a child process by a C++ host."""
import json, os, sys, time

t0 = time.time()
import mlx.core as mx

# Real GPU work — a matmul large enough that it cannot be constant-folded,
# forcing Metal kernel dispatch (and, on first run, shader compilation).
a = mx.random.normal((512, 512))
b = mx.random.normal((512, 512))
c = (a @ b).sum()
mx.eval(c)

print(json.dumps({
    "ok": True,
    "python": sys.version.split()[0],
    "executable": sys.executable,
    "mlx": mx.__version__,
    "default_device": str(mx.default_device()),
    "result": float(c),
    "seconds": round(time.time() - t0, 2),
}, indent=2))
