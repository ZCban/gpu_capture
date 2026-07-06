# gpu_capture
High-performance screen capture module for Windows using DirectX 11 and DXGI Desktop Duplication. Exposes frames directly as numpy arrays (CPU) or torch tensors (CUDA) ready for inference with YOLO/TensorRT/DirectML.

## Features

- DXGI Desktop Duplication 
- D3D11 Compute Shader — crop, channel swap and CHW layout conversion on GPU (FP32)
- **CPU path** — returns `numpy array [1, 3, H, W] float32` ready for ONNX/standard inference
- **CUDA path** — returns `torch.Tensor` on VRAM, zero CPU involvement, ready for TensorRT
- Pre-allocated buffers — zero allocations per frame

---

## Requirements

### Common
- Windows 10/11
- Python 3.10 / 3.11 (recommended)
- Visual Studio 2022 with C++ build tools
- pybind11
- CUDATOOLKIT
