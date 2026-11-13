import os
import sys
import shutil
import subprocess
from pathlib import Path
from setuptools import setup, find_packages, Extension
from setuptools.command.build_ext import build_ext


class CMakeExtension(Extension):
    def __init__(self, name: str, sourcedir: str = ""):
        super().__init__(name, sources=[])
        self.sourcedir = os.path.abspath(sourcedir)


class CMakeBuild(build_ext):
    def build_extension(self, ext: Extension) -> None:
        if not isinstance(ext, CMakeExtension):
            super().build_extension(ext)
            return

        dest_file = Path(self.get_ext_fullpath(ext.name)).resolve()
        dest_dir = dest_file.parent.resolve()
        dest_dir.mkdir(parents=True, exist_ok=True)

        # Check if pre-built binary already exists in bindings/python/purpledb
        source_pkg = Path(ext.sourcedir).resolve() / "bindings" / "python" / "purpledb"
        prebuilt_pyd = list(source_pkg.glob("_purpledb*.pyd")) + list(source_pkg.glob("_purpledb*.so"))
        if prebuilt_pyd and not os.environ.get("FORCE_REBUILD"):
            shutil.copy2(prebuilt_pyd[0], dest_file)
            # Copy runtime dlls if present
            for dll in source_pkg.glob("*.dll"):
                shutil.copy2(dll, dest_dir / dll.name)
            return

        cfg = "Debug" if self.debug else "Release"
        build_dir = Path(self.build_temp).resolve()
        build_dir.mkdir(parents=True, exist_ok=True)

        cmake_args = [
            f"-DCMAKE_BUILD_TYPE={cfg}",
            f"-DCMAKE_LIBRARY_OUTPUT_DIRECTORY={dest_dir}",
            f"-DCMAKE_RUNTIME_OUTPUT_DIRECTORY={dest_dir}",
            f"-DPython3_EXECUTABLE={sys.executable}",
            "-DBUILD_PYTHON=ON",
        ]

        build_args = ["--config", cfg]

        subprocess.check_call(["cmake", ext.sourcedir] + cmake_args, cwd=build_dir)
        subprocess.check_call(["cmake", "--build", "."] + build_args, cwd=build_dir)

        # Also copy built binaries to source_pkg for local/in-tree imports
        for ext_file in dest_dir.glob("_purpledb*"):
            try:
                shutil.copy2(ext_file, source_pkg / ext_file.name)
            except Exception:
                pass


setup(
    name="purpledb",
    version="1.0.0",
    author="PurpleDB Contributors",
    description="Embedded multimodal database for semantic video and image search",
    long_description=open("README.md", "r", encoding="utf-8").read(),
    long_description_content_type="text/markdown",
    url="https://github.com/Ameya79/PurpleDB",
    package_dir={"": "bindings/python"},
    packages=["purpledb"],
    package_data={
        "purpledb": ["*.pyi", "py.typed", "*.pyd", "*.so", "*.dll"],
    },
    include_package_data=True,
    ext_modules=[CMakeExtension("purpledb._purpledb")],
    cmdclass={"build_ext": CMakeBuild},
    python_requires=">=3.8",
    classifiers=[
        "Development Status :: 5 - Production/Stable",
        "Intended Audience :: Developers",
        "Programming Language :: Python :: 3",
        "Programming Language :: C++",
        "Topic :: Database",
        "Topic :: Multimedia :: Video",
    ],
    zip_safe=False,
)
