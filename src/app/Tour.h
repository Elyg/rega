#pragma once

#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>

#include <string>
#include <vector>

namespace rega
{

/// Where the user is, for the tour steps that need a place as well as keys
/// (see App::tourView()).
struct TourView
{
	bool m_viewer = false; ///< the viewer is shown, else the browser
	std::string m_open;    ///< viewer: the current source's display name
	int m_sources = 0;     ///< viewer: images / sequences open
	float m_exposure = 0.0f;
};

/// `rega --tutorial`'s walkthrough: a floating card with one step at a time.
/// Each step names its keys; they turn green once pressed, and the step is
/// done when all of them are (and, for some, the user is in the right place:
/// the sequence open, the sun stopped down...). A done step shows a tick
/// until the next key, which moves on and counts for the next step too. F1
/// hides the card, F2 skips a step, F3 goes back one.
class Tour
{
public:
	Tour();

	/// An event the app is about to handle (not text being typed).
	void press(ftxui::Event e);
	/// After every event: where the user is now. True when that finished the
	/// current step.
	bool update(const TourView& now);
	void skip(); ///< on to the next step, done or not
	void back(); ///< the previous step again, from the start

	void toggleHidden() noexcept
	{
		m_hidden = !m_hidden;
	}
	[[nodiscard]] bool hidden() const noexcept
	{
		return m_hidden;
	}
	[[nodiscard]] int step() const noexcept ///< 0-based
	{
		return m_step;
	}
	[[nodiscard]] static int stepCount();
	[[nodiscard]] bool finished() const; ///< on the last step (no task)
	/// The current step's keys are all pressed; waiting for a key to go on.
	[[nodiscard]] bool done() const noexcept
	{
		return m_done;
	}
	[[nodiscard]] std::string title() const; ///< the current step's
	/// The current step's keys, as its text names them ("space", "Enter").
	[[nodiscard]] const std::vector<std::string>& keys() const noexcept
	{
		return m_keys;
	}
	/// Whether the current step's key `key` has been pressed.
	[[nodiscard]] bool pressed(const std::string& key) const;

	/// The card (without placement).
	[[nodiscard]] ftxui::Element render() const;

private:
	void enter(int step); ///< start `step` with nothing pressed

	int m_step = 0;
	bool m_hidden = false;
	bool m_done = false;
	std::vector<std::string> m_keys;
	std::vector<bool> m_pressed; ///< parallel to m_keys
};

} // namespace rega
