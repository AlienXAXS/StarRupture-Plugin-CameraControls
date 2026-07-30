#include "editor_state.h"

#include <algorithm>
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

	Selection KindOf(const State& state, uint32_t id)
	{
		if (id == 0)
			return Selection::None;
		if (state.timeline.Find(id))
			return Selection::Keyframe;
		if (state.timeline.FindFunc(id))
			return Selection::Function;
		return Selection::None;
	}

	bool IsSelected(const State& state, uint32_t id)
	{
		if (id == 0)
			return false;

		if (state.multiSelection.empty())
			return state.selectedId == id && state.selection != Selection::None;

		for (uint32_t selected : state.multiSelection)
			if (selected == id)
				return true;

		return false;
	}

	void SelectOnly(State& state, uint32_t id)
	{
		state.multiSelection.clear();
		state.selectedId = id;
		state.selection  = KindOf(state, id);
	}

	void ToggleSelection(State& state, uint32_t id)
	{
		if (id == 0 || KindOf(state, id) == Selection::None)
			return;

		if (state.multiSelection.empty())
		{
			// First Ctrl+click. Whatever was already selected joins the set --
			// otherwise Ctrl+clicking a second thing would quietly throw the first
			// one away, which is the opposite of what the modifier is for.
			if (state.selectedId != 0 && state.selectedId != id &&
			    KindOf(state, state.selectedId) != Selection::None)
			{
				state.multiSelection.push_back(state.selectedId);
			}
			else
			{
				// Nothing worth keeping. Ctrl+click behaves as a plain click.
				SelectOnly(state, id);
				return;
			}
		}

		auto it = std::find(state.multiSelection.begin(), state.multiSelection.end(), id);
		if (it != state.multiSelection.end())
			state.multiSelection.erase(it);
		else
			state.multiSelection.push_back(id);

		// Collapse rather than carry a one-element set -- see the invariant on
		// State::multiSelection.
		if (state.multiSelection.size() <= 1)
		{
			const uint32_t survivor = state.multiSelection.empty() ? 0 : state.multiSelection.front();
			SelectOnly(state, survivor);
			return;
		}

		// The last thing touched leads, so the breadcrumb and any single-item
		// affordance still name something the user just pointed at.
		state.selectedId = IsSelected(state, id) ? id : state.multiSelection.back();
		state.selection  = KindOf(state, state.selectedId);
	}

	void ClearSelection(State& state)
	{
		state.multiSelection.clear();
		state.selectedId = 0;
		state.selection  = Selection::None;
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
