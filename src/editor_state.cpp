#include "editor_state.h"

#include <chrono>

namespace CameraControls
{
	namespace
	{
		State      g_state;
		std::mutex g_mutex;

		const std::chrono::steady_clock::time_point g_epoch = std::chrono::steady_clock::now();
	}

	State&      Get()   { return g_state; }
	std::mutex& Mutex() { return g_mutex; }

	void Post(State& state, Request request)
	{
		// A duplicate request in the same frame is always a double-fire (key
		// repeat, a button held across two UI frames) rather than a genuine
		// "do it twice", so collapse them.
		for (Request existing : state.requests)
			if (existing == request)
				return;

		state.requests.push_back(request);
	}

	void SetStatus(State& state, double now, const char* text)
	{
		state.status       = text ? text : "";
		state.statusExpiry = now + 4.0;
	}

	double Now()
	{
		const auto delta = std::chrono::steady_clock::now() - g_epoch;
		return std::chrono::duration<double>(delta).count();
	}
}
