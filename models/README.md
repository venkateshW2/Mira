# models/

Downloaded model weights (`.pb`, `.onnx`) — gitignored (PRD .gitignore, "13 GB+").
Populated by `mira models --download` once that CLI command exists (PRD §8), or
manually for now. See PRD §2c for the model list and §16.3 for the two models needing
offline `tf2onnx` conversion via `lab/`.
