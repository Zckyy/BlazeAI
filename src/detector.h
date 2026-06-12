#pragma once

#include <string>
#include <vector>
#include <onnxruntime_cxx_api.h>
#include <opencv2/opencv.hpp>

struct Detection {
    cv::Rect box;       // Bounding box in absolute screen coordinates
    float confidence;
    int classId;
    std::string label;
    int trackId = -1;   // Stable cross-frame ID from the Tracker (-1 = untracked)
    float vx = 0.0f;    // Tracker velocity estimate, px/s (0 when untracked)
    float vy = 0.0f;
};

class Detector {
public:
    Detector();
    ~Detector();

    // Scans ./models folder relative to executable for .onnx files
    static std::vector<std::wstring> ScanModelsDir();

    // Loads the selected model using ONNX Runtime. When useTensorRT is set, the TensorRT
    // execution provider is appended ahead of CUDA (FP16 optional), with an on-disk engine
    // cache so the multi-minute engine build only happens once per model/GPU/driver combo.
    bool LoadModel(const std::wstring& modelPath, bool useTensorRT = false, bool trtFp16 = true);
    bool IsLoaded() const { return m_session != nullptr; }

    // Which backend the currently loaded session actually built with (for telemetry/UI).
    const std::string& GetActiveBackend() const { return m_activeBackend; }

    // Runs inference on the CUDA device pointer, parses outputs (YOLOv5/v8 auto-detect), and runs NMS
    bool Detect(
        float* d_inputBuffer, 
        int desktopWidth, int desktopHeight, 
        int fovSize,
        float confThreshold, 
        std::vector<Detection>& outDetections
    );

    int GetInputWidth() const { return m_inputWidth; }
    int GetInputHeight() const { return m_inputHeight; }
    const std::wstring& GetCurrentModelName() const { return m_currentModelName; }

private:
    Ort::Env m_env;
    std::unique_ptr<Ort::Session> m_session;
    
    std::wstring m_currentModelName;
    std::string m_activeBackend = "None";

    std::vector<int64_t> m_inputDims;
    int m_inputWidth = 640;
    int m_inputHeight = 640;
    size_t m_inputTensorSize = 0;

    // Input/output node names owned as std::string; .c_str() is passed to ORT at Detect() time.
    std::string m_inputName;
    std::string m_outputName;

    std::vector<std::string> m_classNames;
    void LoadDefaultClassNames();
};
