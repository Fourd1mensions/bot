#pragma once

#include "types.h"
#include <vector>
#include <cmath>

namespace osupp {

namespace slider {

// Bezier curve calculation
inline Vector2 bezier_point(const std::vector<Vector2>& points, double t) {
    if (points.size() == 1) return points[0];

    std::vector<Vector2> temp = points;

    while (temp.size() > 1) {
        std::vector<Vector2> next;
        for (size_t i = 0; i < temp.size() - 1; i++) {
            Vector2 p = temp[i] * (1.0 - t) + temp[i + 1] * t;
            next.push_back(p);
        }
        temp = next;
    }

    return temp[0];
}

// Calculate length of path segment
inline double calculate_bezier_length(const std::vector<Vector2>& points, int subdivisions = 50) {
    double length = 0.0;
    Vector2 prev = bezier_point(points, 0.0);

    for (int i = 1; i <= subdivisions; i++) {
        double t = static_cast<double>(i) / subdivisions;
        Vector2 curr = bezier_point(points, t);
        length += prev.distance(curr);
        prev = curr;
    }

    return length;
}

// Perfect circle (circular arc) calculation
inline std::vector<Vector2> calculate_circle_arc(const Vector2& p1, const Vector2& p2, const Vector2& p3, double desired_length) {
    std::vector<Vector2> path;

    // Calculate circle center
    double d = 2.0 * (p1.x * (p2.y - p3.y) + p2.x * (p3.y - p1.y) + p3.x * (p1.y - p2.y));
    if (std::abs(d) < 1e-10) {
        // Points are collinear, return linear path
        path.push_back(p1);
        path.push_back(p3);
        return path;
    }

    double ux = ((p1.x * p1.x + p1.y * p1.y) * (p2.y - p3.y) +
                 (p2.x * p2.x + p2.y * p2.y) * (p3.y - p1.y) +
                 (p3.x * p3.x + p3.y * p3.y) * (p1.y - p2.y)) / d;
    double uy = ((p1.x * p1.x + p1.y * p1.y) * (p3.x - p2.x) +
                 (p2.x * p2.x + p2.y * p2.y) * (p1.x - p3.x) +
                 (p3.x * p3.x + p3.y * p3.y) * (p2.x - p1.x)) / d;

    Vector2 center(ux, uy);
    double radius = center.distance(p1);

    // Calculate angles
    double start_angle = std::atan2(p1.y - center.y, p1.x - center.x);
    double end_angle = std::atan2(p3.y - center.y, p3.x - center.x);

    // Determine direction (clockwise or counter-clockwise)
    double angle_diff = end_angle - start_angle;
    while (angle_diff > M_PI) angle_diff -= 2.0 * M_PI;
    while (angle_diff < -M_PI) angle_diff += 2.0 * M_PI;

    // Check if we should go the other way
    Vector2 mid_point = bezier_point({p1, p2, p3}, 0.5);
    double mid_angle = std::atan2(mid_point.y - center.y, mid_point.x - center.x);
    double mid_diff1 = mid_angle - start_angle;
    double mid_diff2 = angle_diff;

    while (mid_diff1 > M_PI) mid_diff1 -= 2.0 * M_PI;
    while (mid_diff1 < -M_PI) mid_diff1 += 2.0 * M_PI;

    // If signs don't match, reverse direction
    if ((mid_diff1 > 0) != (mid_diff2 > 0)) {
        angle_diff = angle_diff > 0 ? angle_diff - 2.0 * M_PI : angle_diff + 2.0 * M_PI;
    }

    // Generate points along the arc
    int num_points = std::max(10, static_cast<int>(std::abs(angle_diff) * radius / 10.0));
    for (int i = 0; i <= num_points; i++) {
        double t = static_cast<double>(i) / num_points;
        double angle = start_angle + angle_diff * t;
        Vector2 point(
            center.x + radius * std::cos(angle),
            center.y + radius * std::sin(angle)
        );
        path.push_back(point);
    }

    return path;
}

// Calculate position along path at specific distance
inline Vector2 get_position_at_distance(const std::vector<Vector2>& path, double distance) {
    if (path.empty()) return Vector2(0, 0);
    if (path.size() == 1) return path[0];

    double accumulated = 0.0;

    for (size_t i = 0; i < path.size() - 1; i++) {
        double segment_length = path[i].distance(path[i + 1]);

        if (accumulated + segment_length >= distance) {
            double t = (distance - accumulated) / segment_length;
            return path[i] * (1.0 - t) + path[i + 1] * t;
        }

        accumulated += segment_length;
    }

    return path.back();
}

// Build full slider path
inline std::vector<Vector2> build_slider_path(const HitObject& slider) {
    std::vector<Vector2> path;

    if (slider.control_points.empty()) {
        return path;
    }

    switch (slider.path_type) {
        case PathType::Linear: {
            path = slider.control_points;
            break;
        }

        case PathType::Perfect: {
            if (slider.control_points.size() == 3) {
                path = calculate_circle_arc(
                    slider.control_points[0],
                    slider.control_points[1],
                    slider.control_points[2],
                    slider.pixel_length
                );
            } else {
                // Fallback to bezier
                path.push_back(slider.control_points[0]);
                int subdivisions = std::max(10, static_cast<int>(slider.pixel_length / 10.0));
                for (int i = 1; i <= subdivisions; i++) {
                    double t = static_cast<double>(i) / subdivisions;
                    path.push_back(bezier_point(slider.control_points, t));
                }
            }
            break;
        }

        case PathType::Bezier:
        case PathType::Catmull: {
            path.push_back(slider.control_points[0]);
            int subdivisions = std::max(10, static_cast<int>(slider.pixel_length / 10.0));
            for (int i = 1; i <= subdivisions; i++) {
                double t = static_cast<double>(i) / subdivisions;
                path.push_back(bezier_point(slider.control_points, t));
            }
            break;
        }
    }

    return path;
}

// Calculate actual path length
inline double calculate_path_length(const std::vector<Vector2>& path) {
    double length = 0.0;
    for (size_t i = 1; i < path.size(); i++) {
        length += path[i - 1].distance(path[i]);
    }
    return length;
}

// Get slider end position
inline Vector2 get_slider_end_position(const HitObject& slider) {
    if (slider.control_points.empty()) {
        return slider.position;
    }

    auto path = build_slider_path(slider);
    double actual_length = calculate_path_length(path);

    // Account for repeats
    double target_distance = slider.pixel_length * (slider.repeat_count % 2 == 0 ? 0.0 : 1.0);

    if (actual_length < 1e-10) {
        return slider.position;
    }

    // Normalize and get position
    double normalized_distance = (target_distance / actual_length) * actual_length;
    return get_position_at_distance(path, normalized_distance);
}

} // namespace slider
} // namespace osupp
