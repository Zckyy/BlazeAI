#pragma once

#include "detector.h"
#include <chrono>
#include <vector>

// Frame-to-frame target tracker (greedy-IoU matching, SORT-style but lightweight).
// Assigns stable track IDs to detections so the aim logic can stick to one target
// instead of re-picking "nearest to crosshair" every frame, smooths box jitter with
// an EMA, estimates per-track velocity (px/s), and coasts tracks through short
// detection dropouts so single-frame confidence dips don't cause target snapping.
class Tracker {
public:
    // Replaces `detections` with the current set of confirmed tracks (smoothed boxes,
    // trackId/vx/vy filled in). Coasted tracks (missed this frame but still alive) are
    // included so a locked target survives dropouts. Brand-new tracks are withheld
    // until their second consecutive frame, which also filters one-frame false positives.
    //  - iouThreshold:   minimum IoU between a track's predicted box and a detection to match
    //  - maxMissedFrames: frames a track may coast unmatched before it is killed
    void Update(std::vector<Detection>& detections, float iouThreshold, int maxMissedFrames);

    // Drops all tracks (e.g. when the tracker is toggled off or vision mode changes).
    void Reset();

private:
    struct Track {
        int   id;
        float cx, cy, w, h;     // Smoothed box (center + size), screen pixels
        float vx, vy;           // EMA velocity, px/s
        int   classId;
        float confidence;
        std::string label;
        int   missedFrames;     // Consecutive frames without a matched detection
        int   hits;             // Total matched frames (>= 2 = confirmed)
    };

    std::vector<Track> m_tracks;
    int m_nextId = 1;
    std::chrono::steady_clock::time_point m_lastUpdate{};
    bool m_hasLastUpdate = false;
};
