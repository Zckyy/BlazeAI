#include "tracker.h"

#include <algorithm>
#include <cmath>

namespace {

// EMA weights: position favors the fresh detection (track stays responsive, EMA only
// shaves box jitter); velocity favors history (finite-difference velocity is noisy).
constexpr float kPosAlpha = 0.6f;
constexpr float kVelAlpha = 0.35f;

float IoU(float acx, float acy, float aw, float ah, const cv::Rect& b) {
    float ax1 = acx - aw / 2.0f, ay1 = acy - ah / 2.0f;
    float ax2 = acx + aw / 2.0f, ay2 = acy + ah / 2.0f;
    float bx1 = (float)b.x, by1 = (float)b.y;
    float bx2 = (float)(b.x + b.width), by2 = (float)(b.y + b.height);

    float ix = std::max(0.0f, std::min(ax2, bx2) - std::max(ax1, bx1));
    float iy = std::max(0.0f, std::min(ay2, by2) - std::max(ay1, by1));
    float inter = ix * iy;
    float uni = aw * ah + (float)b.width * (float)b.height - inter;
    return (uni > 0.0f) ? inter / uni : 0.0f;
}

} // namespace

void Tracker::Reset() {
    m_tracks.clear();
    m_hasLastUpdate = false;
}

void Tracker::Update(std::vector<Detection>& detections, float iouThreshold, int maxMissedFrames) {
    auto now = std::chrono::steady_clock::now();
    float dt = 0.0f;
    if (m_hasLastUpdate) {
        dt = std::chrono::duration<float>(now - m_lastUpdate).count();
    }
    m_lastUpdate = now;
    m_hasLastUpdate = true;
    // Clamp so a long stall (model reload, alt-tab) can't produce a huge coast jump
    // or an absurd velocity estimate on the next matched frame.
    dt = std::clamp(dt, 0.001f, 0.1f);

    // Predict each track forward by its velocity; matching is done against the
    // predicted box so fast-moving targets still overlap their own track.
    struct Predicted { float cx, cy; };
    std::vector<Predicted> pred(m_tracks.size());
    for (size_t i = 0; i < m_tracks.size(); ++i) {
        pred[i] = { m_tracks[i].cx + m_tracks[i].vx * dt,
                    m_tracks[i].cy + m_tracks[i].vy * dt };
    }

    // Greedy IoU matching: repeatedly take the best (track, detection) pair above the
    // threshold. O(T*D) pairs with T,D <= ~20, so no assignment solver is needed.
    std::vector<int> detToTrack(detections.size(), -1);
    std::vector<bool> trackMatched(m_tracks.size(), false);
    while (true) {
        float bestIou = iouThreshold;
        int bestT = -1, bestD = -1;
        for (size_t t = 0; t < m_tracks.size(); ++t) {
            if (trackMatched[t]) continue;
            for (size_t d = 0; d < detections.size(); ++d) {
                if (detToTrack[d] != -1) continue;
                float iou = IoU(pred[t].cx, pred[t].cy, m_tracks[t].w, m_tracks[t].h,
                                detections[d].box);
                if (iou > bestIou) { bestIou = iou; bestT = (int)t; bestD = (int)d; }
            }
        }
        if (bestT < 0) break;
        trackMatched[bestT] = true;
        detToTrack[bestD] = bestT;
    }

    // Update matched tracks from their detection.
    for (size_t d = 0; d < detections.size(); ++d) {
        if (detToTrack[d] < 0) continue;
        Track& tr = m_tracks[detToTrack[d]];
        const Detection& det = detections[d];

        float ncx = det.box.x + det.box.width / 2.0f;
        float ncy = det.box.y + det.box.height / 2.0f;

        float instVx = (ncx - tr.cx) / dt;
        float instVy = (ncy - tr.cy) / dt;
        tr.vx += kVelAlpha * (instVx - tr.vx);
        tr.vy += kVelAlpha * (instVy - tr.vy);

        tr.cx += kPosAlpha * (ncx - tr.cx);
        tr.cy += kPosAlpha * (ncy - tr.cy);
        tr.w  += kPosAlpha * (det.box.width  - tr.w);
        tr.h  += kPosAlpha * (det.box.height - tr.h);

        tr.classId = det.classId;
        tr.confidence = det.confidence;
        tr.label = det.label;
        tr.missedFrames = 0;
        tr.hits++;
    }

    // Unmatched detections spawn new (unconfirmed) tracks.
    for (size_t d = 0; d < detections.size(); ++d) {
        if (detToTrack[d] != -1) continue;
        const Detection& det = detections[d];
        Track tr;
        tr.id = m_nextId++;
        tr.cx = det.box.x + det.box.width / 2.0f;
        tr.cy = det.box.y + det.box.height / 2.0f;
        tr.w = (float)det.box.width;
        tr.h = (float)det.box.height;
        tr.vx = tr.vy = 0.0f;
        tr.classId = det.classId;
        tr.confidence = det.confidence;
        tr.label = det.label;
        tr.missedFrames = 0;
        tr.hits = 1;
        m_tracks.push_back(std::move(tr));
    }

    // Unmatched tracks coast forward on their velocity; kill them after the limit.
    // Tracks appended this frame sit past trackMatched.size() and are skipped here.
    for (size_t t = 0; t < trackMatched.size(); ++t) {
        if (trackMatched[t]) continue;
        Track& tr = m_tracks[t];
        tr.missedFrames++;
        tr.cx += tr.vx * dt;
        tr.cy += tr.vy * dt;
    }
    m_tracks.erase(std::remove_if(m_tracks.begin(), m_tracks.end(),
        [&](const Track& tr) {
            // Confirmed tracks get the full coast budget; an unconfirmed one-frame
            // blip dies on its first miss.
            int limit = (tr.hits >= 2) ? maxMissedFrames : 0;
            return tr.missedFrames > limit;
        }), m_tracks.end());

    // Emit confirmed tracks (including coasting ones) as the new detection list.
    detections.clear();
    for (const Track& tr : m_tracks) {
        if (tr.hits < 2) continue;
        Detection det;
        det.box = cv::Rect((int)std::lround(tr.cx - tr.w / 2.0f),
                           (int)std::lround(tr.cy - tr.h / 2.0f),
                           (int)std::lround(tr.w),
                           (int)std::lround(tr.h));
        det.confidence = tr.confidence;
        det.classId = tr.classId;
        det.label = tr.label;
        det.trackId = tr.id;
        det.vx = tr.vx;
        det.vy = tr.vy;
        detections.push_back(std::move(det));
    }
}
