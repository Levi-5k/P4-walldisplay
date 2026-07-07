"""PlatformIO pre-script: enable ESP-IDF's ccache integration.

Sets IDF_CCACHE_ENABLE=1 in the environment used to invoke the IDF
build; ESP-IDF's CMake then plugs ccache into the C/C++ compiler launchers.
Re-builds with unchanged TUs become 3–5x faster.
"""
import glob
import os

Import("env")  # noqa: F821 (provided by SCons/PlatformIO)

compiler_paths = []
for package_root in ("tools", "packages"):
	compiler_paths.extend(glob.glob(os.path.expanduser(f"~/.platformio/{package_root}/toolchain-riscv32-esp*/bin/riscv32-esp-elf-gcc")))

for compiler in compiler_paths:
	toolchain_bin = os.path.dirname(compiler)
	os.environ["PATH"] = toolchain_bin + os.pathsep + os.environ.get("PATH", "")
	env.PrependENVPath("PATH", toolchain_bin)
	break

os.environ["IDF_CCACHE_ENABLE"] = "1"
print("ccache: IDF_CCACHE_ENABLE=1 (ensure `ccache` is on PATH)")
