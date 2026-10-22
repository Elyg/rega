#include "app/Browser.h"

#include "app/ViewerState.h"
#include "image/Loader.h"
#include "util/Fuzzy.h"
#include "util/Ui.h"

#include <ftxui/screen/string.hpp>
#include <ftxui/screen/terminal.hpp>
#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <fstream>
#include <functional>
#include <optional>
#include <utility>

namespace fs = std::filesystem;
using namespace ftxui;

namespace rv
{

namespace
{

// Identity of a sequence that survives frames being added: dir/prefix#suffix.
std::string sequenceKey(const Entry& e)
{
	if(e.m_frames.empty())
	{
		return {};
	}
	const fs::path& f = e.m_frames.front();
	std::string prefix, digits, suffix;
	splitFrame(f.filename().string(), prefix, digits, suffix);
	return (f.parent_path() / (prefix + "#" + suffix)).string();
}

Decorator entryStyle(const Entry& e)
{
	switch(e.m_kind)
	{
		case Entry::Kind::DIR:
			return color(Color::Blue) | bold;
		case Entry::Kind::SEQUENCE:
			return color(Color::Magenta);
		case Entry::Kind::FILE:
			return e.isImage() ? color(Color::Green) : nothing;
	}
	return nothing;
}

const char* entryIcon(const Entry& e)
{
	switch(e.m_kind)
	{
		case Entry::Kind::DIR:
			return "▸ ";
		case Entry::Kind::SEQUENCE:
			return "≋ ";
		case Entry::Kind::FILE:
			if(!e.m_expandedFrom.empty())
			{
				return "┊ "; // a frame of an expanded sequence
			}
			return e.isImage() ? "▪ " : "  ";
	}
	return "  ";
}

bool sameListing(const std::vector<Entry>& a, const std::vector<Entry>& b)
{
	if(a.size() != b.size())
	{
		return false;
	}
	for(size_t i = 0; i < a.size(); ++i)
	{
		if(a[i].m_name != b[i].m_name || a[i].m_kind != b[i].m_kind ||
		   a[i].m_size != b[i].m_size ||
		   a[i].m_frames.size() != b[i].m_frames.size())
		{
			return false;
		}
	}
	return true;
}

// The same file, sequence or directory (marks are compared by this).
bool sameEntry(const Entry& a, const Entry& b)
{
	return a.m_path == b.m_path && a.m_kind == b.m_kind;
}

} // namespace

Browser::Browser(AppContext& ctx,
                 std::string chooserFile,
                 OpenFn onOpen,
                 DiffFn onDiff)
    : m_ctx(ctx), m_chooserFile(std::move(chooserFile)),
      m_onOpen(std::move(onOpen)), m_onDiff(std::move(onDiff)),
      m_previewSlot(newSlot(ctx))
{
}

const std::vector<Entry>& Browser::cachedListing(const fs::path& dir)
{
	auto key = dir.string();
	auto it = m_listingCache.find(key);
	if(it == m_listingCache.end())
	{
		it =
		    m_listingCache.emplace(key, listDirectory(dir, m_showHidden)).first;
	}
	return it->second;
}

void Browser::openDirectory(const fs::path& dir, std::string_view selectName)
{
	if(!m_cwd.empty())
	{
		if(const Entry* e = selectedEntry())
		{
			m_lastSelected[m_cwd.string()] = e->m_name;
		}
	}
	m_cwd = dir;
	m_filter.clear();
	m_filtering = false;
	refreshListing();
	std::string want(selectName);
	if(want.empty())
	{
		auto it = m_lastSelected.find(m_cwd.string());
		if(it != m_lastSelected.end())
		{
			want = it->second;
		}
	}
	m_sel = 0;
	auto vis = visibleIndices();
	for(size_t i = 0; i < vis.size(); ++i)
	{
		if(m_entries[vis[i]].m_name == want ||
		   m_entries[vis[i]].m_path.filename() == want)
		{
			m_sel = static_cast<int>(i);
		}
	}
}

void Browser::refreshListing()
{
	m_listingCache.clear();
	m_entries = currentListing();
}

std::vector<Entry> Browser::currentListing() const
{
	std::vector<Entry> list = listDirectory(m_cwd, m_showHidden);
	if(m_expanded.empty())
	{
		return list;
	}
	std::vector<Entry> out;
	for(auto& e : list)
	{
		const std::string key =
		    e.m_kind == Entry::Kind::SEQUENCE ? sequenceKey(e) : std::string();
		if(key.empty() || !m_expanded.count(key))
		{
			out.push_back(std::move(e));
			continue;
		}
		for(const auto& frame : e.m_frames)
		{
			Entry f;
			f.m_kind = Entry::Kind::FILE;
			f.m_path = frame;
			f.m_name = frame.filename().string();
			std::error_code ec;
			f.m_size = fs::file_size(frame, ec);
			f.m_expandedFrom = key;
			out.push_back(std::move(f));
		}
	}
	return out;
}

void Browser::toggleExpand()
{
	const Entry* e = selectedEntry();
	if(!e)
	{
		return;
	}
	// Where the cursor should land afterwards.
	std::function<bool(const Entry&)> target;
	if(e->m_kind == Entry::Kind::SEQUENCE)
	{
		const fs::path first = e->m_frames.front();
		m_expanded.insert(sequenceKey(*e));
		target = [first](const Entry& x) { return x.m_path == first; };
	}
	else if(!e->m_expandedFrom.empty())
	{
		const std::string key = e->m_expandedFrom;
		m_expanded.erase(key);
		target = [key](const Entry& x)
		{ return x.m_kind == Entry::Kind::SEQUENCE && sequenceKey(x) == key; };
	}
	else
	{
		m_ctx.m_message = "not a sequence";
		return;
	}
	m_entries = currentListing();
	auto vis = visibleIndices();
	for(size_t i = 0; i < vis.size(); ++i)
	{
		if(target(m_entries[vis[i]]))
		{
			m_sel = static_cast<int>(i);
			break;
		}
	}
}

std::vector<int> Browser::visibleIndices() const
{
	std::vector<int> out;
	for(size_t i = 0; i < m_entries.size(); ++i)
	{
		if(fuzzyMatch(m_entries[i].m_name, m_filter))
		{
			out.push_back(static_cast<int>(i));
		}
	}
	return out;
}

const Entry* Browser::selectedEntry() const
{
	auto vis = visibleIndices();
	if(vis.empty())
	{
		return nullptr;
	}
	return &m_entries
	    [vis[std::clamp(m_sel, 0, static_cast<int>(vis.size()) - 1)]];
}

void Browser::moveSelection(int delta)
{
	int n = static_cast<int>(visibleIndices().size());
	if(n == 0)
	{
		return;
	}
	m_sel = std::clamp(m_sel + delta, 0, n - 1);
}

void Browser::goParent()
{
	fs::path parent = m_cwd.parent_path();
	if(parent == m_cwd || parent.empty())
	{
		return;
	}
	std::string name = m_cwd.filename().string();
	openDirectory(parent, name);
}

bool Browser::isMarked(const Entry& e) const
{
	return std::ranges::any_of(m_marks,
	                           [&](const Entry& m) { return sameEntry(m, e); });
}

void Browser::toggleMark(const Entry& e)
{
	auto it =
	    std::ranges::find_if(m_marks,
	                         [&](const Entry& m) { return sameEntry(m, e); });
	if(it != m_marks.end())
	{
		m_marks.erase(it);
	}
	else
	{
		m_marks.push_back(e);
	}
}

void Browser::mark(const std::vector<Entry>& entries)
{
	for(const Entry& e : entries)
	{
		if(e.isImage() && !isMarked(e))
		{
			m_marks.push_back(e);
		}
	}
}

void Browser::toggleMarkAll()
{
	// The images listed (passing the filter): mark them all, or, when all
	// are marked already, unmark them.
	std::vector<Entry> images;
	for(const int i : visibleIndices())
	{
		if(m_entries[i].isImage())
		{
			images.push_back(m_entries[i]);
		}
	}
	if(images.empty())
	{
		m_ctx.m_message = "no images to mark here";
		return;
	}
	if(std::ranges::all_of(images, [&](const Entry& e) { return isMarked(e); }))
	{
		std::erase_if(m_marks,
		              [&](const Entry& m)
		              {
			              return std::ranges::any_of(images,
			                                         [&](const Entry& e)
			                                         {
				                                         return sameEntry(m, e);
			                                         });
		              });
		m_ctx.m_message = fmt::format("unmarked {}", images.size());
	}
	else
	{
		mark(images);
		m_ctx.m_message = fmt::format("marked {}", images.size());
	}
}

int Browser::ListBoxes::rowAt(int x, int y) const
{
	// Rows scrolled out of the list keep boxes outside it.
	if(!inside(m_list, x, y))
	{
		return -1;
	}
	for(size_t i = 0; i < m_rows.size(); ++i)
	{
		if(inside(m_rows[i], x, y))
		{
			return static_cast<int>(i);
		}
	}
	return -1;
}

void Browser::clickRow(int row)
{
	// A click selects; a second one on the same row soon after opens it,
	// as l / Enter would.
	constexpr auto DOUBLE_CLICK = std::chrono::milliseconds(400);
	const auto now = std::chrono::steady_clock::now();
	const bool twice =
	    row == m_lastClickRow && now - m_lastClick < DOUBLE_CLICK;
	m_lastClickRow = twice ? -1 : row;
	m_lastClick = now;
	moveSelection(row - m_sel);
	if(twice)
	{
		enterSelected();
	}
}

void Browser::clickParentRow(int row)
{
	// The parent column lists this directory's siblings: go to the one
	// clicked.
	const auto& pe = cachedListing(m_cwd.parent_path());
	if(row < static_cast<int>(pe.size()) &&
	   pe[row].m_kind == Entry::Kind::DIR && pe[row].m_path != m_cwd)
	{
		openDirectory(pe[row].m_path);
	}
}

void Browser::enterSelected(bool tile)
{
	const Entry* e = selectedEntry();
	if(!e)
	{
		return;
	}
	if(e->m_kind == Entry::Kind::DIR)
	{
		openDirectory(e->m_path);
	}
	else if(e->isImage())
	{
		// Everything marked, wherever it was marked, in marking order.
		std::vector<Entry> sel;
		for(const auto& en : m_marks)
		{
			if(en.isImage())
			{
				sel.push_back(en);
			}
		}
		if(sel.empty())
		{
			sel.push_back(*e);
		}
		m_onOpen(std::move(sel), tile);
	}
	else if(textPreview(e->m_path))
	{
		m_previewFocus = true; // scroll the text in the preview column
	}
}

std::string entryCopyText(const Entry& e, char what)
{
	fs::path path = fs::absolute(e.m_path);
	if(e.m_kind == Entry::Kind::SEQUENCE)
	{
		// The pattern the name shows ("shot.####.exr [1001-1100]").
		path = path.parent_path() / e.m_name.substr(0, e.m_name.rfind(" ["));
	}
	switch(what)
	{
		case 'd':
			return path.parent_path().string();
		case 'f':
			return path.filename().string();
		case 'n':
			return e.m_kind == Entry::Kind::DIR ? path.filename().string()
			                                    : path.stem().string();
		default:
			return path.string();
	}
}

bool Browser::previewEvent(const Event& e)
{
	const int page = std::max(1, m_previewBox.y_max - m_previewBox.y_min - 3);
	const bool wasG = m_pendingG;
	m_pendingG = false;
	auto ch = [&](char c) { return e == Event::Character(c); };
	if(ch('j') || e == Event::ArrowDown)
	{
		++m_previewScroll;
	}
	else if(ch('k') || e == Event::ArrowUp)
	{
		--m_previewScroll;
	}
	else if(e == Event::CtrlD || e == Event::PageDown || ch(' '))
	{
		m_previewScroll += page / 2;
	}
	else if(e == Event::CtrlU || e == Event::PageUp)
	{
		m_previewScroll -= page / 2;
	}
	else if(ch('g'))
	{
		if(wasG)
		{
			m_previewScroll = 0;
		}
		else
		{
			m_pendingG = true;
		}
	}
	else if(ch('G'))
	{
		m_previewScroll = 1 << 30; // clamped when drawn
	}
	else if(ch('h') || ch('q') || e == Event::ArrowLeft || e == Event::Escape ||
	        e == Event::Backspace)
	{
		m_previewFocus = false;
	}
	else
	{
		return false;
	}
	m_previewScroll = std::max(0, m_previewScroll);
	return true;
}

void Browser::chooseAndExit()
{
	if(m_chooserFile.empty())
	{
		m_ctx.m_message = "no --chooser-file given";
		return;
	}
	std::ofstream out(m_chooserFile);
	if(!m_marks.empty())
	{
		for(const auto& m : m_marks)
		{
			out << m.m_path.string() << "\n";
		}
	}
	else if(const Entry* e = selectedEntry())
	{
		out << e->m_path.string() << "\n";
	}
	m_ctx.m_quit();
}

bool Browser::filterEvent(const Event& e)
{
	if(e == Event::Escape)
	{
		m_filtering = false;
		m_filter.clear();
	}
	else if(e == Event::Return)
	{
		m_filtering = false;
	}
	else if(e == Event::Backspace)
	{
		if(!m_filter.empty())
		{
			m_filter.pop_back();
		}
	}
	else if(e.is_character())
	{
		m_filter += e.character();
	}
	else if(e == Event::ArrowDown)
	{
		moveSelection(1);
	}
	else if(e == Event::ArrowUp)
	{
		moveSelection(-1);
	}
	else
	{
		return false;
	}
	moveSelection(0);
	return true;
}

bool Browser::mouseEvent(Event e)
{
	auto m = e.mouse();
	const bool overPreview =
	    m.x >= m_previewBox.x_min && m.x <= m_previewBox.x_max &&
	    m.y >= m_previewBox.y_min && m.y <= m_previewBox.y_max;
	const Entry* sel = selectedEntry();
	const bool textShown = sel && sel->m_kind == Entry::Kind::FILE &&
	                       !sel->isImage() &&
	                       textPreview(sel->m_path).has_value();
	if(overPreview && textShown &&
	   (m.button == Mouse::WheelDown || m.button == Mouse::WheelUp))
	{
		m_previewScroll =
		    std::max(0,
		             m_previewScroll + (m.button == Mouse::WheelDown ? 3 : -3));
		return true;
	}
	// Drag a column divider to resize (it sits right after a column).
	auto [parentW, currentW] = columnWidths();
	const int div1 = parentW, div2 = parentW + 1 + currentW;
	if(m.button == Mouse::Left && m.motion == Mouse::Pressed && m.y >= 1)
	{
		m_dragDivider = std::abs(m.x - div1) <= 1   ? 1
		                : std::abs(m.x - div2) <= 1 ? 2
		                                            : 0;
		if(!m_dragDivider)
		{
			// Clicking the text preview focuses it; elsewhere leaves it.
			m_previewFocus = overPreview && textShown;
			if(const int r = m_currentBoxes.rowAt(m.x, m.y); r >= 0)
			{
				clickRow(r);
			}
			else if(const int p = m_parentBoxes.rowAt(m.x, m.y); p >= 0)
			{
				clickParentRow(p);
			}
		}
		return true;
	}
	if(m_dragDivider && m.motion == Mouse::Released)
	{
		m_dragDivider = 0;
		return true;
	}
	if(m_dragDivider && m.motion == Mouse::Moved)
	{
		const double w = std::max(1, Terminal::Size().dimx);
		constexpr int MIN_COL = 6, MIN_PREVIEW = 12;
		if(m_dragDivider == 1)
		{
			int pw = std::clamp(m.x,
			                    MIN_COL,
			                    static_cast<int>(w) - currentW - MIN_PREVIEW);
			m_parentFrac = pw / w;
		}
		else
		{
			int cw = std::clamp(m.x - parentW - 1,
			                    MIN_COL,
			                    static_cast<int>(w) - parentW - MIN_PREVIEW);
			m_currentFrac = cw / w;
		}
		return true;
	}
	if(m.button == Mouse::WheelDown)
	{
		moveSelection(3);
	}
	else if(m.button == Mouse::WheelUp)
	{
		moveSelection(-3);
	}
	else
	{
		return false;
	}
	return true;
}

bool Browser::event(Event e)
{
	if(m_filtering)
	{
		return filterEvent(e);
	}
	if(m_previewFocus && !e.is_mouse() && previewEvent(e))
	{
		return true;
	}
	if(e.is_mouse())
	{
		return mouseEvent(e);
	}
	if(e == Event::CtrlH || e == Event::CtrlL || e == Event::CtrlK)
	{
		m_ctx.tmuxSelectPane(e == Event::CtrlH   ? 'L'
		                     : e == Event::CtrlL ? 'R'
		                                         : 'U');
		return true;
	}

	bool wasG = m_pendingG;
	m_pendingG = false;
	int page = std::max(1, Terminal::Size().dimy - 4);

	// `c` then c / d / f / n: copy the marked entries' (else the selected
	// one's) path, folder, file name or name, one a line. Any other key
	// cancels and does its own thing.
	if(std::exchange(m_pendingC, false))
	{
		for(char what : {'c', 'd', 'f', 'n'})
		{
			if(e != Event::Character(what))
			{
				continue;
			}
			std::vector<Entry> from = m_marks;
			if(from.empty())
			{
				if(const Entry* sel = selectedEntry())
				{
					from.push_back(*sel);
				}
			}
			std::vector<std::string> lines;
			for(const Entry& en : from)
			{
				std::string line = entryCopyText(en, what);
				if(std::ranges::find(lines, line) == lines.end())
				{
					lines.push_back(std::move(line)); // one folder once
				}
			}
			std::string text;
			for(const std::string& line : lines)
			{
				text += (text.empty() ? "" : "\n") + line;
			}
			if(!text.empty())
			{
				m_ctx.copyText(text);
			}
			return true;
		}
		m_ctx.m_message.clear();
	}

	if(e == Event::Character('c'))
	{
		m_pendingC = true;
		m_ctx.m_message =
		    "copy: c path  d folder  f file name  n name (marked: all)";
	}
	else if(e == Event::Character('q'))
	{
		m_ctx.m_quit();
	}
	else if(e == Event::Escape)
	{
		// Like yazi: Esc never quits; it clears the filter, then the marks.
		if(!m_filter.empty())
		{
			m_filter.clear();
		}
		else
		{
			m_marks.clear();
		}
	}
	else if(e == Event::Character('j') || e == Event::ArrowDown)
	{
		moveSelection(1);
	}
	else if(e == Event::Character('k') || e == Event::ArrowUp)
	{
		moveSelection(-1);
	}
	else if(e == Event::CtrlD || e == Event::PageDown)
	{
		moveSelection(page / 2);
	}
	else if(e == Event::CtrlU || e == Event::PageUp)
	{
		moveSelection(-page / 2);
	}
	else if(e == Event::Character('g'))
	{
		if(wasG)
		{
			m_sel = 0;
		}
		else
		{
			m_pendingG = true;
		}
	}
	else if(e == Event::Character('G'))
	{
		m_sel = std::max(0, static_cast<int>(visibleIndices().size()) - 1);
	}
	else if(e == Event::Character('h') || e == Event::ArrowLeft ||
	        e == Event::Backspace)
	{
		goParent();
	}
	else if(e == Event::Character('l') || e == Event::ArrowRight ||
	        e == Event::Return)
	{
		enterSelected();
	}
	else if(e == Event::Character('/'))
	{
		m_filtering = true;
		m_filter.clear();
	}
	else if(e == Event::Character('.'))
	{
		m_showHidden = !m_showHidden;
		std::string keep = selectedEntry() ? selectedEntry()->m_name : "";
		openDirectory(m_cwd, keep);
	}
	else if(e == Event::Character(' '))
	{
		// Marks are what the viewer opens, so only images take one.
		const Entry* en = selectedEntry();
		if(en && en->isImage())
		{
			toggleMark(*en);
		}
		else if(en)
		{
			m_ctx.m_message = "only images can be marked";
		}
	}
	else if(e == Event::Character('a'))
	{
		toggleMarkAll();
	}
	else if(e == Event::Character('o'))
	{
		chooseAndExit();
	}
	else if(e == Event::Character('e'))
	{
		toggleExpand();
	}
	else if(e == Event::Character('t'))
	{
		enterSelected(true);
	}
	else if(e == Event::Character('d'))
	{
		// Marked images in pairs; one marked pairs with the one here.
		std::vector<Entry> marked;
		for(const auto& en : m_marks)
		{
			if(en.isImage())
			{
				marked.push_back(en);
			}
		}
		const Entry* here = selectedEntry();
		if(marked.size() == 1 && here && here->isImage() &&
		   here->m_path != marked.front().m_path)
		{
			marked.push_back(*here);
		}
		if(marked.size() < 2)
		{
			m_ctx.m_message = "d: mark two images to diff (space), or mark "
			                  "one and stand on the other";
		}
		else if(m_onDiff)
		{
			m_onDiff(std::move(marked));
		}
	}
	else if(e == Event::Character('R'))
	{
		std::string keep = selectedEntry() ? selectedEntry()->m_name : "";
		openDirectory(m_cwd, keep);
	}
	else
	{
		return false;
	}
	return true;
}

Element Browser::renderEntryList(const std::vector<Entry>& entries,
                                 int selected,
                                 bool active,
                                 const std::vector<int>* indices,
                                 int width,
                                 ListBoxes& boxes)
{
	Elements rows;
	int n = indices ? static_cast<int>(indices->size())
	                : static_cast<int>(entries.size());
	boxes.m_rows.assign(n, Box{});
	for(int i = 0; i < n; ++i)
	{
		const Entry& e = entries[indices ? (*indices)[i] : i];
		bool marked = isMarked(e);
		// Marked entries are told apart by colour alone (no marker column,
		// which narrow columns would cut off).
		const Decorator nameStyle =
		    marked ? color(Color::Yellow) | bold : entryStyle(e);
		// Filter matches are highlighted in the active column.
		std::optional<std::vector<size_t>> hits;
		if(active && !m_filter.empty())
		{
			hits = fuzzyMatch(e.m_name, m_filter);
		}
		const std::string size = active && e.m_kind != Entry::Kind::DIR
		                             ? humanSize(e.m_size) + " "
		                             : "";
		// The name cut in the middle to what the row has left (the space
		// before it and the scroll indicator too): a wider row scrolled the
		// column sideways as the cursor moved.
		const int room = std::max(1,
		                          width - 2 - string_width(entryIcon(e)) -
		                              string_width(size));
		Element name;
		if(hits)
		{
			auto [shown, at] = ui::ellipsizeMiddle(e.m_name, room, *hits);
			name = ui::highlighted(shown, at, nameStyle);
		}
		else
		{
			name = text(ui::ellipsizeMiddle(e.m_name, room)) | nameStyle;
		}
		Element row = hbox({
		    text(" "),
		    text(entryIcon(e)) |
		        (marked                      ? color(Color::Yellow)
		         : !e.m_expandedFrom.empty() ? color(Color::Magenta)
		                                     : nothing),
		    name,
		    filler(),
		    text(size) | dim,
		});
		if(i == selected)
		{
			row = row | inverted;
			if(active)
			{
				row = row | focus;
			}
			else
			{
				row = row | focus | dim;
			}
		}
		rows.push_back(row | reflect(boxes.m_rows[i]));
	}
	if(rows.empty())
	{
		rows.push_back(text(" (empty)") | dim);
	}
	return vbox(std::move(rows)) | vscroll_indicator | yframe |
	       reflect(boxes.m_list);
}

const std::optional<std::vector<std::string>>&
Browser::textPreview(const fs::path& p)
{
	constexpr size_t MAX_BYTES = 1024 * 1024;
	constexpr size_t MAX_LINES = 20000;
	std::error_code ec;
	const auto mtime = fs::last_write_time(p, ec).time_since_epoch().count();
	const std::string key =
	    fmt::format("{}|{}|{}", p.string(), mtime, fs::file_size(p, ec));
	if(key == m_textPreview.m_key)
	{
		return m_textPreview.m_lines;
	}
	m_textPreview = {key, std::nullopt};
	m_previewScroll = 0;

	std::string buf(MAX_BYTES, '\0');
	std::ifstream in(p, std::ios::binary);
	in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
	buf.resize(static_cast<size_t>(in.gcount()));
	// Binary: any NUL, or more than a few control characters.
	size_t control = 0;
	for(unsigned char c : buf)
	{
		if(c == 0)
		{
			return m_textPreview.m_lines;
		}
		control += c < 0x20 && c != '\n' && c != '\r' && c != '\t' &&
		           c != 0x1b && c != '\f';
	}
	if(control > buf.size() / 100 + 1)
	{
		return m_textPreview.m_lines;
	}
	std::vector<std::string> lines;
	std::string line;
	for(char c : buf)
	{
		if(c == '\n')
		{
			lines.push_back(std::move(line));
			line.clear();
			if(lines.size() >= MAX_LINES)
			{
				break;
			}
		}
		else if(c == '\t')
		{
			line += "    ";
		}
		else if(c != '\r' && c != 0x1b) // no escape sequences in the TUI
		{
			line += c;
		}
	}
	if(!line.empty() && lines.size() < MAX_LINES)
	{
		lines.push_back(std::move(line));
	}
	m_textPreview.m_lines = std::move(lines);
	return m_textPreview.m_lines;
}

Element Browser::renderPreview()
{
	const Entry* e = selectedEntry();
	if(!e)
	{
		return text("");
	}
	if(e->m_kind == Entry::Kind::DIR)
	{
		const auto& children = cachedListing(e->m_path);
		// The rest of the width, beside the two columns and their dividers.
		const auto [parentW, currentW] = columnWidths();
		return renderEntryList(children,
		                       -1,
		                       false,
		                       nullptr,
		                       Terminal::Size().dimx - parentW - currentW - 2,
		                       m_previewBoxes);
	}
	if(!e->isImage())
	{
		const auto& lines = textPreview(e->m_path);
		const bool focused = m_previewFocus && lines;
		m_previewFocus = focused;
		Elements rows{text(e->m_name) | bold |
		                  (focused ? color(Color::Green) : nothing),
		              text(humanSize(e->m_size)) | dim};
		if(lines)
		{
			// Only the visible slice: files can be long.
			const int total = static_cast<int>(lines->size());
			const int visible = std::max(1, Terminal::Size().dimy - 6);
			m_previewScroll =
			    std::clamp(m_previewScroll, 0, std::max(0, total - visible));
			rows.push_back(separatorLight() |
			               (focused ? color(Color::Green) : nothing));
			const int end = std::min(total, m_previewScroll + visible);
			for(int i = m_previewScroll; i < end; ++i)
			{
				rows.push_back(hbox({
				    text(fmt::format("{:>4} ", i + 1)) | dim,
				    text((*lines)[i]),
				}));
			}
			rows.push_back(filler());
			rows.push_back(focused
			                   ? ui::paneHints(fmt::format("{}-{}/{}",
			                                               m_previewScroll + 1,
			                                               end,
			                                               total),
			                                   {{"j/k ^d/^u gg G", "scroll"},
			                                    {"h", "back"}})
			                   : ui::paneHints(fmt::format("{} lines", total),
			                                   {{"l", "to scroll"}}));
		}
		return vbox(std::move(rows)) | reflect(m_previewBox);
	}

	fs::path p = e->m_path;
	ImageInfoPtr info = m_ctx.m_svc.info(p);
	if(!info)
	{
		if(auto err = m_ctx.m_svc.error(p))
		{
			return paragraph("error: " + *err) | color(Color::Red);
		}
		return text("loading…") | dim | center;
	}
	// Decode at roughly the preview's resolution.
	auto label = info->m_layers.front().label();
	int reduce = m_ctx.reduceFor(*m_previewSlot,
	                             info->fitBounds(label),
	                             std::nullopt,
	                             TOTAL_PIXEL_BUDGET);
	LayerImagePtr img = m_ctx.m_svc.layerBestEffort(p, label, reduce);

	const auto& part = info->m_parts.front();
	Elements lines;
	lines.push_back(text(e->m_name) | bold);
	lines.push_back(text(fmt::format("{} · {}x{} · {}{}",
	                                 info->m_format,
	                                 part.m_displayWindow.width(),
	                                 part.m_displayWindow.height(),
	                                 part.m_compression,
	                                 part.m_tiled ? " · tiled" : "")) |
	                dim);
	if(img && img->incomplete())
	{
		lines.push_back(text("incomplete image") | color(Color::Yellow));
	}
	if(img && img->damaged())
	{
		lines.push_back(
		    text(fmt::format("damaged image: {} chunk{} fail{} "
		                     "their checksum",
		                     img->m_chunksDamaged,
		                     img->m_chunksDamaged == 1 ? "" : "s",
		                     img->m_chunksDamaged == 1 ? "s" : "")) |
		    color(Color::Yellow));
	}
	if(!(part.m_dataWindow == part.m_displayWindow))
	{
		lines.push_back(text(fmt::format("data window {}x{} @ [{}, {}]",
		                                 part.m_dataWindow.width(),
		                                 part.m_dataWindow.height(),
		                                 part.m_dataWindow.m_x0,
		                                 part.m_dataWindow.m_y0)) |
		                dim);
	}
	if(e->m_kind == Entry::Kind::SEQUENCE)
	{
		lines.push_back(text(fmt::format("{} frames [{}] · {}",
		                                 e->m_frames.size(),
		                                 e->rangeString(),
		                                 humanSize(e->m_size))) |
		                color(Color::Magenta));
	}
	else
	{
		lines.push_back(text(humanSize(e->m_size)) | dim);
	}
	if(info->m_parts.size() > 1)
	{
		lines.push_back(text(fmt::format("{} parts", info->m_parts.size())) |
		                dim);
	}
	std::string layers;
	for(size_t i = 0; i < info->m_layers.size() && i < 12; ++i)
	{
		layers += (i ? "  " : "") + info->m_layers[i].label();
	}
	if(info->m_layers.size() > 12)
	{
		layers += fmt::format("  … +{}", info->m_layers.size() - 12);
	}
	lines.push_back(paragraph(fmt::format("{} layer{}: {}",
	                                      info->m_layers.size(),
	                                      info->m_layers.size() == 1 ? "" : "s",
	                                      layers)) |
	                color(Color::Cyan));

	// The viewer's colour management (OCIO view), at default exposure.
	DisplayParams disp;
	disp.m_ocio = m_ctx.m_colour.transformFor(p,
	                                          sourceKey(Source{*e}),
	                                          info->m_displayReferred);
	const auto err = img ? std::nullopt : m_ctx.m_svc.error(p);
	Element image =
	    img   ? m_previewSlot->element(img, ViewParams{}, disp)
	    : err ? paragraph("error: " + *err) | color(Color::Red) | center | flex
	          : text("decoding…") | dim | center | flex;
	return vbox({image | flex, separatorLight(), vbox(std::move(lines))});
}

bool Browser::refresh()
{
	bool changed = false;
	// Current directory: keep the selection on the same entry by name.
	std::vector<Entry> fresh = currentListing();
	if(!sameListing(fresh, m_entries))
	{
		std::string keep = selectedEntry() ? selectedEntry()->m_name : "";
		m_entries = std::move(fresh);
		auto vis = visibleIndices();
		for(size_t i = 0; i < vis.size(); ++i)
		{
			if(m_entries[vis[i]].m_name == keep)
			{
				m_sel = static_cast<int>(i);
			}
		}
		changed = true;
	}
	// Parent / previewed directories shown from the listing cache.
	for(auto& [dir, entries] : m_listingCache)
	{
		std::vector<Entry> again = listDirectory(dir, m_showHidden);
		if(!sameListing(again, entries))
		{
			entries = std::move(again);
			changed = true;
		}
	}
	return changed;
}

std::vector<fs::path> Browser::shownDirs() const
{
	std::vector<fs::path> dirs{m_cwd};
	for(const auto& [dir, entries] : m_listingCache)
	{
		dirs.emplace_back(dir);
	}
	return dirs;
}

std::pair<int, int> Browser::columnWidths() const
{
	const int w = Terminal::Size().dimx;
	return {std::max(6, static_cast<int>(w * m_parentFrac)),
	        std::max(6, static_cast<int>(w * m_currentFrac))};
}

Element Browser::render()
{
	auto [parentW, currentW] = columnWidths();

	// Parent column with the current dir selected.
	Element parentCol = text("");
	fs::path parent = m_cwd.parent_path();
	if(parent != m_cwd && !parent.empty())
	{
		const auto& pe = cachedListing(parent);
		int psel = -1;
		for(size_t i = 0; i < pe.size(); ++i)
		{
			if(pe[i].m_path == m_cwd)
			{
				psel = static_cast<int>(i);
			}
		}
		parentCol =
		    renderEntryList(pe, psel, false, nullptr, parentW, m_parentBoxes);
	}
	else
	{
		m_parentBoxes = {};
	}
	auto vis = visibleIndices();
	m_sel = std::clamp(m_sel, 0, std::max(0, static_cast<int>(vis.size()) - 1));
	Element currentCol =
	    renderEntryList(m_entries, m_sel, true, &vis, currentW, m_currentBoxes);

	// Top bar: the filter prompt (first, so a long path never hides it),
	// then the directory.
	Elements top;
	if(m_filtering || !m_filter.empty())
	{
		top.push_back(text(" / ") | bgcolor(Color::Yellow) |
		              color(Color::Black) | bold);
		top.push_back(text(" " + m_filter + (m_filtering ? "▏" : "") + " ") |
		              color(Color::Yellow) | bold);
		top.push_back(text(fmt::format("{} match{}  {}  ",
		                               vis.size(),
		                               vis.size() == 1 ? "" : "es",
		                               m_filtering ? "Enter keep · Esc clear"
		                                           : "Esc clear")) |
		              dim);
	}
	top.push_back(text(" " + m_cwd.string() + " ") | bold);
	Element topBar = hbox(std::move(top)) | bgcolor(Color::GrayDark);

	std::string right;
	if(!m_marks.empty())
	{
		right += fmt::format("{} marked  ", m_marks.size());
	}
	right +=
	    fmt::format("{}/{}   / filter  e expand  space mark  ? help  q quit",
	                vis.empty() ? 0 : m_sel + 1,
	                vis.size());

	return vbox({
	    topBar,
	    hbox({
	        parentCol | size(WIDTH, EQUAL, parentW),
	        separator(),
	        currentCol | size(WIDTH, EQUAL, currentW),
	        separator(),
	        renderPreview() | flex,
	    }) | flex,
	    m_ctx.statusLine(right),
	});
}

} // namespace rv
