#pragma once

#include <string>
#include <vector>

namespace rega::agent
{

/// `rega --agent-demo <sequence>`: the remote side of the prototype.
struct DemoOptions
{
	std::vector<std::string> m_paths;
	std::string m_quality = "85"; ///< 60 | 85 | 95 | lossless | raw
	int m_loops = 3;
	double m_fps = 24; ///< loops 2+; 0 as fast as the agent re-arms
};

/// Send every frame to the agent and show it from there, then replay the
/// loop from what the agent keeps; print what each loop cost.
int runAgentDemo(const DemoOptions& opt);

} // namespace rega::agent
