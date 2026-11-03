package com.pocketsplat.app

import kotlin.math.acos
import kotlin.math.cos
import kotlin.math.sin
import kotlin.math.sqrt

/**
 * Unit quaternions as FloatArray(4) = (w, x, y, z), the order splatcore and the JNI use. Only what
 * the trackball camera needs; every operation that composes rotations is followed by [normalize]
 * at the call site, so rounding never accumulates into a scaled rotation.
 */
object Quat {
    fun identity() = floatArrayOf(1f, 0f, 0f, 0f)

    /** Rotation by [angle] radians about the unit [axis]. */
    fun axisAngle(ax: Float, ay: Float, az: Float, angle: Float): FloatArray {
        val s = sin(0.5f * angle)
        return floatArrayOf(cos(0.5f * angle), ax * s, ay * s, az * s)
    }

    /** a * b: apply b first, then a. */
    fun mul(a: FloatArray, b: FloatArray) = floatArrayOf(
        a[0] * b[0] - a[1] * b[1] - a[2] * b[2] - a[3] * b[3],
        a[0] * b[1] + a[1] * b[0] + a[2] * b[3] - a[3] * b[2],
        a[0] * b[2] - a[1] * b[3] + a[2] * b[0] + a[3] * b[1],
        a[0] * b[3] + a[1] * b[2] - a[2] * b[1] + a[3] * b[0],
    )

    fun normalize(q: FloatArray): FloatArray {
        val n = sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3])
        return if (n > 0f) floatArrayOf(q[0] / n, q[1] / n, q[2] / n, q[3] / n) else identity()
    }

    /** The rotated vector q v q*. */
    fun rotate(q: FloatArray, vx: Float, vy: Float, vz: Float): FloatArray {
        val (w, x, y, z) = q
        // t = 2 (q.xyz x v); v' = v + w t + q.xyz x t
        val tx = 2f * (y * vz - z * vy)
        val ty = 2f * (z * vx - x * vz)
        val tz = 2f * (x * vy - y * vx)
        return floatArrayOf(
            vx + w * tx + (y * tz - z * ty),
            vy + w * ty + (z * tx - x * tz),
            vz + w * tz + (x * ty - y * tx),
        )
    }

    /** Shortest rotation taking unit vector [from] to unit vector [to]. */
    fun fromTo(from: FloatArray, to: FloatArray): FloatArray {
        val d = from[0] * to[0] + from[1] * to[1] + from[2] * to[2]
        if (d < -0.9999f) {  // opposite: any perpendicular axis
            val axis = if (kotlin.math.abs(from[0]) < 0.9f) floatArrayOf(0f, -from[2], from[1]) else floatArrayOf(-from[2], 0f, from[0])
            val n = sqrt(axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2])
            return floatArrayOf(0f, axis[0] / n, axis[1] / n, axis[2] / n)
        }
        val cx = from[1] * to[2] - from[2] * to[1]
        val cy = from[2] * to[0] - from[0] * to[2]
        val cz = from[0] * to[1] - from[1] * to[0]
        return normalize(floatArrayOf(1f + d, cx, cy, cz))
    }

    /** Spherical interpolation along the shorter arc, t in [0, 1]. */
    fun slerp(a: FloatArray, b: FloatArray, t: Float): FloatArray {
        var d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3]
        val sign = if (d < 0f) -1f else 1f
        d *= sign
        if (d > 0.9995f) {  // nearly equal: linear is exact enough
            return normalize(FloatArray(4) { a[it] + t * (sign * b[it] - a[it]) })
        }
        val theta = acos(d)
        val wa = sin((1f - t) * theta) / sin(theta)
        val wb = sign * sin(t * theta) / sin(theta)
        return normalize(FloatArray(4) { wa * a[it] + wb * b[it] })
    }
}
