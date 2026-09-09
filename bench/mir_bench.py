import csv, json, time
import numpy as np
import essentia.standard as es
import onnxruntime as ort

WAV, DUR = "test60.wav", 60.0

def t(label, fn):
    t0 = time.perf_counter()
    out = fn()
    dt = time.perf_counter() - t0
    print(f"  {label:<36} {dt:7.2f}s  {DUR/dt:8.1f}x realtime")
    return out, dt

print(f"\n=== {WAV}  ({DUR:.0f}s audio) ===\n")

# ---------- 1. DSP / MIR ----------
print("--- 1. DSP / MIR layer (Essentia C++) ---")
audio44, _ = t("load @44.1k mono", lambda: es.MonoLoader(filename=WAV, sampleRate=44100)())
rhythm, _  = t("RhythmExtractor2013 (multifeature)",
               lambda: es.RhythmExtractor2013(method="multifeature")(audio44))
keyres, _  = t("KeyExtractor", lambda: es.KeyExtractor()(audio44))
stereo = np.column_stack([audio44, audio44]).astype(np.float32)
loud, _    = t("LoudnessEBUR128", lambda: es.LoudnessEBUR128()(stereo))
print(f"    -> BPM {rhythm[0]:.1f} (conf {rhythm[2]:.2f}) | "
      f"key {keyres[0]} {keyres[1]} (str {keyres[2]:.2f}) | LUFS {loud[2]:.1f}")

# ---------- 2. discogs-effnet + head ----------
print("\n--- 2. discogs-effnet + instrument head (Essentia TF) ---")
audio16, _ = t("load @16k mono", lambda: es.MonoLoader(filename=WAV, sampleRate=16000)())
emb_m = es.TensorflowPredictEffnetDiscogs(
    graphFilename="models/discogs-effnet-bs64-1.pb", output="PartitionedCall:1")
emb, t_emb = t("discogs-effnet EMBEDDING", lambda: emb_m(audio16))
head = es.TensorflowPredict2D(
    graphFilename="models/mtg_jamendo_instrument-discogs-effnet-1.pb")
inst, t_head = t("instrument head (40 classes)", lambda: head(emb))
emb_a = np.asarray(emb)
print(f"    embedding shape {emb_a.shape}  ({emb_a.shape[1]}-dim x {emb_a.shape[0]} patches)")
labels40 = json.load(open("models/mtg_jamendo_instrument-discogs-effnet-1.json"))["classes"]
mean = np.asarray(inst).mean(axis=0)
print("    -> top instruments:")
for i in np.argsort(-mean)[:8]:
    print(f"        {labels40[i]:<20} {mean[i]:.3f}")

# ---------- 3. CED / AudioSet via ONNX ----------
print("\n--- 3. CED-small (ONNX, AudioSet 527) ---")

def melbank(sr=16000, n_fft=512, n_mels=64):
    hz2mel = lambda f: 2595.0 * np.log10(1.0 + f / 700.0)
    mel2hz = lambda m: 700.0 * (10 ** (m / 2595.0) - 1.0)
    f = mel2hz(np.linspace(hz2mel(0), hz2mel(sr / 2), n_mels + 2))
    b = np.floor((n_fft + 1) * f / sr).astype(int)
    fb = np.zeros((n_mels, n_fft // 2 + 1), dtype=np.float32)
    for i in range(n_mels):
        l, c, r = b[i], max(b[i + 1], b[i] + 1), max(b[i + 2], b[i + 1] + 2)
        for k in range(l, min(c, fb.shape[1])): fb[i, k] = (k - l) / max(c - l, 1)
        for k in range(c, min(r, fb.shape[1])): fb[i, k] = (r - k) / max(r - c, 1)
    return fb

FB = melbank()

def features(y, n_fft=512, hop=160):
    win = np.hanning(n_fft).astype(np.float32)
    n = 1 + (len(y) - n_fft) // hop
    idx = np.arange(n_fft)[None, :] + hop * np.arange(n)[:, None]
    S = np.abs(np.fft.rfft(y[idx] * win, axis=1)) ** 2
    return np.log(S @ FB.T + 1e-6).T.astype(np.float32)

feats, _ = t("log-mel featurisation (numpy)", lambda: features(audio16))
print(f"    feats shape {feats.shape}")

as_labels = [r["display_name"] for r in csv.DictReader(open("models/audioset_labels.csv"))]
W = 1000  # 10 s at 10 ms hop
chunks = [feats[:, i:i + W] for i in range(0, feats.shape[1], W)]
chunks = [c for c in chunks if c.shape[1] >= 100]
print(f"    {len(chunks)} chunks of <=10s")

for prov in ["CPUExecutionProvider", "CoreMLExecutionProvider"]:
    try:
        sess = ort.InferenceSession("models/ced-small.onnx", providers=[prov])
    except Exception as e:
        print(f"  {prov}: FAILED — {type(e).__name__}: {e}")
        continue
    run = lambda: np.mean([sess.run(["prob"], {"feats": c[None]})[0][0] for c in chunks], axis=0)
    try:
        run()  # warmup
        probs, _ = t(f"CED inference [{prov.replace('ExecutionProvider','')}]", run)
    except Exception as e:
        print(f"  {prov}: FAILED — {type(e).__name__}: {e}")
        continue
    if prov == "CPUExecutionProvider":
        print("    -> top AudioSet events:")
        for i in np.argsort(-probs)[:8]:
            print(f"        {as_labels[i]:<20} {probs[i]:.3f}")

print(f"\n--- neural cost summary ---")
print(f"  discogs-effnet embedding + head : {t_emb + t_head:6.2f}s  -> {DUR/(t_emb+t_head):.1f}x realtime")
print()
