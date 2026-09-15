"""Show the hash-bound evaluation; never silently regenerate frozen scores."""
from data_analysis.ml.insights import main

if __name__ == "__main__":
    import sys
    raise SystemExit(main(["report", *sys.argv[1:]]))
