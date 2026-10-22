#pragma once

#include <string>
#include <vector>

namespace rv::help
{

/// A key and what it does. An empty key continues the row above.
struct Row
{
	std::string m_key, m_text;
};

/// A block of rows in the `?` overlay, and a table in the user guide's keys
/// reference (docs/src/reference/keys.md, from `rvtui --help-keys`).
struct Section
{
	std::string m_id;      ///< guide anchor, e.g. "viewer-image"
	std::string m_title;   ///< guide heading, and the overlay's when m_heading
	bool m_heading = true; ///< overlay: draw the title above the rows
	int m_column = 0;      ///< overlay: 0 left, 1 right (two-column layout)
	std::vector<Row> m_rows;
};

/// The browser's keys.
const std::vector<Section>& browser();
/// The viewer's keys.
const std::vector<Section>& viewer();

/// Every section as Markdown: a heading and a table each, between mdBook
/// `ANCHOR: <id>` / `ANCHOR_END: <id>` comments for `{{#include}}`.
std::string markdown();

} // namespace rv::help
