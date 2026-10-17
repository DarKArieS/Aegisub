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
#include <wx/font.h>
#include <wx/window.h>

#include "grid_column.h"

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
/// position.
///
/// The file shown here is the subtitles controller's secondary file. Clicking
/// the pane swaps it with the active file so that it can be edited, and the
/// two panes trade places so each file stays on the same side of the window.
class CompareGrid final : public wxWindow {
	agi::Context *c;
	BaseGrid *grid;
	std::vector<agi::signal::Connection> connections;

	/// Is the active file (shown in the main grid) on the left?
	bool active_on_left = true;

	struct RefLine {
		const AssDialogue *line;
		bool overlaps; ///< Does it overlap the main line it's attached to?
	};
	/// Main grid row -> reference lines attached to it
	std::vector<std::vector<RefLine>> row_refs;
	/// Main grid row -> main line
	std::vector<AssDialogue *> rows;
	bool maps_dirty = true;

	int scroll_wheel_progress = 0;

	/// Same columns as the main grid, sized for this pane's file
	std::vector<std::unique_ptr<GridColumn>> columns = GetGridColumns();
	std::unique_ptr<WidthHelper> width_helper;
	bool widths_dirty = true;
	bool by_frame = false;
	/// Font the column widths were last calculated with
	wxFont width_font;

	void OnPaint(wxPaintEvent &);
	void OnMouse(wxMouseEvent &evt);
	void OnCommit(int type);
	void OnGridRepaint();

	void OnSecondaryChanged(bool swapped);
	void OnContextMenu(wxContextMenuEvent &evt);

	/// Load this pane's column visibility from the option for its side
	void ApplyColumnOption();
	void SetColumnWidths();

	void UpdateMaps();
	void UpdateLayout();
	/// Make the file shown in this pane the one being edited
	/// @param row Main grid row whose reference line should become active, or -1
	void Activate(int row = -1);

public:
	CompareGrid(wxWindow *parent, agi::Context *c);
	~CompareGrid();

	/// Load a subtitle file as the comparison file
	/// @return false if the load was cancelled or failed
	bool Load(agi::fs::path const& path);
	/// Unload the comparison file and hide the pane
	/// @return false if the user cancelled
	bool Close();
	/// Swap the file being edited with the comparison file
	void Swap() { Activate(); }
	bool IsLoaded() const;

	/// Get the time span of the comparison lines matched to a line of the
	/// file being edited
	/// @return false if no comparison lines are matched to it
	bool GetMatchedTimes(const AssDialogue *line, int &start, int &end);
};
