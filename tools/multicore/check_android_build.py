import itertools
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
VARIABLES = (
    "MOCK_HOST", "TARGET", "TARGET_IS_ANDROID", "TARGET_TRIPLE", "IS_X86",
    "IS_64BIT", "HAVE_CDROM", "CFLAGS", "CXXFLAGS", "LDFLAGS", "GL_LIB",
    "SOURCES_C", "GLES", "GLES3", "LOCAL_CFLAGS", "LOCAL_CXXFLAGS",
    "LOCAL_LDFLAGS", "LOCAL_LDLIBS", "APP_OPTIM", "APP_ABI", "APP_STL",
    "OBJECT_DIR", "OBJECTS", "DEPS", "LIBS", "SYS_LIBS",
)
TARGETS = (
    ("armv7a-linux-androideabi21", "armeabi-v7a", "arm", "0", "0"),
    ("aarch64-linux-android21", "arm64-v8a", "arm64", "0", "1"),
    ("x86_64-linux-android21", "x86_64", "x86_64", "1", "1"),
)
HOSTS = (("Linux", "x86_64"), ("Linux", "aarch64"),
         ("Darwin", "arm64"), ("Haiku", "x86_64"), ("SunOS", "sparc"))


def find_tool(variable, names, candidates):
    explicit = os.environ.get(variable)
    if explicit:
        return explicit
    for name in names:
        found = shutil.which(name)
        if found:
            return found
    for candidate in candidates:
        if candidate.is_file():
            return str(candidate)
    raise unittest.SkipTest(f"Set {variable} to the required executable")


class AndroidBuildTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        ndks = [Path(os.environ[key]) for key in
                ("ANDROID_NDK_HOME", "ANDROID_NDK_ROOT") if key in os.environ]
        if "LOCALAPPDATA" in os.environ:
            ndks.extend(sorted((Path(os.environ["LOCALAPPDATA"]) /
                                "Android/Sdk/ndk").glob("*"), reverse=True))
        cls.make = find_tool("ANDROID_BUILD_TEST_MAKE", ("make", "gmake", "mingw32-make"),
                             [ndk / "prebuilt/windows-x86_64/bin/make.exe" for ndk in ndks])
        cls.shell = find_tool("ANDROID_BUILD_TEST_SH", ("sh",),
                              [Path(os.environ.get("ProgramFiles", "C:/Program Files")) /
                               "Git/usr/bin/sh.exe"])
        git_shell = Path(cls.shell).parent.parent / "usr/bin/sh.exe"
        if git_shell.is_file() and "ANDROID_BUILD_TEST_SH" not in os.environ:
            cls.shell = str(git_shell)
        cls.temporary = tempfile.TemporaryDirectory(prefix="android_build_")
        cls.addClassCleanup(cls.temporary.cleanup)
        cls.directory = Path(cls.temporary.name)
        (cls.directory / "jni").mkdir()
        for name in ("Makefile", "Makefile.common", "jni/Android.mk", "jni/Application.mk"):
            shutil.copyfile(ROOT / name, cls.directory / name)
        (cls.directory / "empty.mk").write_text("", encoding="utf-8")
        (cls.directory / "compiler.py").write_text(
            "import os, sys\n"
            "if '-dumpmachine' not in sys.argv:\n"
            "    raise SystemExit('Only compiler target queries are allowed')\n"
            "target = os.environ['MOCK_TARGET']\n"
            "for index, argument in enumerate(sys.argv[1:], 1):\n"
            "    if argument.startswith('--target='):\n"
            "        target = argument.split('=', 1)[1]\n"
            "    elif argument in ('--target', '-target'):\n"
            "        target = sys.argv[index + 1]\n"
            "print(target)\n", encoding="utf-8")
        uname = (
            "uname() { case \"$1\" in\n"
            "    -s) printf '%s\\n' \"$MOCK_UNAME_S\" ;;\n"
            "    -p|-m) printf '%s\\n' \"$MOCK_UNAME_M\" ;;\n"
            "    -a) printf '%s mock %s\\n' \"$MOCK_UNAME_S\" \"$MOCK_UNAME_M\" ;;\n"
            "    *) return 1 ;;\n"
            "esac; }\n"
        )
        # GNU Make 3.81 (still shipped on macOS) ignores .SHELLFLAGS.
        # On POSIX use an executable shell adapter which accepts make's -c;
        # Windows needs the explicit interpreter used by the existing harness.
        cls.mock_shell = cls.directory / "shell.sh"
        script = uname + 'eval "$1"\n'
        if os.name != "nt":
            script = ("#!" + cls.shell + "\n" + uname +
                      'if [ "$1" = "-c" ]; then shift; fi\n' + 'eval "$1"\n')
        cls.mock_shell.write_bytes(script.encode("utf-8"))
        if os.name != "nt":
            cls.mock_shell.chmod(0o700)
        report = (
            ".PHONY: check-android-config\n"
            "check-android-config:\n" +
            "".join(f"\t$(info ANDROID_CHECK|{name}|$({name}))\n" for name in VARIABLES) +
            "\t@:\n"
        )
        host = "MOCK_HOST := $(shell printf '%s:%s' \"$$(uname -s)\" \"$$(uname -m)\")\n"
        (cls.directory / "root.mk").write_text(host + "include Makefile\n" + report,
                                               encoding="utf-8")
        (cls.directory / "jni.mk").write_text(
            host + "my-dir = $(CURDIR)/jni\n"
            "CLEAR_VARS := empty.mk\n"
            "BUILD_SHARED_LIBRARY := empty.mk\n"
            "include jni/Application.mk\n"
            "include jni/Android.mk\n" + report, encoding="utf-8")

    def config(self, triple, platform="unix", host=("Linux", "x86_64"), jni=False, **overrides):
        ignored = set(VARIABLES) | {
            "MAKEFLAGS", "MFLAGS", "MAKEOVERRIDES", "GNUMAKEFLAGS", "CC", "CXX",
            "CPPFLAGS", "FLAGS", "COREFLAGS", "INCFLAGS", "EXTRA_FLAGS", "LIBS",
            "DEBUG", "HAVE_HW", "HAVE_OPENGL", "HAVE_VULKAN", "HAVE_CHD",
            "HAVE_LIGHTREC", "LIGHTREC_DEBUG", "LIGHTREC_LOG_LEVEL", "PTHREAD_FLAGS",
            "THREADED_RECOMPILER", "LINK_STATIC_LIBCPLUSPLUS", "SANITIZER",
            "STATIC_LINKING", "SYSTEM_LIBCHDR", "NO_GCC", "TARGET_ARCH_ABI",
            "TARGET_ARCH", "ANDROID_ABI", "platform", "CROSS_COMPILE", "GIT_VERSION",
        }
        env = {key: value for key, value in os.environ.items() if key not in ignored}
        env.update(MOCK_TARGET=triple, MOCK_UNAME_S=host[0], MOCK_UNAME_M=host[1])
        env["PATH"] = str(Path(self.shell).parent) + os.pathsep + env.get("PATH", "")
        compiler = f'"{Path(sys.executable).as_posix()}" "compiler.py"'
        arguments = {
            "SHELL": (Path(self.shell) if os.name == "nt" else self.mock_shell).as_posix(),
            ".SHELLFLAGS": "shell.sh" if os.name == "nt" else "-c",
            "CC": compiler, "CXX": compiler, "GIT_VERSION": "test", "platform": platform,
        }
        arguments.update(overrides)
        result = subprocess.run(
            [self.make, "--no-print-directory", "-rR", "-n", "-f",
             "jni.mk" if jni else "root.mk", "check-android-config"] +
            [f"{key}={value}" for key, value in arguments.items()],
            cwd=self.directory, env=env, text=True, capture_output=True, timeout=120,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertNotIn("not found", result.stderr, result.stdout + result.stderr)
        values = {}
        for line in result.stdout.splitlines():
            if line.startswith("ANDROID_CHECK|"):
                _, name, value = line.split("|", 2)
                values[name] = value.strip()
        self.assertEqual(set(values), set(VARIABLES), result.stdout + result.stderr)
        self.assertEqual(values["MOCK_HOST"], ":".join(host), result.stdout + result.stderr)
        return values

    def assert_android(self, values, x86, bits, jni=False, lightrec=True, debug=False):
        self.assertEqual(values["IS_X86"], x86)
        self.assertEqual(values["IS_64BIT"], bits)
        if not jni:
            self.assertEqual(values["TARGET_IS_ANDROID"], "1")
            self.assertEqual(values["HAVE_CDROM"], "0")
            self.assertNotIn("/cdrom/cdrom.c", values["SOURCES_C"])
        for name in (("LOCAL_CFLAGS", "LOCAL_CXXFLAGS") if jni else ("CFLAGS", "CXXFLAGS")):
            flags = values[name].split()
            for flag in ("-fwrapv", "-fsigned-char", "-DANDROID", "-DHAVE_MMAP"):
                self.assertIn(flag, flags, name)
            for flag in ("-DHAVE_ASHMEM", "-DHAVE_LIGHTREC"):
                self.assertEqual(flag in flags, lightrec, name)
            self.assertEqual("-DARCH_X86" in flags, x86 == "1", name)
            self.assertEqual("-fomit-frame-pointer" in flags, x86 == "1", name)
            for flag in ("-DHAVE_CDROM", "-DHAVE_SHM", "-ffast-math", "-Ofast",
                         "-funsafe-math-optimizations", "-mfpu=neon", "-DGPU_HAVE_NEON=1"):
                self.assertNotIn(flag, flags, name)
            self.assertFalse(any(flag.startswith(("-march=", "-mcpu=", "-mtune="))
                                 for flag in flags), name)
            for flag in (("-O0", "-g", "-DDEBUG") if debug else ("-O3", "-DNDEBUG")):
                self.assertIn(flag, flags, name)
            for flag in (("-O3", "-DNDEBUG") if debug else ("-O0", "-DDEBUG")):
                self.assertNotIn(flag, flags, name)
        links = " ".join(values[name] for name in
                         (("LOCAL_LDFLAGS", "LOCAL_LDLIBS") if jni else ("LDFLAGS", "GL_LIB", "LIBS", "SYS_LIBS"))).split()
        for flag in ("-Wl,--build-id=sha1", "-Wl,-z,max-page-size=16384",
                     "-Wl,-z,common-page-size=16384", "-ldl", "-llog", "-landroid"):
            self.assertIn(flag, links)
        for flag in ("-lrt", "-lpthread", "-lroot", "-lGL", "-static-libgcc"):
            self.assertNotIn(flag, links)
        if not jni:
            self.assertIn("-static-libstdc++", links)

    def test_android_system_libraries_survive_link_overrides(self):
        for target, platform in itertools.product(TARGETS, ("unix", "android")):
            with self.subTest(target=target[0], platform=platform):
                values = self.config(target[0], platform,
                                     LDFLAGS="-Wl,--build-id=sha1", LIBS="-lcustom")
                self.assertEqual(values["LDFLAGS"], "-Wl,--build-id=sha1")
                self.assertEqual(values["LIBS"], "-lcustom")
                for flag in ("-ldl", "-llog", "-landroid", "-lm"):
                    self.assertIn(flag, values["SYS_LIBS"].split())

    def test_shared_cpp_runtime_override(self):
        values = self.config(TARGETS[1][0], LINK_STATIC_LIBCPLUSPLUS="0")
        self.assertNotIn("-static-libstdc++", values["LDFLAGS"].split())

    def test_isolated_object_directories(self):
        baseline = self.config(TARGETS[0][0])
        isolated = self.config(TARGETS[0][0], OBJECT_DIR="build/arm32/obj")
        self.assertTrue(baseline["OBJECTS"].split())
        self.assertEqual(isolated["OBJECTS"].split(),
                         ["build/arm32/obj/" + name for name in baseline["OBJECTS"].split()])
        self.assertEqual(isolated["DEPS"].split(),
                         [name[:-2] + ".d" for name in isolated["OBJECTS"].split()])

    def test_root_android_target_matrix(self):
        for target, platform, host in itertools.product(TARGETS, ("unix", "android"), HOSTS):
            triple, _, _, x86, bits = target
            with self.subTest(triple=triple, platform=platform, host=host):
                values = self.config(triple, platform, host)
                self.assert_android(values, x86, bits)
                suffix = "_android" if platform == "android" else ""
                self.assertEqual(values["TARGET"], f"mednafen_psx_libretro{suffix}.so")

    def test_root_explicit_abi_and_compiler_target(self):
        for target, platform, abi_variable in itertools.product(
                TARGETS, ("unix", "android"), ("TARGET_ARCH_ABI", "ANDROID_ABI")):
            triple, abi, _, x86, bits = target
            with self.subTest(abi=abi, platform=platform, variable=abi_variable):
                values = self.config("x86_64-pc-linux-gnu", platform, **{abi_variable: abi})
                self.assert_android(values, x86, bits)
                compiler = f'"{Path(sys.executable).as_posix()}" "compiler.py" --target={triple}'
                values = self.config("x86_64-pc-linux-gnu", platform, CC=compiler, CXX=compiler)
                self.assert_android(values, x86, bits)
        values = self.config("x86_64-pc-linux-gnu", TARGET_TRIPLE="aarch64-linux-android21")
        self.assert_android(values, "0", "1")

    def test_root_explicit_overrides(self):
        for platform in ("unix", "android"):
            with self.subTest(platform=platform):
                values = self.config("aarch64-linux-android21", platform,
                                     IS_X86="1", IS_64BIT="0", HAVE_CDROM="1",
                                     TARGET="custom.so", HAVE_OPENGL="1", GLES3="0",
                                     GL_LIB="-lGLESv2", EXTRA_FLAGS="-DTEST_OVERRIDE")
                self.assertEqual(values["IS_X86"], "1")
                self.assertEqual(values["IS_64BIT"], "0")
                self.assertEqual(values["HAVE_CDROM"], "1")
                self.assertEqual(values["TARGET"], "custom.so")
                self.assertEqual(values["GLES3"], "0")
                self.assertEqual(values["GL_LIB"], "-lGLESv2")
                for name in ("CFLAGS", "CXXFLAGS"):
                    for flag in ("-DARCH_X86", "-fomit-frame-pointer", "-DHAVE_CDROM", "-DTEST_OVERRIDE"):
                        self.assertIn(flag, values[name].split())
                values = self.config("x86_64-linux-android21", platform, IS_X86="0")
                self.assert_android(values, "0", "1")

    def test_root_hw_debug_and_interpreter(self):
        for target, platform in itertools.product(TARGETS, ("unix", "android")):
            triple, _, _, x86, bits = target
            with self.subTest(triple=triple, platform=platform):
                values = self.config(triple, platform, HAVE_HW="1")
                self.assert_android(values, x86, bits)
                self.assertEqual(values["GLES3"], "1")
                self.assertEqual(values["GL_LIB"], "-lGLESv3 -lEGL")
                self.assertIn("-DHAVE_OPENGLES3", values["CFLAGS"].split())
                values = self.config(triple, platform, HAVE_LIGHTREC="0", DEBUG="1")
                self.assert_android(values, x86, bits, lightrec=False, debug=True)

    def test_non_android_unix_regression(self):
        for host in HOSTS[:3]:
            with self.subTest(host=host):
                values = self.config("aarch64-linux-gnu", host=host)
                self.assertEqual(values["TARGET_IS_ANDROID"], "0")
                self.assertEqual(values["IS_X86"], "1" if host[1] == "x86_64" else "")
                self.assertEqual(values["HAVE_CDROM"], "1" if host[0] == "Linux" else "0")
                self.assertEqual(values["TARGET"], "mednafen_psx_libretro.so")
                for flag in ("-lpthread", "-lrt", "-ldl", "-static-libgcc", "-static-libstdc++"):
                    self.assertIn(flag, values["LDFLAGS"].split())
                for flag in ("-O3", "-DNDEBUG", "-fwrapv", "-fsigned-char", "-DHAVE_SHM"):
                    self.assertIn(flag, values["CFLAGS"].split())
                self.assertNotIn("-DANDROID", values["CFLAGS"].split())
                self.assertNotIn("--build-id", values["LDFLAGS"])
                self.assertNotIn("page-size", values["LDFLAGS"])
        values = self.config("x86_64-linux-gnu", IS_X86="0", HAVE_CDROM="0")
        self.assertNotIn("-DARCH_X86", values["CFLAGS"].split())
        self.assertNotIn("-DHAVE_CDROM", values["CFLAGS"].split())

    def test_jni_target_matrix(self):
        for target, host in itertools.product(TARGETS, HOSTS[:3]):
            triple, abi, arch, x86, bits = target
            with self.subTest(abi=abi, host=host):
                values = self.config(triple, host=host, jni=True,
                                     TARGET_ARCH_ABI=abi, TARGET_ARCH=arch)
                self.assert_android(values, x86, bits, jni=True)
                self.assertEqual(values["APP_OPTIM"], "release")
                self.assertEqual(values["APP_ABI"], "all")
                self.assertEqual(values["APP_STL"], "c++_static")

    def test_jni_debug_hw_and_overrides(self):
        for triple, abi, arch, x86, bits in TARGETS:
            with self.subTest(abi=abi):
                values = self.config(triple, jni=True, TARGET_ARCH_ABI=abi, TARGET_ARCH=arch,
                                     APP_OPTIM="debug", HAVE_LIGHTREC="0", HAVE_HW="1")
                self.assert_android(values, x86, bits, jni=True, lightrec=False, debug=True)
                self.assertEqual(values["APP_OPTIM"], "debug")
                self.assertEqual(values["GLES3"], "1")
                self.assertIn("-DHAVE_OPENGLES3", values["LOCAL_CFLAGS"].split())
                self.assertIn("-lGLESv3", values["LOCAL_LDLIBS"].split())
        values = self.config("x86_64-linux-android21", jni=True,
                             TARGET_ARCH_ABI="x86_64", TARGET_ARCH="x86_64",
                             IS_X86="0", IS_64BIT="0", DEBUG="1", APP_ABI="x86_64")
        self.assert_android(values, "0", "0", jni=True, debug=True)
        self.assertEqual(values["APP_ABI"], "x86_64")


if __name__ == "__main__":
    unittest.main(verbosity=2)
