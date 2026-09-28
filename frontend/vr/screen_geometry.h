#pragma once

// Where the virtual screen sits and where a controller's laser meets it. Pure maths with no
// OpenXR types, so it is tested on a desktop compiler (tests/quest_vr_logic_test.cpp).
//
// Coordinates are OpenXR's: metres, +x right, +y up, -z forward, in the LOCAL reference space.

#include <cmath>

namespace armsx::vr
{
	struct Vec3
	{
		float x = 0.0f, y = 0.0f, z = 0.0f;
	};

	struct Quat
	{
		float x = 0.0f, y = 0.0f, z = 0.0f, w = 1.0f;
	};

	inline Vec3 Rotate(const Quat& q, const Vec3& v)
	{
		// v' = v + 2w(u x v) + 2(u x (u x v)), u = q.xyz
		const Vec3 t{2.0f * (q.y * v.z - q.z * v.y), 2.0f * (q.z * v.x - q.x * v.z), 2.0f * (q.x * v.y - q.y * v.x)};
		return {v.x + q.w * t.x + (q.y * t.z - q.z * t.y), v.y + q.w * t.y + (q.z * t.x - q.x * t.z),
			v.z + q.w * t.z + (q.x * t.y - q.y * t.x)};
	}

	inline Quat YawQuat(float yaw)
	{
		return {0.0f, std::sin(yaw * 0.5f), 0.0f, std::cos(yaw * 0.5f)};
	}

	// Heading of an orientation, 0 when looking down -z, positive turning left.
	inline float YawOf(const Quat& orientation)
	{
		const Vec3 forward = Rotate(orientation, {0.0f, 0.0f, -1.0f});
		return std::atan2(-forward.x, -forward.z);
	}

	// One controller's laser pointer where it meets the screen, in 0..1 texture coordinates with
	// the origin at the top left.
	struct PointerHit
	{
		bool valid = false;
		float u = 0.0f;
		float v = 0.0f;
	};

	struct ScreenGeometry
	{
		float yaw = 0.0f;     // the screen faces the viewer along this heading
		Vec3 head{};          // viewer position when the screen was placed
		float width_m = 2.4f; // chord width
		float distance_m = 1.5f;
		bool curved = true;
		float aspect = 16.0f / 9.0f; // texture width / height

		// A cylinder layer is posed at its AXIS -- the viewer, with the image wrapped around them at
		// `radius`. The chord across the curve is the width the user asked for, so widening the
		// screen wraps it further around rather than pushing it away.
		float CentralAngle() const { return 2.0f * std::atan((width_m * 0.5f) / distance_m); }
		float CylinderHeight() const { return distance_m * CentralAngle() / aspect; }
		float QuadHeight() const { return width_m / aspect; }

		// The centre of a flat screen, `distance_m` straight ahead.
		Vec3 QuadCentre() const
		{
			return {head.x - std::sin(yaw) * distance_m, head.y, head.z - std::cos(yaw) * distance_m};
		}

		PointerHit Intersect(const Vec3& origin, const Vec3& direction) const
		{
			const Quat unyaw = YawQuat(-yaw);
			const Vec3 o = Rotate(unyaw, {origin.x - head.x, origin.y - head.y, origin.z - head.z});
			const Vec3 d = Rotate(unyaw, direction);

			PointerHit hit;
			float u = 0.0f;
			float v = 0.0f;
			if (curved)
			{
				const float a = d.x * d.x + d.z * d.z;
				const float b = 2.0f * (o.x * d.x + o.z * d.z);
				const float c = o.x * o.x + o.z * o.z - distance_m * distance_m;
				const float disc = b * b - 4.0f * a * c;
				if (a < 1e-6f || disc < 0.0f)
					return hit;
				// The viewer is inside the cylinder, so the far root is the wall ahead of the ray.
				const float t = (-b + std::sqrt(disc)) / (2.0f * a);
				if (t <= 0.0f)
					return hit;
				const Vec3 p{o.x + t * d.x, o.y + t * d.y, o.z + t * d.z};
				u = 0.5f + std::atan2(p.x, -p.z) / CentralAngle();
				v = 0.5f - p.y / CylinderHeight();
			}
			else
			{
				if (d.z > -1e-6f)
					return hit;
				const float t = (-distance_m - o.z) / d.z;
				if (t <= 0.0f)
					return hit;
				u = 0.5f + (o.x + t * d.x) / width_m;
				v = 0.5f - (o.y + t * d.y) / QuadHeight();
			}

			if (u < 0.0f || u > 1.0f || v < 0.0f || v > 1.0f)
				return hit;
			hit.valid = true;
			hit.u = u;
			hit.v = v;
			return hit;
		}
	};
} // namespace armsx::vr
