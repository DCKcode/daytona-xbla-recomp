import os
import pathlib
import tempfile
import unittest
from unittest import mock

from native_shader_tools import find_dxc


class NativeShaderToolsTests(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.root = pathlib.Path(directory.name)

    def file(self, path):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.touch()
        return path

    def test_bundled_compilers_and_library_paths(self):
        for system, machine, arch, name, variable in (
            ("Linux", "x86_64", "x64", "dxc-linux", "LD_LIBRARY_PATH"),
            ("Darwin", "arm64", "arm64", "dxc-macos", "DYLD_LIBRARY_PATH"),
            ("Windows", "AMD64", "x64", "dxc.exe", "PATH"),
        ):
            with (
                self.subTest(system=system),
                mock.patch("platform.system", return_value=system),
                mock.patch("platform.machine", return_value=machine),
                mock.patch.dict(os.environ, {variable: "existing"}, clear=True),
            ):
                bundle = self.root / "thirdparty/dxc-bin"
                executable = self.file(bundle / "bin" / arch / name)
                dxc, env = find_dxc(self.root)
                self.assertEqual(dxc, executable)
                directory = (
                    executable.parent if system == "Windows" else bundle / "lib" / arch
                )
                separator = ";" if system == "Windows" else ":"
                self.assertEqual(env[variable], str(directory) + separator + "existing")

    @mock.patch("platform.system", return_value="Linux")
    @mock.patch("platform.machine", return_value="aarch64")
    def test_arm_linux_uses_installed_compiler(self, *_):
        executable = self.file(self.root / "native/bin/dxc")
        # An x86-64 binary in the checkout must not be selected on ARM64.
        self.file(self.root / "thirdparty/dxc-bin/bin/x64/dxc-linux")
        with mock.patch("shutil.which", return_value=str(executable)):
            dxc, env = find_dxc(self.root)
        self.assertEqual(dxc, executable)
        self.assertTrue(
            env["LD_LIBRARY_PATH"].startswith(str(self.root / "native/lib"))
        )

    def test_explicit_compiler_overrides_bundle(self):
        with (
            mock.patch("platform.system", return_value="Linux"),
            mock.patch("platform.machine", return_value="x86_64"),
        ):
            self.file(self.root / "thirdparty/dxc-bin/bin/x64/dxc-linux")
            explicit = self.file(self.root / "custom DXC/bin/dxc")
            dxc, _ = find_dxc(self.root, explicit)
            self.assertEqual(dxc, explicit)
            with self.assertRaisesRegex(ValueError, "executable not found"):
                find_dxc(self.root, self.root / "missing/dxc")

    def test_installed_executable_is_not_modified(self):
        executable = self.file(self.root / "system/bin/dxc")
        executable.chmod(0o755)
        with (
            mock.patch("platform.system", return_value="Linux"),
            mock.patch("platform.machine", return_value="aarch64"),
            mock.patch("shutil.which", return_value=str(executable)),
            mock.patch.object(pathlib.Path, "chmod", side_effect=PermissionError),
        ):
            dxc, _ = find_dxc(self.root)
            self.assertEqual(dxc, executable)

    def test_missing_native_compiler_explains_requirement(self):
        with (
            mock.patch("platform.system", return_value="Linux"),
            mock.patch("platform.machine", return_value="aarch64"),
            mock.patch("shutil.which", return_value=None),
            self.assertRaisesRegex(ValueError, "No native DXC found"),
        ):
            find_dxc(self.root)


if __name__ == "__main__":
    unittest.main()
