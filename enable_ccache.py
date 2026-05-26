"""PlatformIO pre-script: enable ESP-IDF's ccache integration.

Sets IDF_CCACHE_ENABLE=1 in the environment used to invoke the IDF
build; ESP-IDF's CMake then plugs ccache into the C/C++ compiler launchers.
Re-builds with unchanged TUs become 3–5x faster.
"""
import os

Import("env")  # noqa: F821 (provided by SCons/PlatformIO)

os.environ["IDF_CCACHE_ENABLE"] = "1"
print("ccache: IDF_CCACHE_ENABLE=1 (ensure `ccache` is on PATH)")
