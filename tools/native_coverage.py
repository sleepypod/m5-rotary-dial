"""PlatformIO extra_script for [env:native]: link with --coverage.

build_flags only reach the compiler for -f*/--coverage, so the gcov runtime
(libgcov on gcc, clang_rt.profile on clang) would otherwise be missing at
link time.
"""

Import("env")  # noqa: F821 - injected by PlatformIO

env.Append(LINKFLAGS=["--coverage"])  # noqa: F821
