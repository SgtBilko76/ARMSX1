#pragma once

// Keeps the emulator thread on the fast cores. The Quest 3's six cores come in two speeds (two
// at capacity 848, four at 1024), and a heavy game scene that takes 11 ms per frame on a fast
// core takes 21 ms on a slow one -- well past the 16.7 ms a 60 Hz frame has, which drains the
// audio queue and crackles. The scheduler does move the thread there under load.

#include <string>
#include <vector>

#if defined(__linux__)
#include <sched.h>
#include <sys/resource.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#endif

namespace armsx::vr
{
	// Relative speed of each core (cpu_capacity, else the maximum clock), indexed by core number.
	inline const std::vector<long>& CoreSpeeds()
	{
		static const std::vector<long> speeds = [] {
			std::vector<long> result;
#if defined(__linux__)
			for (int cpu = 0; cpu < CPU_SETSIZE; cpu++)
			{
				long value = -1;
				for (const char* file : {"cpu_capacity", "cpufreq/cpuinfo_max_freq"})
				{
					char path[96];
					std::snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/%s", cpu, file);
					if (FILE* f = std::fopen(path, "r"))
					{
						if (std::fscanf(f, "%ld", &value) != 1)
							value = -1;
						std::fclose(f);
					}
					if (value > 0)
						break;
				}
				if (value <= 0)
					break;
				result.push_back(value);
			}
#endif
			return result;
		}();
		return speeds;
	}

	// True when the calling thread runs on one of the slower cores right now.
	inline bool OnSlowCore()
	{
#if defined(__linux__)
		const std::vector<long>& speeds = CoreSpeeds();
		const int cpu = sched_getcpu();
		if (cpu < 0 || cpu >= static_cast<int>(speeds.size()))
			return false;
		for (long speed : speeds)
			if (speed > speeds[cpu])
				return true;
#endif
		return false;
	}

	// Restricts the calling thread to the fastest cores and raises its priority. Describes what
	// it did in `summary` for the log. Does nothing on uniform machines.
	inline bool PinCurrentThreadToFastCores(std::string& summary)
	{
#if defined(__linux__)
		const std::vector<long>& speeds = CoreSpeeds();
		long fastest = 0;
		for (long speed : speeds)
			fastest = speed > fastest ? speed : fastest;

		cpu_set_t set;
		CPU_ZERO(&set);
		int count = 0;
		summary = "cores";
		for (size_t cpu = 0; cpu < speeds.size(); cpu++)
		{
			if (speeds[cpu] != fastest)
				continue;
			CPU_SET(cpu, &set);
			count++;
			summary += " " + std::to_string(cpu);
		}
		if (count == 0 || count == static_cast<int>(speeds.size()))
		{
			summary = "all cores equal, not pinned";
			return false;
		}
		const bool pinned = sched_setaffinity(0, sizeof(set), &set) == 0;
		// Android lets an app raise its own threads down to nice -20; -10 sits between the
		// display (-4/-8) and audio (-16/-19) priorities.
		const bool raised = setpriority(PRIO_PROCESS, static_cast<id_t>(gettid()), -10) == 0;
		summary += pinned ? " pinned" : " pin failed";
		summary += raised ? ", nice -10" : ", nice unchanged";
		return pinned;
#else
		summary = "not supported";
		return false;
#endif
	}

	// New threads inherit their creator's core set and priority. Wrap the creation of helper
	// threads (audio, stereo workers) on the pinned emulator thread in this, so they don't
	// crowd onto the emulator's cores at its priority; the emulator thread is restored after.
	class ScopedUnpinned
	{
	public:
		ScopedUnpinned()
		{
#if defined(__linux__)
			m_saved = sched_getaffinity(0, sizeof(m_set), &m_set) == 0;
			errno = 0;
			m_nice = getpriority(PRIO_PROCESS, static_cast<id_t>(gettid()));
			m_nice_saved = errno == 0;
			const std::vector<long>& speeds = CoreSpeeds();
			if (m_saved && !speeds.empty())
			{
				cpu_set_t all;
				CPU_ZERO(&all);
				for (size_t cpu = 0; cpu < speeds.size(); cpu++)
					CPU_SET(cpu, &all);
				sched_setaffinity(0, sizeof(all), &all);
			}
			if (m_nice_saved && m_nice < 0)
				setpriority(PRIO_PROCESS, static_cast<id_t>(gettid()), 0);
#endif
		}

		~ScopedUnpinned()
		{
#if defined(__linux__)
			if (m_saved)
				sched_setaffinity(0, sizeof(m_set), &m_set);
			if (m_nice_saved && m_nice < 0)
				setpriority(PRIO_PROCESS, static_cast<id_t>(gettid()), m_nice);
#endif
		}

		ScopedUnpinned(const ScopedUnpinned&) = delete;
		ScopedUnpinned& operator=(const ScopedUnpinned&) = delete;

	private:
#if defined(__linux__)
		cpu_set_t m_set{};
		bool m_saved = false;
		int m_nice = 0;
		bool m_nice_saved = false;
#endif
	};
} // namespace armsx::vr
