#pragma once

#include <d3d11.h>
#include <cuda_runtime.h>
#include <cuda_d3d11_interop.h>

class CUDAProcessor {
public:
    CUDAProcessor();
    ~CUDAProcessor();

    bool RegisterTexture(ID3D11Texture2D* texture);
    void UnregisterTexture();

    bool ProcessFrame(
        int desktopWidth, int desktopHeight,
        int fovSize,
        int outWidth, int outHeight,
        float* d_outBuffer,
        cudaStream_t stream = nullptr
    );

private:
    cudaGraphicsResource_t m_cudaResource = nullptr;
};
