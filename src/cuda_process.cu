#include "cuda_process.h"
#include <device_launch_parameters.h>
#include <iostream>
#include <chrono>

// CUDA kernel to crop, resize, convert BGRA to RGB planar, and normalize
__global__ void PreprocessKernel(
    cudaTextureObject_t texObj,
    int startX, int startY,
    int fovSize,
    int outWidth, int outHeight,
    float* outBuffer
) {
    int x = blockIdx.x * blockDim.x + threadIdx.x;
    int y = blockIdx.y * blockDim.y + threadIdx.y;

    if (x >= outWidth || y >= outHeight) return;

    // Map output coordinates back to FOV crop coordinates
    float srcX = startX + (x + 0.5f) * ((float)fovSize / outWidth);
    float srcY = startY + (y + 0.5f) * ((float)fovSize / outHeight);

    // Read texture. Since we configure cudaReadModeNormalizedFloat, 
    // it automatically normalizes uchar4 from [0, 255] to float4 in [0.0, 1.0].
    float4 pixel = tex2D<float4>(texObj, srcX, srcY);

    // DXGI_FORMAT_B8G8R8A8_UNORM maps to: x = Blue, y = Green, z = Red, w = Alpha
    float r = pixel.z;
    float g = pixel.y;
    float b = pixel.x;

    // Write to CHW planar format
    int planeSize = outWidth * outHeight;
    outBuffer[0 * planeSize + y * outWidth + x] = r;
    outBuffer[1 * planeSize + y * outWidth + x] = g;
    outBuffer[2 * planeSize + y * outWidth + x] = b;
}

CUDAProcessor::CUDAProcessor() {}

CUDAProcessor::~CUDAProcessor() {
    UnregisterTexture();
    if (m_evStart) cudaEventDestroy(m_evStart);
    if (m_evStop) cudaEventDestroy(m_evStop);
}

bool CUDAProcessor::RegisterTexture(ID3D11Texture2D* texture) {
    UnregisterTexture();

    HRESULT hr = cudaGraphicsD3D11RegisterResource(
        &m_cudaResource,
        texture,
        cudaGraphicsRegisterFlagsNone
    );

    if (hr != cudaSuccess) {
        std::cerr << "CUDA failed to register D3D11 Texture resource. Code: " << hr << "\n";
        return false;
    }

    return true;
}

void CUDAProcessor::UnregisterTexture() {
    DestroyTextureObject();
    if (m_cudaResource) {
        cudaGraphicsUnregisterResource(m_cudaResource);
        m_cudaResource = nullptr;
    }
}

void CUDAProcessor::DestroyTextureObject() {
    if (m_texObj) {
        cudaDestroyTextureObject(m_texObj);
        m_texObj = 0;
    }
    m_cachedArray = nullptr;
}

cudaTextureObject_t CUDAProcessor::GetOrCreateTextureObject(cudaArray_t array) {
    // Reuse the cached texture object as long as the mapped array is identical.
    if (m_texObj && m_cachedArray == array) {
        return m_texObj;
    }
    DestroyTextureObject();

    struct cudaResourceDesc resDesc = {};
    resDesc.resType = cudaResourceTypeArray;
    resDesc.res.array.array = array;

    struct cudaTextureDesc texDesc = {};
    texDesc.addressMode[0] = cudaAddressModeClamp;
    texDesc.addressMode[1] = cudaAddressModeClamp;
    texDesc.filterMode = cudaFilterModeLinear;       // Bilinear filtering for smooth resize
    texDesc.readMode = cudaReadModeNormalizedFloat;  // Auto normalize uchar to [0.0f, 1.0f]
    texDesc.normalizedCoords = 0;                    // Use pixel coordinates

    cudaError_t err = cudaCreateTextureObject(&m_texObj, &resDesc, &texDesc, nullptr);
    if (err != cudaSuccess) {
        std::cerr << "Failed to create texture object: " << cudaGetErrorString(err) << "\n";
        m_texObj = 0;
        return 0;
    }
    m_cachedArray = array;
    return m_texObj;
}

bool CUDAProcessor::ProcessFrame(
    int desktopWidth, int desktopHeight,
    int fovSize,
    int outWidth, int outHeight,
    float* d_outBuffer,
    cudaStream_t stream
) {
    if (!m_cudaResource) {
        return false;
    }

    auto tMap0 = std::chrono::high_resolution_clock::now();

    // Map D3D11 resource to CUDA
    cudaError_t err = cudaGraphicsMapResources(1, &m_cudaResource, stream);
    if (err != cudaSuccess) {
        std::cerr << "Failed to map graphics resource: " << cudaGetErrorString(err) << "\n";
        return false;
    }

    // Get CUDA Array from mapped resource
    cudaArray_t cuArray = nullptr;
    err = cudaGraphicsSubResourceGetMappedArray(&cuArray, m_cudaResource, 0, 0);
    if (err != cudaSuccess) {
        std::cerr << "Failed to get mapped array: " << cudaGetErrorString(err) << "\n";
        cudaGraphicsUnmapResources(1, &m_cudaResource, stream);
        return false;
    }

    // Get (or reuse) the texture object bound to this mapped array
    cudaTextureObject_t texObj = GetOrCreateTextureObject(cuArray);
    if (!texObj) {
        cudaGraphicsUnmapResources(1, &m_cudaResource, stream);
        return false;
    }

    // Map (incl. getting the mapped array) is done; record its cost.
    auto tMap1 = std::chrono::high_resolution_clock::now();
    m_lastMapMs = std::chrono::duration<float, std::milli>(tMap1 - tMap0).count();

    // Calculate crop boundaries centered on the screen
    int startX = (desktopWidth - fovSize) / 2;
    int startY = (desktopHeight - fovSize) / 2;

    // Run GPU preprocessing kernel
    dim3 block(16, 16);
    dim3 grid((outWidth + block.x - 1) / block.x, (outHeight + block.y - 1) / block.y);

    // Bracket the kernel with CUDA events to measure its true on-GPU execution time,
    // independent of how long the CPU blocks in the sync below (which includes queue wait).
    if (!m_evStart) {
        cudaEventCreate(&m_evStart);
        cudaEventCreate(&m_evStop);
    }
    cudaEventRecord(m_evStart, stream);

    PreprocessKernel<<<grid, block, 0, stream>>>(
        texObj,
        startX, startY,
        fovSize,
        outWidth, outHeight,
        d_outBuffer
    );

    cudaEventRecord(m_evStop, stream);

    // Synchronize to check for kernel errors
    err = cudaStreamSynchronize(stream);
    if (err != cudaSuccess) {
        std::cerr << "Kernel launch failed: " << cudaGetErrorString(err) << "\n";
    }

    cudaEventElapsedTime(&m_lastKernelGpuMs, m_evStart, m_evStop);

    // Kernel launch + sync is done; record its cost (this is the GPU compute portion).
    auto tKernel1 = std::chrono::high_resolution_clock::now();
    m_lastKernelMs = std::chrono::duration<float, std::milli>(tKernel1 - tMap1).count();

    // Unmap the graphics resource. The texture object is cached and reused next frame
    // (it stays valid because the registered D3D texture is not recreated per frame).
    cudaGraphicsUnmapResources(1, &m_cudaResource, stream);

    auto tUnmap1 = std::chrono::high_resolution_clock::now();
    m_lastUnmapMs = std::chrono::duration<float, std::milli>(tUnmap1 - tKernel1).count();

    return (err == cudaSuccess);
}
