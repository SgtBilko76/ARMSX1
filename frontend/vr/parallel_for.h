#pragma once

// A few persistent worker threads for splitting per-frame work (the stereo eye images) across
// cores. The Quest has six; the emulator keeps one busy and the rest are mostly idle.

#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace armsx::vr
{
	class ParallelFor
	{
	public:
		explicit ParallelFor(int workers)
		{
			for (int i = 0; i < workers; i++)
				m_threads.emplace_back([this] { WorkerMain(); });
		}

		~ParallelFor()
		{
			{
				std::lock_guard<std::mutex> lock(m_mutex);
				m_quit = true;
			}
			m_wake.notify_all();
			for (std::thread& thread : m_threads)
				thread.join();
		}

		ParallelFor(const ParallelFor&) = delete;
		ParallelFor& operator=(const ParallelFor&) = delete;

		// Calls task(0 .. count-1), spread over the workers and the calling thread, and returns
		// once all of them have finished.
		void Run(int count, const std::function<void(int)>& task)
		{
			Start(count, task);
			Drain();
			Wait();
		}

		// Hands task(0 .. count-1) to the workers only and returns at once, so the caller can get
		// on with other work. `task` must stay alive until Wait(). A pool with no workers runs
		// the tasks right here.
		void Start(int count, const std::function<void(int)>& task)
		{
			Wait();
			{
				std::lock_guard<std::mutex> lock(m_mutex);
				m_task = &task;
				m_count = count;
				m_next = 0;
				m_pending = count;
				m_generation++;
			}
			m_wake.notify_all();
			if (m_threads.empty())
				Drain();
		}

		// Blocks until the tasks from Start() are done (returns at once if there are none).
		void Wait()
		{
			std::unique_lock<std::mutex> lock(m_mutex);
			m_done.wait(lock, [this] { return m_pending == 0; });
			m_task = nullptr;
		}

		bool Busy()
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			return m_pending != 0;
		}

	private:
		// Takes and runs tasks until none are left.
		void Drain()
		{
			for (;;)
			{
				int index = 0;
				const std::function<void(int)>* task = nullptr;
				{
					std::lock_guard<std::mutex> lock(m_mutex);
					if (!m_task || m_next >= m_count)
						return;
					index = m_next++;
					task = m_task;
				}
				(*task)(index);
				std::lock_guard<std::mutex> lock(m_mutex);
				if (--m_pending == 0)
					m_done.notify_all();
			}
		}

		void WorkerMain()
		{
			unsigned seen = 0;
			for (;;)
			{
				{
					std::unique_lock<std::mutex> lock(m_mutex);
					m_wake.wait(lock, [&] { return m_quit || m_generation != seen; });
					if (m_quit)
						return;
					seen = m_generation;
				}
				Drain();
			}
		}

		std::vector<std::thread> m_threads;
		std::mutex m_mutex;
		std::condition_variable m_wake;
		std::condition_variable m_done;
		const std::function<void(int)>* m_task = nullptr;
		int m_count = 0;
		int m_next = 0;
		int m_pending = 0;
		unsigned m_generation = 0;
		bool m_quit = false;
	};
} // namespace armsx::vr
