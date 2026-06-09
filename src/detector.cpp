#include "detector.h"
#include <iostream>
#include <filesystem>
#include <windows.h>

Detector::Detector() {
    LoadDefaultClassNames();
}

Detector::~Detector() = default;

std::vector<std::wstring> Detector::ScanModelsDir() {
    std::vector<std::wstring> models;
    
    // Get path of the running executable
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(NULL, exePath, MAX_PATH);
    std::filesystem::path path(exePath);
    std::filesystem::path modelsDir = path.parent_path() / "models";

    std::wcout << L"Scanning models directory: " << modelsDir.wstring() << std::endl;

    if (!std::filesystem::exists(modelsDir)) {
        std::filesystem::create_directories(modelsDir);
        return models;
    }

    for (const auto& entry : std::filesystem::directory_iterator(modelsDir)) {
        if (entry.is_regular_file() && entry.path().extension() == ".onnx") {
            models.push_back(entry.path().wstring());
        }
    }
    return models;
}

bool Detector::LoadModel(const std::wstring& modelPath) {
    try {
        m_session.reset();

        Ort::SessionOptions sessionOptions;
        sessionOptions.SetIntraOpNumThreads(1);
        sessionOptions.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);

        // Try to enable CUDA EP
        try {
            OrtCUDAProviderOptions cudaOptions;
            cudaOptions.device_id = 0;
            cudaOptions.gpu_mem_limit = 2ULL * 1024 * 1024 * 1024; // 2GB limit or similar
            cudaOptions.arena_extend_strategy = 0;
            cudaOptions.cudnn_conv_algo_search = OrtCudnnConvAlgoSearchExhaustive;
            cudaOptions.do_copy_in_default_stream = 1;
            sessionOptions.AppendExecutionProvider_CUDA(cudaOptions);
            std::cout << "Successfully enabled CUDA Execution Provider for ONNX Runtime.\n";
        }
        catch (const std::exception& e) {
            std::cerr << "Failed to enable CUDA EP: " << e.what() << ". Falling back to CPU.\n";
        }

        m_session = std::make_unique<Ort::Session>(m_env, modelPath.c_str(), sessionOptions);
        
        std::filesystem::path p(modelPath);
        m_currentModelName = p.filename().wstring();

        // Query node info
        Ort::AllocatorWithDefaultOptions allocator;
        
        // Input details
        size_t numInputNodes = m_session->GetInputCount();
        if (numInputNodes > 0) {
            m_inputName = m_session->GetInputNameAllocated(0, allocator).get();

            Ort::TypeInfo typeInfo = m_session->GetInputTypeInfo(0);
            auto tensorInfo = typeInfo.GetTensorTypeAndShapeInfo();
            m_inputDims = tensorInfo.GetShape();
            
            // YOLO input is [1, 3, height, width]
            m_inputHeight = (int)m_inputDims[2];
            m_inputWidth = (int)m_inputDims[3];
            m_inputTensorSize = 1 * 3 * m_inputHeight * m_inputWidth;
        }

        // Output details
        size_t numOutputNodes = m_session->GetOutputCount();
        if (numOutputNodes > 0) {
            m_outputName = m_session->GetOutputNameAllocated(0, allocator).get();
        }

        std::wcout << L"Successfully loaded model: " << m_currentModelName 
                  << L" (Input: " << m_inputWidth << L"x" << m_inputHeight << L")\n";
        return true;
    }
    catch (const Ort::Exception& oe) {
        std::cerr << "ONNX Runtime Exception: " << oe.what() << "\n";
        return false;
    }
    catch (const std::exception& e) {
        std::cerr << "Standard exception loading model: " << e.what() << "\n";
        return false;
    }
}

bool Detector::Detect(
    float* d_inputBuffer, 
    int desktopWidth, int desktopHeight, 
    int fovSize,
    float confThreshold, 
    std::vector<Detection>& outDetections
) {
    if (!m_session) return false;

    outDetections.clear();

    try {
        // Map the existing CUDA device pointer directly to the ONNX Runtime input tensor
        Ort::MemoryInfo memInfo("Cuda", OrtAllocatorType::OrtDeviceAllocator, 0, OrtMemTypeDefault);
        
        Ort::Value inputTensor = Ort::Value::CreateTensor<float>(
            memInfo,
            d_inputBuffer,
            m_inputTensorSize,
            m_inputDims.data(),
            m_inputDims.size()
        );

        const char* inputNames[] = { m_inputName.c_str() };
        const char* outputNames[] = { m_outputName.c_str() };

        // Run inference
        auto outputTensors = m_session->Run(
            Ort::RunOptions{nullptr},
            inputNames,
            &inputTensor,
            1,
            outputNames,
            1
        );

        if (outputTensors.empty()) return false;

        // Extract output tensor details (transfers data to CPU for postprocessing/NMS)
        float* rawOutput = outputTensors[0].GetTensorMutableData<float>();
        auto typeInfo = outputTensors[0].GetTensorTypeAndShapeInfo();
        auto shape = typeInfo.GetShape();

        // Auto-detect YOLO format based on shape:
        // YOLOv5/v7: [1, num_anchors, elements] where elements = 5 + classes (usually 85)
        // YOLOv8: [1, elements, num_anchors] where elements = 4 + classes (usually 84)
        
        std::vector<cv::Rect> bboxes;
        std::vector<float> confidences;
        std::vector<int> classIds;

        int cropStartX = (desktopWidth - fovSize) / 2;
        int cropStartY = (desktopHeight - fovSize) / 2;

        if (shape.size() == 3) {
            int dim1 = (int)shape[1];
            int dim2 = (int)shape[2];

            if (dim1 < dim2) {
                // --- YOLOv8 Format: [1, elements, num_anchors] ---
                int numElements = dim1; // e.g., 84
                int numAnchors = dim2;  // e.g., 8400
                int numClasses = numElements - 4;

                for (int col = 0; col < numAnchors; ++col) {
                    // Find max class confidence
                    float maxClassScore = 0.0f;
                    int classId = -1;
                    for (int c = 0; c < numClasses; ++c) {
                        float score = rawOutput[(4 + c) * numAnchors + col];
                        if (score > maxClassScore) {
                            maxClassScore = score;
                            classId = c;
                        }
                    }

                    if (maxClassScore > confThreshold) {
                        float cx = rawOutput[0 * numAnchors + col];
                        float cy = rawOutput[1 * numAnchors + col];
                        float w  = rawOutput[2 * numAnchors + col];
                        float h  = rawOutput[3 * numAnchors + col];

                        // Convert center format to left/top
                        float left = cx - w / 2.0f;
                        float top  = cy - h / 2.0f;

                        // Map from network coords [0, inputSize] to FOV crop coords [0, fovSize]
                        float fovX = left * ((float)fovSize / m_inputWidth);
                        float fovY = top * ((float)fovSize / m_inputHeight);
                        float fovW = w * ((float)fovSize / m_inputWidth);
                        float fovH = h * ((float)fovSize / m_inputHeight);

                        // Map to absolute screen coords
                        int screenX = static_cast<int>(fovX + cropStartX);
                        int screenY = static_cast<int>(fovY + cropStartY);
                        int screenW = static_cast<int>(fovW);
                        int screenH = static_cast<int>(fovH);

                        bboxes.push_back(cv::Rect(screenX, screenY, screenW, screenH));
                        confidences.push_back(maxClassScore);
                        classIds.push_back(classId);
                    }
                }
            }
            else {
                // --- YOLOv5 Format: [1, num_anchors, elements] ---
                int numAnchors = dim1;  // e.g., 25200
                int numElements = dim2; // e.g., 85
                int numClasses = numElements - 5;

                for (int row = 0; row < numAnchors; ++row) {
                    float* rowData = rawOutput + (row * numElements);
                    float boxConfidence = rowData[4];

                    if (boxConfidence > confThreshold) {
                        // Find max class score
                        float maxClassScore = 0.0f;
                        int classId = -1;
                        for (int c = 0; c < numClasses; ++c) {
                            float score = rowData[5 + c];
                            if (score > maxClassScore) {
                                maxClassScore = score;
                                classId = c;
                            }
                        }

                        float confidence = boxConfidence * maxClassScore;
                        if (confidence > confThreshold) {
                            float cx = rowData[0];
                            float cy = rowData[1];
                            float w  = rowData[2];
                            float h  = rowData[3];

                            float left = cx - w / 2.0f;
                            float top  = cy - h / 2.0f;

                            // Scale to FOV coords
                            float fovX = left * ((float)fovSize / m_inputWidth);
                            float fovY = top * ((float)fovSize / m_inputHeight);
                            float fovW = w * ((float)fovSize / m_inputWidth);
                            float fovH = h * ((float)fovSize / m_inputHeight);

                            // Map to absolute screen coords
                            int screenX = static_cast<int>(fovX + cropStartX);
                            int screenY = static_cast<int>(fovY + cropStartY);
                            int screenW = static_cast<int>(fovW);
                            int screenH = static_cast<int>(fovH);

                            bboxes.push_back(cv::Rect(screenX, screenY, screenW, screenH));
                            confidences.push_back(confidence);
                            classIds.push_back(classId);
                        }
                    }
                }
            }
        }

        // Run standard NMS on CPU (efficient since boxes are heavily pre-filtered by confidence)
        std::vector<int> indices;
        cv::dnn::NMSBoxes(bboxes, confidences, confThreshold, 0.45f, indices);

        for (int idx : indices) {
            Detection det;
            det.box = bboxes[idx];
            det.confidence = confidences[idx];
            det.classId = classIds[idx];
            if (det.classId >= 0 && det.classId < m_classNames.size()) {
                det.label = m_classNames[det.classId];
            } else {
                det.label = "object";
            }
            outDetections.push_back(det);
        }

        return true;
    }
    catch (const Ort::Exception& oe) {
        std::cerr << "Inference ONNX runtime exception: " << oe.what() << "\n";
        return false;
    }
}

void Detector::LoadDefaultClassNames() {
    // Default COCO class names (80 classes)
    m_classNames = {
        "person", "bicycle", "car", "motorcycle", "airplane", "bus", "train", "truck", "boat", "traffic light",
        "fire hydrant", "stop sign", "parking meter", "bench", "bird", "cat", "dog", "horse", "sheep", "cow",
        "elephant", "bear", "zebra", "giraffe", "backpack", "umbrella", "handbag", "tie", "suitcase", "frisbee",
        "skis", "snowboard", "sports ball", "kite", "baseball bat", "baseball glove", "skateboard", "surfboard",
        "tennis racket", "bottle", "wine glass", "cup", "fork", "knife", "spoon", "bowl", "banana", "apple",
        "sandwich", "orange", "broccoli", "carrot", "hot dog", "pizza", "donut", "cake", "chair", "couch",
        "potted plant", "bed", "dining table", "toilet", "tv", "laptop", "mouse", "remote", "keyboard", "cell phone",
        "microwave", "oven", "toaster", "sink", "refrigerator", "book", "clock", "vase", "scissors", "teddy bear",
        "hair drier", "toothbrush"
    };
}
