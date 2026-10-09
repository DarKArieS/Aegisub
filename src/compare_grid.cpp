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
#include "compat.h"
#include "format.h"
#include "options.h"
#include "project.h"
#include "selection_controller.h"
#include "subtitle_format.h"
#include "utils.h"
#include "video_controller.h"

#include <libaegisub/exception.h>

#include <algorithm>
#include <limits>
#include <numeric>

#include <wx/dcbuffer.h>
#include <wx/menu.h>
#include <wx/msgdlg.h>
#include <wx/splitter.h>

namespace {
wxString display_text(AssDialogue const& d) {
	int mode = OPT_GET("Subtitle/Grid/Hide Overrides")->GetInt();
	if (mode == 0) return to_wx(d.Text);

	wxString replace_char = to_wx(OPT_GET("Subtitle/Grid/Hide Overrides Char")->GetString());
	auto const& text = d.Text.get();
	wxString str;
	size_t start = 0, pos;
	while ((pos = text.find('{', start)) != std::string::npos) {
		str += to_wx(text.substr(start, pos - start));
		if (mode == 1)
			str += replace_char;
		start = text.find('}', pos);
		if (start != std::string::npos) ++start;
	}
	if (start != std::string::npos)
		str += to_wx(text.substr(start));
	return str;
}
}

CompareGrid::CompareGrid(wxWindow *parent, agi::Context *c)
: wxWindow(parent, -1, wxDefaultPosition, wxDefaultSize, wxSUNKEN_BORDER)
, c(c)
, grid(c->subsGrid)
{
	SetBackgroundStyle(wxBG_STYLE_PAINT);
	Hide();

	connections = agi::signal::make_vector({
		c->ass->AddCommitListener(&CompareGrid::OnCommit, this),
		grid->AddRepaintListener(&CompareGrid::OnGridRepaint, this),
	});

	Bind(wxEVT_PAINT, &CompareGrid::OnPaint, this);
	Bind(wxEVT_SIZE, [this](wxSizeEvent &) { Refresh(false); });
	Bind(wxEVT_LEFT_DOWN, &CompareGrid::OnMouse, this);
	Bind(wxEVT_LEFT_DCLICK, &CompareGrid::OnMouse, this);
	Bind(wxEVT_MOUSEWHEEL, &CompareGrid::OnMouse, this);
	Bind(wxEVT_CONTEXT_MENU, [this](wxContextMenuEvent &) {
		wxMenu menu;
		menu.Append(wxID_CLOSE, _("&Close Comparison Subtitles"));
		menu.Bind(wxEVT_MENU, [this](wxCommandEvent &) { Close(); }, wxID_CLOSE);
		PopupMenu(&menu);
	});
}

CompareGrid::~CompareGrid() { }

bool CompareGrid::Load(agi::fs::path const& path) {
	auto show_error = [&](wxString const& message) {
		wxMessageBox(message, _("Error loading file"), wxOK | wxICON_ERROR | wxCENTER, c->parent);
	};

	try {
		std::string encoding = CharSetDetect::GetEncoding(path);
		auto reader = SubtitleFormat::GetReader(path, encoding.c_str());
		auto file = std::make_unique<AssFile>();
		reader->ReadFile(file.get(), path, c->project->Timecodes(), encoding.c_str());
		ref = std::move(file);
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

	filename = path;
	maps_dirty = true;
	SetShown(true);
	Refresh(false);
	return true;
}

void CompareGrid::Close() {
	ref.reset();
	filename.clear();
	row_refs.clear();
	maps_dirty = true;
	SetShown(false);
}

void CompareGrid::SetShown(bool show) {
	auto splitter = dynamic_cast<wxSplitterWindow *>(GetParent());
	if (!splitter) {
		Show(show);
		GetParent()->Layout();
		return;
	}

	if (show && !splitter->IsSplit())
		splitter->SplitVertically(grid, this, splitter->GetClientSize().GetWidth() / 2);
	else if (!show && splitter->IsSplit())
		splitter->Unsplit(this);
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

	int number = 0;
	for (auto const& r : ref->Events) {
		++number;
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
			row_refs[best].push_back({&r, number, best_overlaps});
	}
}

void CompareGrid::OnPaint(wxPaintEvent &) {
	if (maps_dirty) UpdateMaps();

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
	wxBrush bg_left(to_wx(OPT_GET("Colour/Subtitle Grid/Left Column")->GetColor()));
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

	const int num_w = dc.GetTextExtent(wxS("00000+")).GetWidth() + 8;
	const int time_w = dc.GetTextExtent(wxS("0:00:00.00")).GetWidth() + 8;
	const int col_x[] = {0, num_w, num_w + time_w, num_w + time_w * 2};

	auto draw_cell = [&](wxString const& str, int col, int y) {
		dc.DrawText(str, col_x[col] + 4, y + 2);
	};

	// Header
	dc.SetPen(*wxTRANSPARENT_PEN);
	dc.SetBrush(bg_left);
	dc.DrawRectangle(0, line_height, num_w, h - line_height);
	dc.SetBrush(bg_header);
	dc.DrawRectangle(0, 0, w, line_height);
	dc.SetTextForeground(text_standard);
	draw_cell(wxS("#"), 0, 0);
	draw_cell(_("Start"), 1, 0);
	draw_cell(_("End"), 2, 0);
	draw_cell(_("Reference: ") + filename.filename().wstring(), 3, 0);

	dc.SetPen(grid_pen);
	dc.DrawLine(0, 0, w, 0);
	dc.DrawLine(0, line_height, w, line_height);

	const int rows_on_screen = h / line_height + 1;
	const int n_draw = mid(0, rows_on_screen, (int)rows.size() - y_pos);
	auto const& selection = c->selectionController->GetSelectedSet();
	const auto active_line = c->selectionController->GetActiveLine();

	for (int i = 0; i < n_draw; ++i) {
		const int row = y_pos + i;
		const int y = (i + 1) * line_height;
		AssDialogue *main_line = rows[row];

		// Mirror the main grid's row colours so rows are easy to match up
		bool in_sel = !!selection.count(main_line);
		wxBrush const* brush = nullptr;
		if (in_sel && main_line->Comment) brush = &bg_sel_comment;
		else if (in_sel) brush = &bg_selection;
		else if (main_line->Comment) brush = &bg_comment;
		if (brush) {
			dc.SetPen(*wxTRANSPARENT_PEN);
			dc.SetBrush(*brush);
			dc.DrawRectangle(num_w, y + 1, w, line_height);
		}

		auto const& refs = row_refs[row];
		if (!refs.empty()) {
			int start = std::numeric_limits<int>::max(), end = 0;
			bool any_overlap = false;
			wxString text;
			for (auto const& r : refs) {
				start = std::min<int>(start, r.line->Start);
				end = std::max<int>(end, r.line->End);
				any_overlap |= r.overlaps;
				if (!text.empty()) text += wxS(" / ");
				text += display_text(*r.line);
			}
			if (text.size() > 512)
				text = text.Left(512) + "...";

			wxColour base = !any_overlap ? text_faded : in_sel ? text_selection : text_standard;
			wxString number = std::to_wstring(refs.front().number);
			if (refs.size() > 1) number += wxS("+");

			dc.SetTextForeground(base);
			draw_cell(number, 0, y);
			dc.SetTextForeground(start != (int)main_line->Start ? text_diff : base);
			draw_cell(to_wx(agi::Time(start).GetAssFormatted()), 1, y);
			dc.SetTextForeground(end != (int)main_line->End ? text_diff : base);
			draw_cell(to_wx(agi::Time(end).GetAssFormatted()), 2, y);
			dc.SetTextForeground(base);
			dc.SetClippingRegion(col_x[3], y, w - col_x[3], line_height);
			draw_cell(text, 3, y);
			dc.DestroyClippingRegion();
		}

		dc.SetPen(grid_pen);
		dc.DrawLine(0, y + line_height, w, y + line_height);
	}

	const int max_h = (n_draw + 1) * line_height;
	dc.SetPen(grid_pen);
	for (int x : col_x)
		dc.DrawLine(x, 0, x, max_h);
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

	if (maps_dirty) UpdateMaps();
	int row = evt.GetY() / line_height + y_pos - 1;
	if (evt.GetY() < line_height || row < 0 || row >= (int)rows.size())
		return;

	// Clicking a row selects the main line it is aligned with, so that the
	// grid scroll position is preserved
	AssDialogue *line = rows[row];
	c->selectionController->SetSelectionAndActive({line}, line);
	grid->ScrollTo(y_pos);

	if (evt.LeftDClick() && !row_refs[row].empty())
		c->videoController->JumpToTime(row_refs[row].front().line->Start);
}
