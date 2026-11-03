#pragma once

// The phone app's trackball camera (android/.../RenderEngine.kt, ADR-0012), ported for the
// desktop viewer so both hosts navigate the same way: the eye orbits a pivot at a distance along
// the camera's +z; orientation is camera-to-world. A drag turns about the camera's own up and right
// axes (no fixed axis, no limits) or, in level mode, about the scene's fitted up axis. A double
// click moves the pivot to the picked point with a short turn of the camera (the eye stays put).
// RUB world, glm quaternions (w, x, y, z); every composition is renormalized.

#include <algorithm>
#include <cmath>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <optional>

#include "splatcore/home_view.h"

namespace pocketsplat {

class Trackball {
public:
    static constexpr float kOrbitRadPerPixel = 0.005f;  // the phone's ORBIT_RAD_PER_PX
    static constexpr double kPivotAnimationSeconds = 0.3;

    // Home pose from splatcore::estimate_home_view: looking from eye to focus with `up` up.
    void set_home(const splatcore::HomeView& home, float scene_diagonal) {
        m_diagonal = std::max(scene_diagonal, 1e-3f);
        const glm::vec3 focus(home.focus[0], home.focus[1], home.focus[2]);
        const glm::vec3 eye(home.eye[0], home.eye[1], home.eye[2]);
        m_scene_up = glm::normalize(glm::vec3(home.up[0], home.up[1], home.up[2]));
        const glm::vec3 back = glm::normalize(eye - focus);
        const glm::vec3 right = glm::normalize(glm::cross(m_scene_up, back));
        m_home_orientation = glm::normalize(glm::quat_cast(glm::mat3(right, glm::cross(back, right), back)));
        m_home_pivot = focus;
        m_home_distance = std::max(glm::length(eye - focus), 1e-4f * m_diagonal);
        reset_home();
    }

    void reset_home() {
        m_animation.reset();
        m_pivot = m_home_pivot;
        m_orientation = m_home_orientation;
        m_distance = m_home_distance;
    }

    // A drag of (dx, dy) pixels; the scene follows the cursor.
    void orbit(float dx, float dy) {
        m_animation.reset();
        const glm::quat pitch = glm::angleAxis(-dy * kOrbitRadPerPixel, glm::vec3(1.0f, 0.0f, 0.0f));
        if (m_level) {
            const glm::quat yaw = glm::angleAxis(-dx * kOrbitRadPerPixel, m_scene_up);
            m_orientation = glm::normalize(yaw * m_orientation * pitch);
        } else {
            const glm::quat yaw = glm::angleAxis(-dx * kOrbitRadPerPixel, glm::vec3(0.0f, 1.0f, 0.0f));
            m_orientation = glm::normalize(m_orientation * yaw * pitch);
        }
    }

    // The eye moves toward (factor < 1) or away from the pivot.
    void zoom(float factor) {
        m_animation.reset();
        m_distance = glm::clamp(m_distance * factor, 1e-4f * m_diagonal, 10.0f * m_diagonal);
    }

    // A drag by (dx, dy) in view long sides: the pivot moves in the camera plane so the point at
    // the pivot's depth stays under the cursor.
    void pan(float dx, float dy, float fov_long_side) {
        m_animation.reset();
        const float world_per_long_side = 2.0f * m_distance * std::tan(0.5f * fov_long_side);
        const glm::vec3 right = m_orientation * glm::vec3(1.0f, 0.0f, 0.0f);
        const glm::vec3 up = m_orientation * glm::vec3(0.0f, 1.0f, 0.0f);
        m_pivot += (-right * dx + up * dy) * world_per_long_side;
    }

    // Turn toward a picked point and orbit around it from here on.
    void start_pivot_animation(const glm::vec3& point, double now_seconds) {
        const glm::vec3 eye_now = eye();
        const glm::vec3 to = point - eye_now;
        const float to_distance = glm::length(to);
        if (to_distance < 1e-5f * m_diagonal) return;
        const glm::vec3 forward = m_orientation * glm::vec3(0.0f, 0.0f, -1.0f);
        const glm::quat target = glm::normalize(from_to(forward, to / to_distance) * m_orientation);
        m_animation = Animation{eye_now, m_orientation, target, m_distance, to_distance, now_seconds};
    }

    // Advances the pivot animation; true while it runs (the caller keeps drawing).
    bool step(double now_seconds) {
        if (!m_animation) return false;
        const Animation& a = *m_animation;
        const float t =
            static_cast<float>(glm::clamp((now_seconds - a.start) / kPivotAnimationSeconds, 0.0, 1.0));
        const float s = t * t * (3.0f - 2.0f * t);  // smoothstep
        m_orientation = glm::normalize(glm::slerp(a.from, a.to, s));
        m_distance = a.from_distance + s * (a.to_distance - a.from_distance);
        m_pivot = a.eye - m_distance * (m_orientation * glm::vec3(0.0f, 0.0f, 1.0f));
        if (t >= 1.0f) m_animation.reset();
        return true;
    }

    void set_level(bool level) { m_level = level; }
    bool level() const { return m_level; }

    glm::vec3 eye() const { return m_pivot + m_distance * (m_orientation * glm::vec3(0.0f, 0.0f, 1.0f)); }
    // World -> view: the inverse of the camera's rigid pose.
    glm::mat4 view() const {
        const glm::mat3 inverse = glm::transpose(glm::mat3_cast(m_orientation));
        glm::mat4 view(inverse);
        view[3] = glm::vec4(-(inverse * eye()), 1.0f);
        return view;
    }
    float diagonal() const { return m_diagonal; }

private:
    struct Animation {
        glm::vec3 eye;
        glm::quat from, to;
        float from_distance, to_distance;
        double start;
    };

    // Shortest rotation taking unit vector `from` to unit vector `to`.
    static glm::quat from_to(const glm::vec3& from, const glm::vec3& to) {
        const float d = glm::dot(from, to);
        if (d < -0.9999f) {  // opposite: any perpendicular axis
            const glm::vec3 axis =
                std::abs(from.x) < 0.9f ? glm::vec3(0.0f, -from.z, from.y) : glm::vec3(-from.z, 0.0f, from.x);
            const glm::vec3 n = glm::normalize(axis);
            return glm::quat(0.0f, n.x, n.y, n.z);
        }
        const glm::vec3 c = glm::cross(from, to);
        return glm::normalize(glm::quat(1.0f + d, c.x, c.y, c.z));
    }

    glm::vec3 m_pivot{0.0f};
    glm::quat m_orientation{1.0f, 0.0f, 0.0f, 0.0f};
    float m_distance = 5.0f;
    float m_diagonal = 1.0f;
    bool m_level = false;
    glm::vec3 m_scene_up{0.0f, 1.0f, 0.0f};
    glm::vec3 m_home_pivot{0.0f};
    glm::quat m_home_orientation{1.0f, 0.0f, 0.0f, 0.0f};
    float m_home_distance = 5.0f;
    std::optional<Animation> m_animation;
};

}  // namespace pocketsplat
