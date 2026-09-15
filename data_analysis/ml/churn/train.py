"""Compatibility entry point; trains both administrator capabilities once."""
from data_analysis.ml.insights import main

if __name__ == "__main__":
    import sys
    raise SystemExit(main(["train", *sys.argv[1:]]))
