from setuptools import setup, Extension
import pybind11

gpu_capture_module = Extension(
    "gpu_capture",
    sources=["main.cpp"],
    include_dirs=[pybind11.get_include()],
    libraries=["d3d11", "dxgi", "d2d1", "user32", "ole32", "d3dcompiler"],
    extra_compile_args=["/O2", "/std:c++17"],
    language="c++"
)

setup(
    name="gpu_capture",
    version="1.0",
    ext_modules=[gpu_capture_module],
    setup_requires=["pybind11"],
)
