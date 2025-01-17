# Copyright Contributors to the OpenVDB Project
# SPDX-License-Identifier: Apache-2.0
#
import logging
import os
import re
import shutil
import subprocess
import tarfile
from pathlib import Path
from typing import List, Tuple

import git
import git.repo
import requests
from git.exc import GitCommandError, InvalidGitRepositoryError
from setuptools import setup
from torch.utils import cpp_extension
from tqdm import tqdm

is_conda_env = "CONDA_PREFIX" in os.environ
if is_conda_env:
    os.environ["CXX"] = "x86_64-conda-linux-gnu-g++"
    os.environ["NVCC_CCBIN"] = "x86_64-conda-linux-gnu-gcc"


def get_nanovdb_source_dir() -> str:
    nanovdb_source_dir = "../nanovdb"
    if not os.path.exists(nanovdb_source_dir):
        nanovdb_source_dir = "external/openvdb/nanovdb"
    return nanovdb_source_dir


def get_cwd() -> Path:
    return Path(__file__).resolve().parent


def get_external_dir() -> Path:
    based = get_cwd()
    external_path = based / "external"
    if not external_path.exists():
        external_path.mkdir()
    elif not external_path.is_dir():
        raise RuntimeError(f"External path {external_path} exists but is not a directory")
    return external_path


class FVDBBuildCommand(cpp_extension.BuildExtension):

    @staticmethod
    def is_git_repo(repo_path: str | Path) -> bool:
        is_repo = False
        try:
            _ = git.repo.Repo(repo_path)
            is_repo = True
        except InvalidGitRepositoryError:
            is_repo = False

        return is_repo

    @staticmethod
    def download_external_dep(
        name: str, git_url: str, git_tag: str, recursive: bool = False
    ) -> Tuple[Path, git.repo.Repo]:
        external_path = get_external_dir()

        repo_path = external_path / name
        if repo_path.exists() and repo_path.is_dir():
            if FVDBBuildCommand.is_git_repo(repo_path):
                repo = git.repo.Repo(repo_path)
                repo.git.checkout(git_tag)
            else:
                raise ValueError(f"A path {repo_path} exists but is not a git repo")
        else:
            print(f"Cloning {git_url}:{git_tag} into {repo_path}")
            if recursive:
                repo = git.repo.Repo.clone_from(git_url, repo_path, multi_options=["--recursive"])
            else:
                repo = git.repo.Repo.clone_from(git_url, repo_path)
            repo.git.checkout(git_tag)

        return repo_path, repo

    @staticmethod
    def build_cmake_project(base_path, cmake_args, parallel_jobs: int = 1):
        cmake_build_dir = os.path.join(base_path, "build")
        cmake_install_dir = os.path.join(base_path, "install")
        os.makedirs(cmake_build_dir, exist_ok=True)
        os.makedirs(cmake_install_dir, exist_ok=True)
        subprocess.check_call(
            ["cmake", base_path, f"-DCMAKE_INSTALL_PREFIX={cmake_install_dir}", "-DCMAKE_INSTALL_LIBDIR=lib"]
            + cmake_args,
            cwd=cmake_build_dir,
        )
        subprocess.check_call(
            ["cmake", "--build", ".", "--target", "install", f"-j{parallel_jobs}"], cwd=cmake_build_dir
        )
        return cmake_install_dir

    def build_extension(self, _ext) -> None:
        path = os.path.join(self.build_lib, "fvdb")

        if _ext.name == "fvdb._Cpp":
            _ext.library_dirs.append(path)

        super().build_extension(_ext)

        if _ext.name == "fvdb.fvdblib":
            if os.path.exists(os.path.join(path, "libfvdb.so")):
                os.remove(os.path.join(path, "libfvdb.so"))

            # Find the .so file in the fvdb subdirectory of self.build_lib
            # assert that there is only a single one.
            so_files = [
                os.path.join(path, t) for t in os.listdir(path) if t.endswith(".so") and t.startswith("fvdblib")
            ]
            assert len(so_files) == 1

            # Copy the file in so_files[0] to lib/libfvdb.so
            shutil.copy(so_files[0], os.path.join(path, "libfvdb.so"))

            # Also copy the file to the appropriate directory if installing inplace
            if self.old_inplace:
                build_py = self.get_finalized_command("build_py")
                inplace_file, regular_file = self._get_inplace_equivalent(build_py, _ext)  # type: ignore
                inplace_file = os.path.join(os.path.dirname(inplace_file), "libfvdb.so")
                regular_file = os.path.join(os.path.dirname(regular_file), "libfvdb.so")
                self.copy_file(regular_file, inplace_file, level=self.verbose)  # type: ignore

    def run(self) -> None:
        # A sibling nanovdb source directory will exist if fvdb is being built as part of OpenVDB
        sibling_nanovdb_dir = Path("../nanovdb")
        if not sibling_nanovdb_dir.exists():
            openvdb_url = "https://github.com/kmuseth/openvdb.git"
            self.download_external_dep(name="openvdb", git_url=openvdb_url, git_tag="feature/nanovdb_v32.7")

        _, cutlass_repo = self.download_external_dep(
            name="cutlass", git_url="https://github.com/NVIDIA/cutlass.git", git_tag="v3.4.0"
        )
        try:
            # NOTE:  In python <=3.8, __file__ will be a relative path and >3.8 it is an absolute path
            cutlass_repo.git.apply(Path(__file__).resolve().parent / "env" / "cutlass.patch")
        except GitCommandError as e:
            logging.info(f"Failed to apply cutlass patch: {str(e)}, continuing without patching")

        self.download_external_dep(
            name="cudnn_fe", git_url="https://github.com/NVIDIA/cudnn-frontend", git_tag="v1.3.0"
        )

        blosc_source_dir, _ = self.download_external_dep(
            name="c-blosc", git_url="https://github.com/Blosc/c-blosc.git", git_tag="v1.21.4"
        )
        self.build_cmake_project(
            blosc_source_dir,
            [
                "-DBUILD_SHARED=OFF",
                "-DBUILD_TESTS=OFF",
                "-DBUILD_FUZZERS=OFF",
                "-DBUILD_BENCHMARKS=OFF",
                "-DCMAKE_POSITION_INDEPENDENT_CODE=ON",
            ],
        )

        self.download_external_dep(name="glm", git_url="https://github.com/g-truc/glm.git", git_tag="1.0.1")

        self.old_inplace = self.inplace
        super().run()

        # Find all the headers and copy them into the build directory.
        # This way extension modules of FVDB can include them.
        fvdb_headers = get_header_files_recursive("src", "fvdb")
        nanovdb_headers = get_header_files_recursive(get_nanovdb_source_dir(), "nanovdb")

        for header_folder, header_files in fvdb_headers + nanovdb_headers:
            os.makedirs(os.path.join(self.build_lib, header_folder), exist_ok=True)

            for header_file in header_files:
                shutil.copy(header_file, os.path.join(self.build_lib, header_folder))


def get_source_files_recursive(base_path, include_bindings=True) -> List[str]:
    source_files = []
    for dir_name, _, dir_files in os.walk(base_path):
        if not include_bindings and os.path.basename(dir_name) == "python":
            continue
        cpp_files = [os.path.join(dir_name, t) for t in dir_files if t.endswith(".cpp")]
        cu_files = [os.path.join(dir_name, t) for t in dir_files if t.endswith(".cu")]
        source_files += cpp_files + cu_files
    return source_files


def get_header_files_recursive(base_path, new_path) -> List[Tuple[str, List[str]]]:
    base_len = len(base_path.split("/"))
    source_files = []
    for dir_name, _, dir_files in os.walk(base_path):
        header_files = [os.path.join(dir_name, t) for t in dir_files if t.endswith(".h") or t.endswith(".cuh")]
        header_folder = [os.path.join("fvdb/include", new_path, *(h.split("/")[base_len:-1])) for h in header_files]

        # All items of header_folder should be the same
        if len(header_folder) > 0:
            source_files.append((header_folder[0], header_files))

    return source_files


def download_and_install_cudnn() -> Tuple[List[str], List[str]]:
    url = (
        "https://developer.download.nvidia.com/compute/cudnn/redist/cudnn/linux-x86_64/"
        + "cudnn-linux-x86_64-9.1.0.70_cuda12-archive.tar.xz"
    )
    external_dir = get_external_dir()
    tar_filepath = external_dir / "cudnn.tar.xz"
    folder_filepath = external_dir / "cudnn"

    logging.info("CUDNN path: %s" % folder_filepath.resolve())
    if not tar_filepath.exists():
        response = requests.get(url, stream=True)

        # Sizes in bytes.
        total_size = int(response.headers.get("content-length", 0))
        block_size = 1024

        with tqdm(total=total_size, unit="B", unit_scale=True, desc="Downloading cudnn") as progress_bar:
            with open(tar_filepath, "wb") as file:
                for data in response.iter_content(block_size):
                    progress_bar.update(len(data))
                    file.write(data)

        if total_size != 0 and progress_bar.n != total_size:
            raise RuntimeError("Could not download cudnn")

        # Check hash of cudnn.tar.xz
        cudnn_hash = "105585f6dd62cc013354d81a2a692d2e"
        cudnn_hash_cmd = f"md5sum {tar_filepath}"
        cudnn_hash_output = subprocess.check_output(cudnn_hash_cmd, shell=True).decode().split()[0]
        if cudnn_hash != cudnn_hash_output:
            raise RuntimeError("Hash of cudnn.tar.xz does not match")

    if not folder_filepath.exists():
        logging.info("Extracting cudnn…")
        with tarfile.open(tar_filepath, "r:xz") as tar:
            tar.extractall(folder_filepath)

    # Find include directories and all static libraries
    cudnn_include = None
    cudnn_libs = []
    # NOTE: Similar Path.walk() is only available in Python 3.12+
    for dir_name, _, dir_files in os.walk(str(folder_filepath)):
        if cudnn_include is None and "include" in dir_name:
            cudnn_include = dir_name
        if "lib" in dir_name:
            cudnn_libs += [os.path.join(dir_name, t) for t in dir_files if t.endswith(".a")]

    # Remove symbolic links
    cudnn_libs = [t for t in cudnn_libs if not os.path.islink(t)]

    if cudnn_include is None or len(cudnn_libs) == 0:
        raise RuntimeError("Could not find cudnn include or libraries")

    return [cudnn_include], cudnn_libs


if __name__ == "__main__":
    # check we will be compiling for a supported compute architecture
    for arch_flag in cpp_extension._get_cuda_arch_flags():
        match = re.search(r"code=sm_(\d+)", arch_flag)
        if match:
            if int(match.group(1)) < 80:
                raise RuntimeError("ƒVDB requires a minimum compute capability of 8.0 (Ampere)")

    external_dir = get_external_dir()

    # Use new C++ standard for newer NVCC versions
    cuda_home = cpp_extension.CUDA_HOME
    cuda_version = None
    if cuda_home is not None:
        cuda_version_str = subprocess.check_output([cuda_home + "/bin/nvcc", "--version"]).strip().decode()
        cuda_version = re.search(r"release (\d+[.]\d+)", cuda_version_str)
        if cuda_version is not None:
            cuda_version = cuda_version.group(1)

    if cuda_version is not None and int(cuda_version.split(".")[0]) >= 12:
        cpp_std = "c++20"
        cudnn_include_dirs, cudnn_static_libs = download_and_install_cudnn()
    else:
        cpp_std = "c++17"
        cudnn_include_dirs, cudnn_static_libs = [], []

    cpp_flags = [
        f"-std={cpp_std}",
        "-Wno-unknown-pragmas",
        "-Wno-class-memaccess",
        "-fdiagnostics-color=always",
        "-DNANOVDB_USE_BLOSC",
    ]
    nvcc_flags = [
        f"-std={cpp_std}",
        "--extended-lambda",
        "--diag-suppress=186",
        "-diag-suppress=3189",
        "-Xfatbin",
        "-compress-all",
    ]
    user_nvcc_flags = os.getenv("NVCC_FLAGS", "").split()
    nvcc_flags += user_nvcc_flags

    cwd = get_cwd()
    lib_ext = cpp_extension.CUDAExtension(
        name="fvdb.fvdblib",
        sources=get_source_files_recursive("src", include_bindings=False),
        include_dirs=[
            cwd / "src",
            cwd / get_nanovdb_source_dir(),
            cwd / "external/cutlass/include",
            cwd / "external/c-blosc/install/include",
            cwd / "external/cudnn_fe/include",
            cwd / "external/glm",
        ]
        + cudnn_include_dirs,
        extra_objects=[
            "external/c-blosc/install/lib/libblosc.a",
        ]
        + cudnn_static_libs,
        extra_compile_args={"cxx": cpp_flags + ["-fvisibility=default"], "nvcc": nvcc_flags},
        language="c++",
    )

    bind_ext = cpp_extension.CUDAExtension(
        name="fvdb._Cpp",
        sources=get_source_files_recursive("src/python/"),
        include_dirs=[
            cwd / "src",
            cwd / get_nanovdb_source_dir(),
            cwd / "external/cutlass/include",
            cwd / "external/c-blosc/install/include",
            cwd / "external/glm",
        ],
        library_dirs=[str(cwd / "fvdb")],
        libraries=["fvdb"],
        extra_link_args=["-Wl,-rpath,$ORIGIN"],
        extra_compile_args={"cxx": cpp_flags + ["-fvisibility=hidden"], "nvcc": nvcc_flags},
        language="c++",
    )

    def retrieve_version(file_path: Path = get_cwd() / "fvdb/__init__.py") -> str:
        with open(file_path, "r") as f:
            for line in f:
                if line.startswith("__version__"):
                    return line.split("=")[1].strip().strip("'").strip('"')
        return "0.0.0"

    setup(
        name="fvdb",
        version=retrieve_version(),
        ext_modules=[lib_ext, bind_ext],
        packages=[
            "fvdb",
            "fvdb.nn",
            "fvdb.utils",
            "fvdb.utils.examples",
            "fvdb.utils.tests",
            "fvdb.utils.data",
            "fvdb.utils.data._colmap_utils",
        ],
        include_package_data=True,
        package_data={"fvdb": ["_Cpp.pyi", "py.typed"]},
        cmdclass={"build_ext": FVDBBuildCommand},
    )
