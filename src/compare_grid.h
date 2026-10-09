// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.
//
// THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
// WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
// ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
// WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
// ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
// OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
//
// Aegisub Project http://www.aegisub.org/

/// @file compare_grid.h
/// @brief Read-only reference subtitle pane shown beside the main grid
/// @ingroup main_ui

#include <libaegisub/fs.h>
#include <libaegisub/signal.h>

#include <memory>
#include <vector>
#include <wx/window.h>

namespace agi { struct Context; }
class AssDialogue;
class AssFile;
class BaseGrid;

/// @class CompareGrid
/// @brief Displays a second subtitle file next to the main grid
///
/// Every line of the reference file is attached to the main grid row it
/// overlaps the most in time (or the nearest row when it overlaps nothing),
/// and is drawn on the same screen row as that line so that the two files
/// can be compared side by side. The pane follows the main grid's scroll
/// position and never modifies the reference file.
class CompareGrid final : public wxWindow {
	agi::Context *c;
	BaseGrid *grid;
	std::vector<agi::signal::Connection> connections;

	std::unique_ptr<AssFile> ref;
	agi::fs::path filename;

	struct RefLine {
		const AssDialogue *line;
		int number;   ///< 1-based line number in the reference file
		bool overlaps; ///< Does it overlap the main line it's attached to?
	};
	/// Main grid row -> reference lines attached to it
	std::vector<std::vector<RefLine>> row_refs;
	/// Main grid row -> main line
	std::vector<AssDialogue *> rows;
	bool maps_dirty = true;

	int scroll_wheel_progress = 0;

	void OnPaint(wxPaintEvent &);
	void OnMouse(wxMouseEvent &evt);
	void OnCommit(int type);
	void OnGridRepaint();

	void UpdateMaps();
	void SetShown(bool show);

public:
	CompareGrid(wxWindow *parent, agi::Context *c);
	~CompareGrid();

	/// Load a subtitle file as the comparison reference
	/// @return false if the load was cancelled or failed
	bool Load(agi::fs::path const& path);
	/// Unload the reference file and hide the pane
	void Close();
	bool IsLoaded() const { return !!ref; }
};
