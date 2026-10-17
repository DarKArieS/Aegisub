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

/// @file compare_grid.cpp
/// @brief Read-only reference subtitle pane shown beside the main grid
/// @ingroup main_ui

#include "compare_grid.h"

#include "include/aegisub/context.h"

#include "ass_dialogue.h"
#include "ass_file.h"
#include "base_grid.h"
#include "charset_detect.h"
#include "command/command.h"
#include "compat.h"
#include "format.h"
#include "options.h"
#include "selection_controller.h"
#include "subs_controller.h"
#include "utils.h"

#include <libaegisub/exception.h>

#include <algorithm>
#include <limits>
#include <numeric>

#include <wx/dcbuffer.h>
#include <wx/menu.h>
#include <wx/msgdlg.h>
#include <wx/splitter.h>

namespace {
const char *column_option(bool left) {
	return left ? "Subtitle/Grid/Column" : "Subtitle/Grid/Column Right";
}

// Indices into GetGridColumns()
const size_t COL_START = 2;
const size_t COL_END = 3;

// Check menu.h for id range allocation before editing this enum
const int MENU_SHOW_COL = (wxID_HIGHEST + 1) + 2100; // Needs 15 IDs after this
}

CompareGrid::CompareGrid(wxWindow *parent, agi::Context *c)
: wxWindow(parent, -1, wxDefaultPosition, wxDefaultSize, wxSUNKEN_BORDER)
, c(c)
, grid(c->subsGrid)
{
	SetBackgroundStyle(wxBG_STYLE_PAINT);
	Hide();
	ApplyColumnOption();

	connections = agi::signal::make_vector({
		c->ass->AddCommitListener(&CompareGrid::OnCommit, this),
		c->subsController->AddSecondaryChangedListener(&CompareGrid::OnSecondaryChanged, this),
		c->subsController->AddFileSaveListener([this] { Refresh(false); }),
		grid->AddRepaintListener(&CompareGrid::OnGridRepaint, this),
	});

	Bind(wxEVT_PAINT, &CompareGrid::OnPaint, this);
	Bind(wxEVT_SIZE, [this](wxSizeEvent &) { Refresh(false); });
	Bind(wxEVT_LEFT_DOWN, &CompareGrid::OnMouse, this);
	Bind(wxEVT_MOUSEWHEEL, &CompareGrid::OnMouse, this);
	Bind(wxEVT_CONTEXT_MENU, &CompareGrid::OnContextMenu, this);

	// Remember where the user puts the boundary between the two files
	if (auto splitter = dynamic_cast<wxSplitterWindow *>(parent)) {
		splitter->Bind(wxEVT_SPLITTER_SASH_POS_CHANGED, [splitter](wxSplitterEvent &evt) {
			int width = splitter->GetClientSize().GetWidth();
			if (width > 0)
				OPT_SET("Subtitle/Grid/Compare Split")->SetDouble((double)evt.GetSashPosition() / width);
			evt.Skip();
		});
	}
}

void CompareGrid::OnContextMenu(wxContextMenuEvent &evt) {
	wxPoint pos = evt.GetPosition();
	bool on_header = pos != wxDefaultPosition && ScreenToClient(pos).y <= grid->GetLineHeight();

	wxMenu menu;
	if (on_header) {
		// Same column chooser as the main grid, stored separately for each side
		for (size_t i = 0; i < columns.size(); ++i) {
			if (columns[i]->CanHide())
				menu.Append(MENU_SHOW_COL + i, columns[i]->Description(), "", wxITEM_CHECK)->Check(columns[i]->Visible());
		}
		menu.Bind(wxEVT_MENU, [this](wxCommandEvent &e) {
			size_t item = e.GetId() - MENU_SHOW_COL;
			auto option = column_option(!active_on_left);
			auto visible = OPT_GET(option)->GetListBool();
			visible.resize(columns.size(), true);
			visible[item] = !visible[item];
			OPT_SET(option)->SetListBool(visible);
			ApplyColumnOption();
		}, MENU_SHOW_COL, MENU_SHOW_COL + 15);
	}
	else {
		menu.Append(wxID_EDIT, _("&Edit This File"));
		menu.Append(wxID_APPLY, _("Apply These &Times to the Selected Lines"))
			->Enable(!c->selectionController->GetSelectedSet().empty());
		menu.AppendSeparator();
		menu.Append(wxID_CLOSE, _("&Close Comparison Subtitles"));
		menu.Bind(wxEVT_MENU, [this](wxCommandEvent &) { Activate(); }, wxID_EDIT);
		menu.Bind(wxEVT_MENU, [this](wxCommandEvent &) { cmd::call("subtitle/compare/sync_time", c); }, wxID_APPLY);
		menu.Bind(wxEVT_MENU, [this](wxCommandEvent &) { Close(); }, wxID_CLOSE);
	}
	PopupMenu(&menu);
}

void CompareGrid::ApplyColumnOption() {
	auto visible = OPT_GET(column_option(!active_on_left))->GetListBool();
	for (size_t i = 0; i < columns.size(); ++i)
		columns[i]->SetVisible(i >= visible.size() || visible[i]);
	widths_dirty = true;
	Refresh(false);
}

void CompareGrid::SetColumnWidths() {
	widths_dirty = false;
	width_font = grid->GetGridFont();
	auto file = c->subsController->SecondaryFile();
	if (!file) return;

	wxClientDC dc(this);
	dc.SetFont(width_font);
	if (!width_helper)
		width_helper = std::make_unique<WidthHelper>();
	width_helper->SetDC(&dc);
	for (auto& column : columns)
		column->UpdateWidth(c, *file, *width_helper);
	width_helper->Age();
}

CompareGrid::~CompareGrid() { }

bool CompareGrid::IsLoaded() const {
	return c->subsController->HasSecondary();
}

bool CompareGrid::GetMatchedTimes(const AssDialogue *line, int &start, int &end) {
	if (!IsLoaded()) return false;
	if (maps_dirty) UpdateMaps();

	size_t row = line->Row;
	if (row >= rows.size() || rows[row] != line || row_refs[row].empty())
		return false;

	start = std::numeric_limits<int>::max();
	end = std::numeric_limits<int>::min();
	for (auto const& r : row_refs[row]) {
		start = std::min<int>(start, r.line->Start);
		end = std::max<int>(end, r.line->End);
	}
	return true;
}

bool CompareGrid::Load(agi::fs::path const& path) {
	if (c->subsController->TryToCloseSecondary() == wxCANCEL)
		return false;

	auto show_error = [&](wxString const& message) {
		wxMessageBox(message, _("Error loading file"), wxOK | wxICON_ERROR | wxCENTER, c->parent);
	};

	try {
		std::string encoding = CharSetDetect::GetEncoding(path);
		c->subsController->LoadSecondary(path, encoding.c_str());
	}
	catch (agi::UserCancelException const&) { return false; }
	catch (agi::fs::FileNotFound const&) {
		show_error(fmt_tl("%s not found.", path.string()));
		return false;
	}
	catch (agi::Exception const& e) {
		show_error(to_wx(e.GetMessage()));
		return false;
	}
	catch (std::exception const& e) {
		show_error(to_wx(e.what()));
		return false;
	}
	return true;
}

bool CompareGrid::Close() {
	if (c->subsController->TryToCloseSecondary() == wxCANCEL)
		return false;
	c->subsController->CloseSecondary();
	return true;
}

void CompareGrid::OnSecondaryChanged(bool swapped) {
	maps_dirty = true;
	widths_dirty = true;
	// Each file stays on its own side, so the main grid moves to whichever
	// side the newly active file is on
	if (swapped)
		active_on_left = !active_on_left;
	if (!IsLoaded())
		active_on_left = true;
	UpdateLayout();
	Refresh(false);
}

void CompareGrid::UpdateLayout() {
	auto splitter = dynamic_cast<wxSplitterWindow *>(GetParent());
	if (!splitter) {
		Show(IsLoaded());
		GetParent()->Layout();
		return;
	}

	// Each side of the window has its own set of visible columns, which
	// follows whichever pane is currently on that side
	grid->SetColumnOption(column_option(active_on_left));
	ApplyColumnOption();

	if (!IsLoaded()) {
		if (splitter->IsSplit())
			splitter->Unsplit(this);
		return;
	}

	wxWindow *left = active_on_left ? static_cast<wxWindow *>(grid) : this;
	wxWindow *right = active_on_left ? static_cast<wxWindow *>(this) : grid;
	if (splitter->IsSplit() && splitter->GetWindow1() == left)
		return;

	int sash = (int)(splitter->GetClientSize().GetWidth() * OPT_GET("Subtitle/Grid/Compare Split")->GetDouble());
	splitter->Freeze();
	if (splitter->IsSplit()) {
		// Keep the boundary where it was on screen even though the panes swap
		sash = splitter->GetSashPosition();
		splitter->Unsplit();
	}
	splitter->SplitVertically(left, right, sash);
	splitter->Thaw();
}

void CompareGrid::Activate(int row) {
	if (!IsLoaded()) return;
	if (maps_dirty) UpdateMaps();

	// Line in this pane that should become the active line, and the screen
	// row it should stay on so that the view doesn't jump
	int line_id = -1;
	int screen_row = 0;
	if (row >= 0 && row < (int)row_refs.size() && !row_refs[row].empty()) {
		line_id = row_refs[row].front().line->Id;
		screen_row = row - grid->GetFirstVisibleRow();
	}

	// The grid overwrites the file's stored scroll position when it refreshes
	// after the swap, so grab it first
	const int scroll_position = c->subsController->SecondaryFile()->Properties.scroll_position;
	c->subsController->SwapSecondary();

	if (line_id < 0) {
		grid->ScrollTo(scroll_position);
		return;
	}
	for (auto& line : c->ass->Events) {
		if (line.Id == line_id) {
			c->selectionController->SetSelectionAndActive({&line}, &line);
			grid->ScrollTo(line.Row - screen_row);
			break;
		}
	}
}

void CompareGrid::OnCommit(int type) {
	if (type == AssFile::COMMIT_NEW || type & (AssFile::COMMIT_ORDER | AssFile::COMMIT_DIAG_ADDREM | AssFile::COMMIT_DIAG_TIME | AssFile::COMMIT_DIAG_META))
		maps_dirty = true;
	if (IsShown())
		Refresh(false);
}

void CompareGrid::OnGridRepaint() {
	if (IsShown())
		Refresh(false);
}

void CompareGrid::UpdateMaps() {
	maps_dirty = false;
	rows.clear();
	row_refs.clear();

	for (auto& line : c->ass->Events)
		rows.push_back(&line);
	row_refs.resize(rows.size());
	auto ref = c->subsController->SecondaryFile();
	if (!ref || rows.empty()) return;

	// Main rows sorted by start time, so that only the rows which can
	// possibly overlap a reference line need to be examined
	std::vector<size_t> order(rows.size());
	std::iota(order.begin(), order.end(), 0);
	std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
		return rows[a]->Start < rows[b]->Start;
	});
	int max_duration = 0;
	for (auto line : rows)
		max_duration = std::max<int>(max_duration, line->End - line->Start);

	for (auto const& r : ref->Events) {
		if (r.Comment) continue;
		const int rs = r.Start, re = r.End;

		size_t best = std::numeric_limits<size_t>::max();
		bool best_overlaps = false;
		int best_score = std::numeric_limits<int>::min();

		// Higher score wins; non-comment lines beat comments on ties so that
		// commented-out originals don't steal matches from the actual lines
		auto consider = [&](size_t row, int score, bool overlaps) {
			auto line = rows[row];
			if (best != std::numeric_limits<size_t>::max()) {
				if (score < best_score) return;
				if (score == best_score) {
					bool cur_comment = rows[best]->Comment;
					if (line->Comment && !cur_comment) return;
					if (line->Comment == cur_comment && row > best) return;
				}
			}
			best = row;
			best_score = score;
			best_overlaps = overlaps;
		};

		auto ub = std::partition_point(order.begin(), order.end(), [&](size_t i) {
			return (int)rows[i]->Start < re;
		});

		std::vector<size_t> window;
		for (auto it = ub; it != order.begin(); ) {
			--it;
			auto line = rows[*it];
			if ((int)line->Start + max_duration < rs) break;
			window.push_back(*it);
			int overlap = std::min<int>(re, line->End) - std::max<int>(rs, line->Start);
			if (overlap > 0)
				consider(*it, overlap, true);
		}

		// Nothing overlaps, so attach it to the closest line instead
		if (best == std::numeric_limits<size_t>::max()) {
			if (ub != order.end()) window.push_back(*ub);
			if (ub != order.begin()) window.push_back(*(ub - 1));
			for (size_t row : window) {
				auto line = rows[row];
				int gap = (int)line->Start >= re ? (int)line->Start - re : std::max(0, rs - (int)line->End);
				consider(row, -gap, false);
			}
		}

		if (best != std::numeric_limits<size_t>::max())
			row_refs[best].push_back({&r, best_overlaps});
	}
}

void CompareGrid::OnPaint(wxPaintEvent &) {
	if (maps_dirty) UpdateMaps();
	if (by_frame != grid->IsByFrame()) {
		by_frame = grid->IsByFrame();
		for (auto& column : columns)
			column->SetByFrame(by_frame);
		widths_dirty = true;
	}
	if (widths_dirty || width_font != grid->GetGridFont())
		SetColumnWidths();

	wxAutoBufferedPaintDC dc(this);
	int w, h;
	GetClientSize(&w, &h);

	const int line_height = grid->GetLineHeight();
	const int y_pos = grid->GetFirstVisibleRow();
	dc.SetFont(grid->GetGridFont());

	wxBrush bg_default(to_wx(OPT_GET("Colour/Subtitle Grid/Background/Background")->GetColor()));
	wxBrush bg_header(to_wx(OPT_GET("Colour/Subtitle Grid/Header")->GetColor()));
	wxBrush bg_selection(to_wx(OPT_GET("Colour/Subtitle Grid/Background/Selection")->GetColor()));
	wxBrush bg_comment(to_wx(OPT_GET("Colour/Subtitle Grid/Background/Comment")->GetColor()));
	wxBrush bg_sel_comment(to_wx(OPT_GET("Colour/Subtitle Grid/Background/Selected Comment")->GetColor()));
	// The line number column marks this pane as the file not being edited
	wxBrush bg_left(to_wx(OPT_GET("Colour/Subtitle Grid/Left Column Inactive")->GetColor()));
	wxColour text_standard(to_wx(OPT_GET("Colour/Subtitle Grid/Standard")->GetColor()));
	wxColour text_selection(to_wx(OPT_GET("Colour/Subtitle Grid/Selection")->GetColor()));
	wxColour text_diff(to_wx(OPT_GET("Colour/Subtitle Grid/Collision")->GetColor()));
	wxPen grid_pen(to_wx(OPT_GET("Colour/Subtitle Grid/Lines")->GetColor()));

	// Reference lines attached to a row they don't overlap are drawn faded
	wxColour bg_col = bg_default.GetColour();
	wxColour text_faded(
		(text_standard.Red() + bg_col.Red()) / 2,
		(text_standard.Green() + bg_col.Green()) / 2,
		(text_standard.Blue() + bg_col.Blue()) / 2);

	dc.SetBackground(bg_default);
	dc.Clear();

	// Header, laid out the same way as the main grid's
	dc.SetPen(*wxTRANSPARENT_PEN);
	dc.SetBrush(bg_left);
	dc.DrawRectangle(0, line_height, columns[0]->Width(), h - line_height);
	dc.SetBrush(bg_header);
	dc.DrawRectangle(0, 0, w, line_height);
	dc.SetTextForeground(text_standard);
	{
		int x = 0;
		for (auto const& column : columns) {
			if (column->Width()) {
				wxString const& str = column->Header();
				int left = x + 4;
				if (column->Centered())
					left += (column->Width() - 6 - dc.GetTextExtent(str).GetWidth()) / 2;
				dc.DrawText(str, left, 2);
			}
			x += column->Width();
		}
	}

	dc.SetPen(grid_pen);
	dc.DrawLine(0, 0, w, 0);
	dc.DrawLine(0, line_height, w, line_height);

	const int rows_on_screen = h / line_height + 1;
	const int n_draw = mid(0, rows_on_screen, (int)rows.size() - y_pos);
	auto const& selection = c->selectionController->GetSelectedSet();
	const auto active_line = c->selectionController->GetActiveLine();
	const int grid_x = columns[0]->Width();

	for (int i = 0; i < n_draw; ++i) {
		const int row = y_pos + i;
		const int y = (i + 1) * line_height;
		AssDialogue *main_line = rows[row];

		// Mirror the main grid's row colours so rows are easy to match up
		bool in_sel = !!selection.count(main_line);
		wxBrush row_brush = bg_default;
		if (in_sel && main_line->Comment) row_brush = bg_sel_comment;
		else if (in_sel) row_brush = bg_selection;
		else if (main_line->Comment) row_brush = bg_comment;
		dc.SetBrush(row_brush);
		if (row_brush != bg_default) {
			dc.SetPen(*wxTRANSPARENT_PEN);
			dc.DrawRectangle(grid_x, y + 1, w, line_height);
		}

		auto const& refs = row_refs[row];
		if (!refs.empty()) {
			// Several reference lines attached to one row are shown as a
			// single line spanning all of them
			const AssDialogue *line = refs.front().line;
			std::unique_ptr<AssDialogue> merged;
			bool any_overlap = false;
			for (auto const& r : refs)
				any_overlap |= r.overlaps;
			if (refs.size() > 1) {
				merged = std::make_unique<AssDialogue>(*line);
				merged->Row = line->Row;
				std::string text;
				for (auto const& r : refs) {
					merged->Start = std::min<int>(merged->Start, r.line->Start);
					merged->End = std::max<int>(merged->End, r.line->End);
					if (!text.empty()) text += " / ";
					text += r.line->Text.get();
				}
				merged->Text = text;
				line = merged.get();
			}

			wxColour base = !any_overlap ? text_faded : in_sel ? text_selection : text_standard;
			int x = 0;
			for (size_t j = 0; j < columns.size(); ++j) {
				if (columns[j]->Width()) {
					bool differs = (j == COL_START && line->Start != main_line->Start)
						|| (j == COL_END && line->End != main_line->End);
					dc.SetTextForeground(differs ? text_diff : base);
					columns[j]->Paint(dc, x, y, line, c);
				}
				x += columns[j]->Width();
			}
		}

		dc.SetPen(grid_pen);
		dc.DrawLine(0, y + line_height, w, y + line_height);
	}

	const int max_h = (n_draw + 1) * line_height;
	dc.SetPen(grid_pen);
	{
		int x = 0;
		for (auto const& column : columns) {
			x += column->Width();
			if (x < w)
				dc.DrawLine(x, 0, x, max_h);
		}
	}
	dc.DrawLine(0, 0, 0, max_h);
	dc.DrawLine(w - 1, 0, w - 1, max_h);

	if (active_line && active_line->Row >= y_pos && active_line->Row < y_pos + n_draw) {
		dc.SetPen(wxPen(to_wx(OPT_GET("Colour/Subtitle Grid/Active Border")->GetColor())));
		dc.SetBrush(*wxTRANSPARENT_BRUSH);
		dc.DrawRectangle(0, (active_line->Row - y_pos + 1) * line_height, w, line_height + 1);
	}
}

void CompareGrid::OnMouse(wxMouseEvent &evt) {
	const int line_height = grid->GetLineHeight();
	const int y_pos = grid->GetFirstVisibleRow();

	if (evt.GetWheelRotation() != 0) {
		if (ForwardMouseWheelEvent(this, evt)) {
			int step = evt.ShiftDown() ? GetClientSize().GetHeight() / line_height - 2 : 3;
			scroll_wheel_progress += evt.GetWheelRotation();
			grid->ScrollTo(y_pos - step * (scroll_wheel_progress / evt.GetWheelDelta()));
			scroll_wheel_progress %= evt.GetWheelDelta();
		}
		return;
	}

	// Clicking switches to editing this file, with the clicked line active
	int row = evt.GetY() < line_height ? -1 : evt.GetY() / line_height + y_pos - 1;
	Activate(row);
}
