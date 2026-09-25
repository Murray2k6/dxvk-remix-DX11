#!/usr/bin/env python3
"""Compile DX11 Remix shaders directly from the checked-out source tree.

The compiler's depfiles must name real sources, not copies in a temporary
directory. The latter become missing dependencies after each invocation and
make every incremental build recompile the entire shader set.
"""

from pathlib import Path
import runpy
import sys


def main() -> None:
    compiler = Path(__file__).resolve().with_name("compile_shaders_real_original_v174.py")
    sys.argv = [str(compiler), "-dx11"] + sys.argv[1:]
    runpy.run_path(str(compiler), run_name="__main__")


if __name__ == "__main__":
    main()
