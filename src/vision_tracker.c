#include <string.h>
#include "vision_tracker.h"

#define TPV_VISION_MAX_MATCH_EDGES (TPV_VISION_MAX_TRACKS * TPV_VISION_MAX_TRACKS)

typedef struct {
    int track_index;
    int detection_index;
    float iou;
    int distance_sq;
} tpv_vision_match_edge;

static int detection_center_x(const tpv_vision_detection *d) {
    if ((d->flags & TPV_DETECTION_HAS_CENTER) != 0) return d->center_x;
    if ((d->flags & TPV_DETECTION_HAS_BBOX) != 0) return d->bbox_x + d->bbox_w / 2;
    return 0;
}

static int detection_center_y(const tpv_vision_detection *d) {
    if ((d->flags & TPV_DETECTION_HAS_CENTER) != 0) return d->center_y;
    if ((d->flags & TPV_DETECTION_HAS_BBOX) != 0) return d->bbox_y + d->bbox_h / 2;
    return 0;
}

static int has_bbox(const tpv_vision_detection *d) {
    return (d->flags & TPV_DETECTION_HAS_BBOX) != 0 && d->bbox_w > 0 && d->bbox_h > 0;
}

static float bbox_iou(const tpv_vision_detection *a, const tpv_vision_detection *b) {
    if (!has_bbox(a) || !has_bbox(b)) return 0.0f;
    int ax1 = a->bbox_x + a->bbox_w;
    int ay1 = a->bbox_y + a->bbox_h;
    int bx1 = b->bbox_x + b->bbox_w;
    int by1 = b->bbox_y + b->bbox_h;
    int ix0 = a->bbox_x > b->bbox_x ? a->bbox_x : b->bbox_x;
    int iy0 = a->bbox_y > b->bbox_y ? a->bbox_y : b->bbox_y;
    int ix1 = ax1 < bx1 ? ax1 : bx1;
    int iy1 = ay1 < by1 ? ay1 : by1;
    int iw = ix1 - ix0;
    int ih = iy1 - iy0;
    if (iw <= 0 || ih <= 0) return 0.0f;
    int intersection = iw * ih;
    int area_a = a->bbox_w * a->bbox_h;
    int area_b = b->bbox_w * b->bbox_h;
    int denom = area_a + area_b - intersection;
    return denom > 0 ? (float)intersection / (float)denom : 0.0f;
}

static int center_distance_sq(const tpv_vision_detection *a, const tpv_vision_detection *b) {
    int dx = detection_center_x(a) - detection_center_x(b);
    int dy = detection_center_y(a) - detection_center_y(b);
    return dx * dx + dy * dy;
}

static int same_track_group(const tpv_vision_track *track, const tpv_vision_detection *d) {
    return track->active &&
           track->engine_id == d->engine_id &&
           track->class_id == d->class_id;
}

static int detection_matches_track(const tpv_vision_config *cfg,
                                   const tpv_vision_track *track,
                                   const tpv_vision_detection *d,
                                   float *iou_out,
                                   int *distance_sq_out) {
    float iou = bbox_iou(&track->detection, d);
    int dist_sq = center_distance_sq(&track->detection, d);
    float max_dist = cfg->tracker_center_distance_px;
    int matched = 0;
    if (has_bbox(&track->detection) && has_bbox(d) && iou >= cfg->tracker_iou_threshold) {
        matched = 1;
    }
    if (dist_sq <= (int)(max_dist * max_dist)) matched = 1;
    *iou_out = iou;
    *distance_sq_out = dist_sq;
    return matched;
}

static tpv_vision_track *find_free_track(tpv_vision_tracker *tracker) {
    for (int i = 0; i < TPV_VISION_MAX_TRACKS; i++) {
        if (!tracker->tracks[i].active) return &tracker->tracks[i];
    }
    return 0;
}

static void assign_detection_state(tpv_vision_detection *d, const tpv_vision_track *track) {
    d->track_id = track->id;
    d->track_age_frames = (int16_t)track->age_frames;
    d->track_hits = (int16_t)track->hits;
    d->track_misses = (int16_t)track->misses;
    d->flags &= (uint16_t)~(TPV_DETECTION_TRACK_TENTATIVE |
                            TPV_DETECTION_TRACK_CONFIRMED |
                            TPV_DETECTION_TRACK_LOST);
    d->flags |= track->confirmed ? TPV_DETECTION_TRACK_CONFIRMED
                                 : TPV_DETECTION_TRACK_TENTATIVE;
}

static void clear_detection_track_state(tpv_vision_detection *d) {
    d->track_id = 0;
    d->flags &= (uint16_t)~(TPV_DETECTION_TRACK_TENTATIVE |
                            TPV_DETECTION_TRACK_CONFIRMED |
                            TPV_DETECTION_TRACK_LOST);
}

static int edge_precedes(const tpv_vision_match_edge *a,
                         const tpv_vision_match_edge *b,
                         const tpv_vision_tracker *tracker) {
    if (a->iou > b->iou) return 1;
    if (a->iou < b->iou) return 0;
    if (a->distance_sq < b->distance_sq) return 1;
    if (a->distance_sq > b->distance_sq) return 0;
    uint32_t a_id = tracker->tracks[a->track_index].id;
    uint32_t b_id = tracker->tracks[b->track_index].id;
    if (a_id < b_id) return 1;
    if (a_id > b_id) return 0;
    return a->detection_index < b->detection_index;
}

static void insert_edge_sorted(tpv_vision_match_edge *edges,
                               int *edge_count,
                               tpv_vision_match_edge edge,
                               const tpv_vision_tracker *tracker) {
    int pos = *edge_count;
    while (pos > 0 && edge_precedes(&edge, &edges[pos - 1], tracker)) {
        edges[pos] = edges[pos - 1];
        pos--;
    }
    edges[pos] = edge;
    *edge_count += 1;
}

static void mark_track_missed(tpv_vision_track *track,
                              const tpv_vision_config *cfg) {
    track->matched = 0;
    track->age_frames++;
    track->misses++;
    track->detection.flags &= (uint16_t)~(TPV_DETECTION_TRACK_TENTATIVE |
                                          TPV_DETECTION_TRACK_CONFIRMED);
    track->detection.flags |= TPV_DETECTION_TRACK_LOST;
    if (track->misses > cfg->tracker_max_age) track->active = 0;
}

static void create_track(tpv_vision_tracker *tracker,
                         const tpv_vision_config *cfg,
                         tpv_vision_detection *d) {
    tpv_vision_track *track = find_free_track(tracker);
    if (!track) return;
    memset(track, 0, sizeof *track);
    track->active = 1;
    track->confirmed = cfg->tracker_min_hits <= 1 ? 1u : 0u;
    track->matched = 1;
    track->id = tracker->next_track_id++;
    track->engine_id = d->engine_id;
    track->class_id = d->class_id;
    track->age_frames = 1;
    track->hits = 1;
    track->misses = 0;
    track->detection = *d;
    assign_detection_state(d, track);
    track->detection = *d;
}

void tpv_vision_tracker_init(tpv_vision_tracker *tracker) {
    if (!tracker) return;
    memset(tracker, 0, sizeof *tracker);
    tracker->next_track_id = 1;
}

void tpv_vision_tracker_reset(tpv_vision_tracker *tracker) {
    tpv_vision_tracker_init(tracker);
}

void tpv_vision_tracker_update(tpv_vision_tracker *tracker,
                               const tpv_vision_config *cfg,
                               tpv_vision_detection *detections,
                               int detection_count) {
    if (!tracker || !cfg || !detections || detection_count <= 0) {
        if (tracker && cfg && detection_count == 0) {
            for (int i = 0; i < TPV_VISION_MAX_TRACKS; i++) {
                tpv_vision_track *track = &tracker->tracks[i];
                if (!track->active) continue;
                mark_track_missed(track, cfg);
            }
        }
        return;
    }

    for (int i = 0; i < TPV_VISION_MAX_TRACKS; i++) {
        tracker->tracks[i].matched = 0;
    }

    for (int di = 0; di < detection_count; di++) {
        clear_detection_track_state(&detections[di]);
    }

    int match_count = detection_count < TPV_VISION_MAX_TRACKS
        ? detection_count
        : TPV_VISION_MAX_TRACKS;
    uint8_t detection_matched[TPV_VISION_MAX_TRACKS];
    memset(detection_matched, 0, sizeof detection_matched);

    tpv_vision_match_edge edges[TPV_VISION_MAX_MATCH_EDGES];
    int edge_count = 0;
    for (int di = 0; di < match_count; di++) {
        for (int ti = 0; ti < TPV_VISION_MAX_TRACKS; ti++) {
            tpv_vision_track *track = &tracker->tracks[ti];
            tpv_vision_detection *d = &detections[di];
            if (!same_track_group(track, d)) continue;
            if (track->misses > cfg->tracker_max_age) continue;
            float iou = 0.0f;
            int distance_sq = 0;
            if (!detection_matches_track(cfg, track, d, &iou, &distance_sq)) continue;
            tpv_vision_match_edge edge;
            edge.track_index = ti;
            edge.detection_index = di;
            edge.iou = iou;
            edge.distance_sq = distance_sq;
            insert_edge_sorted(edges, &edge_count, edge, tracker);
        }
    }

    for (int ei = 0; ei < edge_count; ei++) {
        tpv_vision_match_edge *edge = &edges[ei];
        tpv_vision_track *track = &tracker->tracks[edge->track_index];
        tpv_vision_detection *d = &detections[edge->detection_index];
        if (track->matched || detection_matched[edge->detection_index]) continue;
        track->matched = 1;
        detection_matched[edge->detection_index] = 1;
        track->age_frames++;
        track->hits++;
        track->misses = 0;
        if (track->hits >= cfg->tracker_min_hits) track->confirmed = 1;
        track->detection = *d;
        assign_detection_state(d, track);
        track->detection = *d;
    }

    for (int di = 0; di < match_count; di++) {
        if (!detection_matched[di]) create_track(tracker, cfg, &detections[di]);
    }

    for (int ti = 0; ti < TPV_VISION_MAX_TRACKS; ti++) {
        tpv_vision_track *track = &tracker->tracks[ti];
        if (!track->active || track->matched) continue;
        mark_track_missed(track, cfg);
    }
}
