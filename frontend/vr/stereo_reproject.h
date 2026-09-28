#pragma once

// Stereoscopic 3D from the PlayStation's own geometry: shifts each pixel of the displayed frame
// left or right by its depth (psx/dev/depth.h) to make the two eye images. Pure logic, tested on a
// desktop compiler (tests/quest_vr_logic_test.cpp).
//
// Parallax model: a pixel with inverse depth w is shifted apart between the eyes by
//
//     d(w) = K * (1 - C * w)          (source pixels, clamped to [-K, K])
//
// K is the separation of something infinitely far away -- the viewer's eye distance, measured in
// pixels of the game image on the virtual screen -- so the far background never asks the eyes to
// diverge. C picks what sits ON the screen plane (w = 1 / C); anything nearer comes out of it.
// Pixels without 3D depth (HUD, sprites, FMV) have d = 0 and stay on the screen plane.
//
// The eye images are built at `scale` times the source width, so a shift of a fraction of a
// source pixel still moves an edge -- without that, depth would come in visible whole-pixel steps.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace armsx::vr
{
	constexpr float kEyeDistanceM = 0.063f;

	struct StereoParams
	{
		float max_disparity_px = 8.0f; // K, in source pixels
		float convergence = 2.0f;      // C
		int scale = 4;                 // output samples per source pixel
	};

	// K for a game image `image_width_m` wide on the virtual screen, `source_width` pixels across,
	// at `strength` (1 = the true eye distance).
	inline float MaxDisparityPx(float strength, float image_width_m, int source_width)
	{
		if (image_width_m <= 0.0f || source_width <= 0)
			return 0.0f;
		return std::max(0.0f, strength) * kEyeDistanceM / image_width_m * static_cast<float>(source_width);
	}

	inline float Disparity(float w, const StereoParams& params)
	{
		if (w < 0.0f)
			return 0.0f; // flat: on the screen plane
		const float k = params.max_disparity_px;
		return std::clamp(k * (1.0f - params.convergence * w), -k, k);
	}

	class StereoReprojector
	{
	public:
		// One output row for one eye (-1 left, +1 right). `color` and `depth` are `width` source
		// pixels; `out` receives width * params.scale pixels.
		void ReprojectRow(const std::uint16_t* color, const float* depth, int width, int eye, const StereoParams& params,
			std::uint16_t* out)
		{
			const int scale = std::max(1, params.scale);
			const int out_width = width * scale;
			m_parallax.assign(static_cast<size_t>(out_width), kUnwritten);

			// Forward splat. Where two pixels land on the same spot, the nearer one (smaller
			// parallax) wins, as it would occlude the other from that eye.
			for (int x = 0; x < width; x++)
			{
				const float d = Disparity(depth[x], params);
				const float shift = static_cast<float>(eye) * d * 0.5f;
				const int start = static_cast<int>(std::lround((static_cast<float>(x) + shift) * static_cast<float>(scale)));
				const int first = std::max(start, 0);
				const int last = std::min(start + scale, out_width);
				for (int o = first; o < last; o++)
				{
					if (d < m_parallax[static_cast<size_t>(o)])
					{
						m_parallax[static_cast<size_t>(o)] = d;
						out[o] = color[x];
					}
				}
			}

			// Holes are background revealed from this eye's angle: fill each run from whichever
			// neighbour is farther away.
			int o = 0;
			while (o < out_width)
			{
				if (m_parallax[static_cast<size_t>(o)] != kUnwritten)
				{
					o++;
					continue;
				}
				int end = o;
				while (end < out_width && m_parallax[static_cast<size_t>(end)] == kUnwritten)
					end++;
				const bool has_left = o > 0;
				const bool has_right = end < out_width;
				std::uint16_t fill = 0;
				if (has_left && has_right)
				{
					const bool left_farther = m_parallax[static_cast<size_t>(o - 1)] >= m_parallax[static_cast<size_t>(end)];
					fill = left_farther ? out[o - 1] : out[end];
				}
				else if (has_left)
				{
					fill = out[o - 1];
				}
				else if (has_right)
				{
					fill = out[end];
				}
				std::fill(out + o, out + end, fill);
				o = end;
			}
		}

	private:
		static constexpr float kUnwritten = 1e30f;
		std::vector<float> m_parallax;
	};
} // namespace armsx::vr
