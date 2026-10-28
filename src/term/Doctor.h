#pragma once

#include <string>
#include <vector>

namespace rega
{

/// One line of `rega --doctor`: what was checked, what was found, and how to
/// fix it when something is off.
struct DoctorCheck
{
	/// Severity, worst last.
	enum class Level
	{
		OK,
		INFO,
		WARN,
		FAIL
	};
	Level m_level = Level::OK;
	std::string m_name;
	std::string m_detail;
	std::string m_fix; ///< empty when there is nothing to do
};

/// Terminal / tmux checks that decide how well images and copy work.
/// `forced` is the --graphics value ("auto" | "kitty" | "halfblock").
std::vector<DoctorCheck> doctorChecks(const std::string& forced);

/// Print the checks to stdout. Returns 1 if any check failed, else 0.
int runDoctor(const std::string& forced);

} // namespace rega
