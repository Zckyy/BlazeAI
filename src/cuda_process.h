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

    // Sub-stage timings (ms) from the most recent ProcessFrame, for diagnosing where the
    // per-frame preprocess cost actually lives (driver map/unmap vs. kernel+sync).
    float m_lastMapMs = 0.0f;
    float m_lastKernelMs = 0.0f;     // CPU wall-time of kernel launch + cudaStreamSynchronize
    float m_lastUnmapMs = 0.0f;
    float m_lastKernelGpuMs = 0.0f;  // True GPU execution time of the kernel (via CUDA events)

private:
    // Returns a CUDA texture object for the given array, reusing the cached one when the
    // underlying array has not changed (avoids per-frame create/destroy overhead).
    cudaTextureObject_t GetOrCreateTextureObject(cudaArray_t array);
    void DestroyTextureObject();

    cudaGraphicsResource_t m_cudaResource = nullptr;
    cudaTextureObject_t m_texObj = 0;
    cudaArray_t m_cachedArray = nullptr;

    // Lazily-created event pair used to measure true on-GPU kernel time.
    cudaEvent_t m_evStart = nullptr;
    cudaEvent_t m_evStop = nullptr;
};
