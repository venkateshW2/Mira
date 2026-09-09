# source this before any command in this project
# STUDIO self-locates from this file, so the folder can be renamed or moved.
STUDIO="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export STUDIO
export HF_HOME="$STUDIO/.hf"                    # HuggingFace models download here
export HUGGINGFACE_HUB_CACHE="$STUDIO/.hf/hub"
export UV_CACHE_DIR="$STUDIO/.uvcache"          # uv package cache here
