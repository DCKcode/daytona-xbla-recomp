import os
import pathlib
import platform
import shutil
import stat
import subprocess


def find_dxc(
    root: pathlib.Path, override: pathlib.Path | None = None
) -> tuple[pathlib.Path, dict[str, str]]:
    system = platform.system()
    machine = platform.machine().lower()
    architecture = {
        "x86_64": "x64",
        "amd64": "x64",
        "aarch64": "arm64",
        "arm64": "arm64",
    }.get(machine)
    executable = {
        "Linux": "dxc-linux",
        "Darwin": "dxc-macos",
        "Windows": "dxc.exe",
    }.get(system)
    if not architecture or not executable:
        raise ValueError(f"Unsupported shader build host: {system} {machine}")

    bundle = root / "thirdparty/dxc-bin"
    bundled = bundle / "bin" / architecture / executable
    if override is not None:
        dxc = override.resolve()
        libraries = dxc.parent.parent / "lib"
    elif bundled.is_file():
        dxc = bundled
        libraries = bundle / "lib" / architecture
    else:
        installed = shutil.which("dxc")
        if not installed:
            raise ValueError(
                "No native DXC found. Install DXC with SPIR-V support and put "
                "dxc on PATH, or pass --dxc /path/to/dxc."
            )
        dxc = pathlib.Path(installed).resolve()
        libraries = dxc.parent.parent / "lib"
    if not dxc.is_file():
        raise ValueError(f"DXC executable not found: {dxc}")
    if system != "Windows" and not os.access(dxc, os.X_OK):
        dxc.chmod(dxc.stat().st_mode | stat.S_IXUSR)

    env = os.environ.copy()
    variable = {
        "Linux": "LD_LIBRARY_PATH",
        "Darwin": "DYLD_LIBRARY_PATH",
        "Windows": "PATH",
    }[system]
    directory = dxc.parent if system == "Windows" else libraries
    separator = ";" if system == "Windows" else ":"
    env[variable] = str(directory) + (
        separator + env[variable] if env.get(variable) else ""
    )
    return dxc, env


def build_translator(root: pathlib.Path, output: pathlib.Path) -> pathlib.Path:
    build = output / "translator"
    source = pathlib.Path(__file__).resolve().parent / "shader_translate"
    subprocess.run(
        [
            "cmake",
            "-S",
            str(source),
            "-B",
            str(build),
            "-G",
            "Ninja",
            "-DCMAKE_BUILD_TYPE=Release",
            "-DCMAKE_CXX_COMPILER=clang++",
            f"-DXENOS_RECOMP_SOURCE_DIR={root}",
        ],
        check=True,
    )
    subprocess.run(["cmake", "--build", str(build), "--config", "Release"], check=True)
    suffix = ".exe" if platform.system() == "Windows" else ""
    return build / "bin" / ("daytona_shader_translate" + suffix)
