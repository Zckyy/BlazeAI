#pragma once

#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <memory>
#include <opencv2/opencv.hpp>

class DXGICapture {
public:
    DXGICapture(ID3D11Device* device, ID3D11DeviceContext* context);
    ~DXGICapture();

    bool Init();
    bool CaptureFrame(Microsoft::WRL::ComPtr<ID3D11Texture2D>& outTexture);
    void ReleaseFrame();
    
    // Captures a cropped FOV region from the srcTexture directly to an OpenCV Mat
    bool CaptureFovToMat(Microsoft::WRL::ComPtr<ID3D11Texture2D>& srcTexture, int fovSize, cv::Mat& outMat);

    // Captures the full screen frame from srcTexture to an OpenCV Mat
    bool CaptureFullToMat(Microsoft::WRL::ComPtr<ID3D11Texture2D>& srcTexture, cv::Mat& outMat);

    int GetWidth() const { return m_width; }
    int GetHeight() const { return m_height; }

private:
    ID3D11Device* m_device = nullptr;
    ID3D11DeviceContext* m_context = nullptr;

    Microsoft::WRL::ComPtr<IDXGIOutputDuplication> m_deskDupl;
    DXGI_OUTDUPL_DESC m_duplDesc = {};
    
    int m_width = 0;
    int m_height = 0;
    bool m_frameAcquired = false;

    // Staging texture cache for CPU-based FOV extraction
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_stagingTexture;
    int m_stagingFovSize = 0;

    bool ResetDuplication();
};
