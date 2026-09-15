"""Compatibility entry point for the single deployable insights pipeline."""
from data_analysis.ml.insights import main

if __name__ == "__main__":
    import sys
    raise SystemExit(main(["train", *sys.argv[1:]]))
