#include "capture.h"
#include <iostream>

DXGICapture::DXGICapture(ID3D11Device* device, ID3D11DeviceContext* context)
    : m_device(device), m_context(context) {}

DXGICapture::~DXGICapture() {
    ReleaseFrame();
}

bool DXGICapture::Init() {
    return ResetDuplication();
}

bool DXGICapture::ResetDuplication() {
    m_deskDupl.Reset();
    m_frameAcquired = false;

    // Get DXGI Device from D3D11 Device
    Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
    if (FAILED(m_device->QueryInterface(IID_PPV_ARGS(&dxgiDevice)))) {
        std::cerr << "Failed to query IDXGIDevice from D3D11 Device\n";
        return false;
    }

    // Get DXGI Adapter
    Microsoft::WRL::ComPtr<IDXGIAdapter> dxgiAdapter;
    if (FAILED(dxgiDevice->GetParent(IID_PPV_ARGS(&dxgiAdapter)))) {
        std::cerr << "Failed to get IDXGIAdapter\n";
        return false;
    }

    // Get primary output (Monitor 0)
    Microsoft::WRL::ComPtr<IDXGIOutput> dxgiOutput;
    if (FAILED(dxgiAdapter->EnumOutputs(0, &dxgiOutput))) {
        std::cerr << "Failed to enumerate output 0\n";
        return false;
    }

    // Query IDXGIOutput1 (required for desktop duplication)
    Microsoft::WRL::ComPtr<IDXGIOutput1> dxgiOutput1;
    if (FAILED(dxgiOutput.As(&dxgiOutput1))) {
        std::cerr << "Failed to cast IDXGIOutput to IDXGIOutput1\n";
        return false;
    }

    // Create desktop duplication
    HRESULT hr = dxgiOutput1->DuplicateOutput(m_device, &m_deskDupl);
    if (FAILED(hr)) {
        std::cerr << "Failed to create DuplicateOutput. HR = " << std::hex << hr << "\n";
        return false;
    }

    m_deskDupl->GetDesc(&m_duplDesc);
    m_width = m_duplDesc.ModeDesc.Width;
    m_height = m_duplDesc.ModeDesc.Height;

    std::cout << "DXGI Capture Initialized: " << m_width << "x" << m_height << "\n";
    return true;
}

bool DXGICapture::CaptureFrame(Microsoft::WRL::ComPtr<ID3D11Texture2D>& outTexture) {
    if (!m_deskDupl) {
        if (!ResetDuplication()) return false;
    }

    ReleaseFrame();

    DXGI_OUTDUPL_FRAME_INFO frameInfo;
    Microsoft::WRL::ComPtr<IDXGIResource> desktopResource;
    
    // Acquire frame instantly (timeout = 0)
    HRESULT hr = m_deskDupl->AcquireNextFrame(0, &frameInfo, &desktopResource);
    
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
        return false; // No new frame yet
    }

    if (FAILED(hr)) {
        // Device lost or mode change - reset duplication interface next frame
        ResetDuplication();
        return false;
    }

    m_frameAcquired = true;

    // Retrieve ID3D11Texture2D
    Microsoft::WRL::ComPtr<ID3D11Texture2D> acquiredTexture;
    if (FAILED(desktopResource.As(&acquiredTexture))) {
        ReleaseFrame();
        return false;
    }

    // We copy the frame to outTexture immediately so we can ReleaseFrame()
    // and not hold the lock longer than needed.
    D3D11_TEXTURE2D_DESC desc;
    acquiredTexture->GetDesc(&desc);

    // Ensure outTexture matches format and dimensions
    if (!outTexture) {
        D3D11_TEXTURE2D_DESC copyDesc = desc;
        copyDesc.Usage = D3D11_USAGE_DEFAULT;
        copyDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        copyDesc.CPUAccessFlags = 0;
        copyDesc.MiscFlags = 0;
        if (FAILED(m_device->CreateTexture2D(&copyDesc, nullptr, &outTexture))) {
            ReleaseFrame();
            return false;
        }
    }

    // Copy texture from desktop duplication to our return texture
    m_context->CopyResource(outTexture.Get(), acquiredTexture.Get());
    m_context->Flush();

    // Release the DXGI lock immediately
    ReleaseFrame();

    return true;
}

void DXGICapture::ReleaseFrame() {
    if (m_frameAcquired && m_deskDupl) {
        m_deskDupl->ReleaseFrame();
        m_frameAcquired = false;
    }
}

bool DXGICapture::CaptureFovToMat(Microsoft::WRL::ComPtr<ID3D11Texture2D>& srcTexture, int fovSize, cv::Mat& outMat) {
    if (!srcTexture) return false;

    // 1. Create or recreate staging texture if fovSize changed
    if (!m_stagingTexture || m_stagingFovSize != fovSize) {
        D3D11_TEXTURE2D_DESC srcDesc;
        srcTexture->GetDesc(&srcDesc);

        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = fovSize;
        desc.Height = fovSize;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = srcDesc.Format;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        desc.MiscFlags = 0;

        HRESULT hr = m_device->CreateTexture2D(&desc, nullptr, &m_stagingTexture);
        if (FAILED(hr)) {
            std::cerr << "Failed to create staging texture. HR = 0x" << std::hex << hr << "\n";
            return false;
        }
        m_stagingFovSize = fovSize;
    }

    // 2. Copy the center subresource region from srcTexture to m_stagingTexture
    int startX = (m_width - fovSize) / 2;
    int startY = (m_height - fovSize) / 2;

    // Clamp boundary just in case
    if (startX < 0) startX = 0;
    if (startY < 0) startY = 0;
    if (startX + fovSize > m_width) fovSize = m_width - startX;
    if (startY + fovSize > m_height) fovSize = m_height - startY;

    D3D11_BOX srcBox;
    srcBox.left = startX;
    srcBox.right = startX + fovSize;
    srcBox.top = startY;
    srcBox.bottom = startY + fovSize;
    srcBox.front = 0;
    srcBox.back = 1;

    m_context->CopySubresourceRegion(
        m_stagingTexture.Get(), 0,
        0, 0, 0,
        srcTexture.Get(), 0,
        &srcBox
    );
    m_context->Flush();

    // 3. Map staging texture and read data to cv::Mat
    D3D11_MAPPED_SUBRESOURCE mapped;
    HRESULT hr = m_context->Map(m_stagingTexture.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) {
        std::cerr << "Failed to map staging texture. HR = 0x" << std::hex << hr << "\n";
        return false;
    }

    // Format is BGRA (8-bit, 4 channels), wrap it
    cv::Mat tempMat(fovSize, fovSize, CV_8UC4, mapped.pData, mapped.RowPitch);
    outMat = tempMat.clone(); // Clone to make a deep copy before we unmap

    m_context->Unmap(m_stagingTexture.Get(), 0);
    return true;
}

bool DXGICapture::CaptureFullToMat(Microsoft::WRL::ComPtr<ID3D11Texture2D>& srcTexture, cv::Mat& outMat) {
    if (!srcTexture) return false;

    D3D11_TEXTURE2D_DESC srcDesc;
    srcTexture->GetDesc(&srcDesc);

    // Create a temporary staging texture for full screen copying
    Microsoft::WRL::ComPtr<ID3D11Texture2D> fullStaging;
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = srcDesc.Width;
    desc.Height = srcDesc.Height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = srcDesc.Format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;

    HRESULT hr = m_device->CreateTexture2D(&desc, nullptr, &fullStaging);
    if (FAILED(hr)) {
        std::cerr << "Failed to create full staging texture. HR = 0x" << std::hex << hr << "\n";
        return false;
    }

    m_context->CopyResource(fullStaging.Get(), srcTexture.Get());
    m_context->Flush();

    D3D11_MAPPED_SUBRESOURCE mapped;
    hr = m_context->Map(fullStaging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) {
        std::cerr << "Failed to map full staging texture. HR = 0x" << std::hex << hr << "\n";
        return false;
    }

    cv::Mat tempMat(srcDesc.Height, srcDesc.Width, CV_8UC4, mapped.pData, mapped.RowPitch);
    outMat = tempMat.clone(); // deep copy

    m_context->Unmap(fullStaging.Get(), 0);
    return true;
}
