"""Compile/run the current production NR backend against an existing native build.

This compiles a test executable only; it does not build or deploy Magpie.
The build root contains obj/ plus build/ (or Magpie-Experimental-x64/).
"""
from pathlib import Path
import argparse
import ctypes
import subprocess
import sys

parser = argparse.ArgumentParser()
parser.add_argument("build_root", type=Path)
parser.add_argument("--timing", action="store_true")
options = parser.parse_args()
repo = Path(__file__).resolve().parents[1]
root = options.build_root.resolve()
runtime = next((root / folder for folder in ("build", "Magpie-Experimental-x64")
                if (root / folder / "Magpie.Core.lib").is_file()), None)
if runtime is None:
    parser.error("The existing build must contain Magpie.Core.lib and the DLSSNR runtime.")

shell32 = ctypes.windll.shell32
shell32.CommandLineToArgvW.argtypes = [ctypes.c_wchar_p, ctypes.POINTER(ctypes.c_int)]
shell32.CommandLineToArgvW.restype = ctypes.POINTER(ctypes.c_wchar_p)
ctypes.windll.kernel32.LocalFree.argtypes = [ctypes.c_void_p]

def command_args(text):
    count = ctypes.c_int()
    pointer = shell32.CommandLineToArgvW("tool " + text, ctypes.byref(count))
    result = [pointer[i] for i in range(1, count.value)]
    ctypes.windll.kernel32.LocalFree(pointer)
    return result

def tlog(path):
    return "\n".join(line for line in path.read_text(encoding="utf-16").splitlines()
                     if not line.startswith("^"))

clargs = command_args(tlog(root / "obj/Magpie.Core/Magpie.Core.tlog/CL.command.1.tlog"))
includes = list(dict.fromkeys(arg for arg in clargs if arg.lower().startswith("/i")))
definitions = list(dict.fromkeys(clargs[i + 1] for i, arg in enumerate(clargs)
                                 if arg == "/D" and clargs[i + 1] != "NDEBUG"))
defs = [item for value in definitions for item in ("/D", value)]
if options.timing:
    defs += ["/D", "MP_ENABLE_NATIVE_BACKEND_TIMING"]
linkargs = command_args(tlog(root / "obj/Magpie/Magpie.tlog/link.command.1.tlog"))
libs = list(dict.fromkeys(arg for arg in linkargs
                         if arg.lower().endswith(".lib") or arg.lower().startswith("/libpath:")))
libs = [arg for arg in libs if Path(arg).name.lower() != "magpie.lib"]
if not any(Path(arg).name.lower() == "magpie.core.lib" for arg in libs):
    libs.append(str(runtime / "Magpie.Core.lib"))

exe = runtime / "DLSSNRPipelineTests.exe"
command = ["cl.exe", "/nologo", "/std:c++20", "/EHsc", "/utf-8", "/MT", "/O2", "/bigobj",
           "/I" + str(repo / "src/Magpie.Core"), "/I" + str(repo / "src/Magpie.Core/include")]
command += includes + defs + [str(repo / "tests/DLSSNRPipelineTests.cpp"),
                              "/Fe:" + str(exe), "/Fo:" + str(exe.with_suffix(".obj")), "/link", "/LTCG"]
command += libs + ["d3d11.lib", "d3dcompiler.lib", "dxgi.lib", "d3d12.lib", "WindowsApp.lib"]
print("Compiling the native test against current production sources.", flush=True)
result = subprocess.run(command, cwd=repo / "src/Magpie.Core")
if result.returncode:
    sys.exit(result.returncode)
sys.exit(subprocess.run([str(exe)], cwd=runtime).returncode)
