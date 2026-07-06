from setuptools import setup, Extension
import pybind11

ext_modules = [
    Extension(
        'gpu_capture',
        ['main.cpp'],
        include_dirs=[pybind11.get_include()],
        language='c++',
        extra_compile_args=['/O2', '/std:c++17'], 
        libraries=['ole32', 'user32', 'd3d11', 'dxgi', 'd2d1', 'dxguid']
    ),
]

setup(
    name='gpu_capture',
    ext_modules=ext_modules,
)
