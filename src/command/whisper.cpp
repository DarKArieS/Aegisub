// Copyright (c) 2024, Aegisub contributors
//
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

#include "command.h"

#include "../ass_dialogue.h"
#include "../ass_file.h"
#include "../compat.h"
#include "../dialog_progress.h"
#include "../include/aegisub/context.h"
#include "../options.h"
#include "../project.h"
#include "../selection_controller.h"
#include "../whisper_api.h"

#include <libaegisub/fs.h>

#include <wx/msgdlg.h>

#ifdef _WIN32
#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

namespace {
using cmd::Command;

// Minimal RAII wrapper for COM interfaces received from the DLL.
template<typename T>
struct ComPtr {
	T *p = nullptr;
	ComPtr() = default;
	~ComPtr() { if (p) p->Release(); }
	ComPtr(const ComPtr &) = delete;
	ComPtr &operator=(const ComPtr &) = delete;
	T **operator&() { return &p; }
	T *operator->() const { return p; }
	explicit operator bool() const { return p != nullptr; }
};

// Try to load Whisper.dll from the Aegisub executable directory first,
// then fall back to the default DLL search order.
static HMODULE LoadWhisperDll() {
	wchar_t exePath[MAX_PATH];
	if (GetModuleFileNameW(nullptr, exePath, MAX_PATH)) {
		std::wstring path(exePath);
		auto sep = path.rfind(L'\\');
		if (sep != std::wstring::npos) {
			path = path.substr(0, sep + 1) + L"Whisper.dll";
			if (HMODULE h = LoadLibraryW(path.c_str()))
				return h;
		}
	}
	return LoadLibraryW(L"Whisper.dll");
}

// Callbacks must be plain __stdcall/__cdecl functions; captureless lambdas
// cannot be implicitly converted to __stdcall function pointers on x86.

static HRESULT __stdcall on_load_progress(double val, void *pv) noexcept {
	static_cast<agi::ProgressSink *>(pv)->SetProgress((int64_t)(val * 100), 100);
	return S_OK;
}

static HRESULT __stdcall on_load_cancel(void *pv) noexcept {
	return static_cast<agi::ProgressSink *>(pv)->IsCancelled() ? S_FALSE : S_OK;
}

static HRESULT __cdecl on_new_segment(Whisper::iContext *, uint32_t, void *pv) noexcept {
	// Return S_FALSE to request early stop when the user cancels.
	return static_cast<agi::ProgressSink *>(pv)->IsCancelled() ? S_FALSE : S_OK;
}

static HRESULT __stdcall on_stream_progress(double val, Whisper::iContext *, void *pv) noexcept {
	auto *ps = static_cast<agi::ProgressSink *>(pv);
	ps->SetProgress((int64_t)(val * 100), 100);
	return ps->IsCancelled() ? S_FALSE : S_OK;
}

struct Segment { int start_ms, end_ms; std::string text; };

// Convert a "Name (xx)" language preference string to the uint32_t key
// that sFullParams.language expects.  Returns UINT_MAX for "Auto detect".
static uint32_t parse_language(const std::string &s) {
	if (s.empty() || s == "Auto detect") return ~0u;
	// Extract the ISO 639-1 code from "Name (xx)" format.
	auto lp = s.rfind('(');
	auto rp = s.rfind(')');
	const char *code = (lp != std::string::npos && rp > lp + 1)
	                 ? s.c_str() + lp + 1
	                 : s.c_str();  // fallback: treat whole string as code
	uint32_t key = 0;
	for (int i = 0; code[i] && code[i] != ')' && i < 4; ++i)
		key |= (uint32_t)(uint8_t)code[i] << (i * 8);
	return key;
}

static Whisper::eModelImplementation parse_impl(const std::string &s) {
	if (s == "Hybrid")    return Whisper::eModelImplementation::Hybrid;
	if (s == "Reference") return Whisper::eModelImplementation::Reference;
	return Whisper::eModelImplementation::GPU;
}

// WhisperDesktop's COM objects carry thread affinity: every method call
// (including Release) must come from the same thread that created the object.
// WhisperWorker owns a single persistent background thread that is the
// exclusive owner of all Whisper COM resources.
class WhisperWorker {
	// COM resources — created, used, and destroyed exclusively on thr.
	// iAudioReader is NOT cached: openAudioFile seeks to start_ms on each call,
	// so creating it fresh per transcription is cheap and avoids holding a
	// large decoded PCM buffer in RAM between calls.
	HMODULE                          hdll        = nullptr;
	Whisper::pfn_initMediaFoundation pfnInitMF   = nullptr;
	Whisper::pfn_loadModel           pfnLoadModel = nullptr;
	Whisper::iMediaFoundation       *mf          = nullptr;
	Whisper::iModel                 *model       = nullptr;
	std::wstring                     model_path;
	Whisper::eModelImplementation    model_impl  = Whisper::eModelImplementation::GPU;

	// Dispatch queue (capacity = 1; callers must wait for completion).
	std::mutex              mtx;
	std::condition_variable cv_in;
	std::condition_variable cv_out;
	std::function<void()>   pending;
	bool has_work  = false;
	bool work_done = false;
	bool quit_flag = false;

	void loop() {
		while (true) {
			std::unique_lock<std::mutex> lk(mtx);
			cv_in.wait(lk, [this]{ return has_work || quit_flag; });
			if (quit_flag) { release_impl(); break; }
			has_work = false;
			auto fn  = std::move(pending);
			lk.unlock();
			fn();
			{
				std::unique_lock<std::mutex> l(mtx);
				work_done = true;
			}
			cv_out.notify_one();
		}
	}

	void release_impl() {
		if (model) { model->Release(); model = nullptr; }
		if (mf)    { mf->Release();    mf    = nullptr; }
		pfnInitMF    = nullptr;
		pfnLoadModel = nullptr;
		if (hdll)  { FreeLibrary(hdll); hdll = nullptr; }
		model_path.clear();
		model_impl = Whisper::eModelImplementation::GPU;
		loaded.store(false, std::memory_order_release);
	}

public:
	// Readable from any thread; written only by the worker thread.
	std::atomic<bool> loaded{false};
	std::thread thr;

	WhisperWorker() : thr([this]{ loop(); }) {}

	~WhisperWorker() {
		{
			std::unique_lock<std::mutex> lk(mtx);
			quit_flag = true;
		}
		cv_in.notify_one();
		thr.join();
		// release_impl() already called by loop() before it returned.
	}

	// Submit fn to the worker thread and block until it completes.
	// Must not be called from the worker thread itself.
	void dispatch(std::function<void()> fn) {
		std::unique_lock<std::mutex> lk(mtx);
		pending   = std::move(fn);
		has_work  = true;
		work_done = false;
		cv_in.notify_one();
		cv_out.wait(lk, [this]{ return work_done; });
	}

	// Called from inside a dispatch() lambda (on the worker thread).
	void transcribe(const std::wstring &model_wpath, const std::wstring &audio_wpath,
	                int start_ms, int duration_ms,
	                Whisper::eModelImplementation desired_impl, uint32_t language,
	                agi::ProgressSink *ps,
	                std::string &error_msg, std::vector<Segment> &segments)
	{
		if (!hdll) {
			hdll = LoadWhisperDll();
			if (!hdll) {
				error_msg = "Could not load Whisper.dll.\n"
				            "Please place Whisper.dll in the same folder as Aegisub.exe.";
				return;
			}
			pfnInitMF    = (Whisper::pfn_initMediaFoundation)GetProcAddress(hdll, "initMediaFoundation");
			pfnLoadModel = (Whisper::pfn_loadModel)           GetProcAddress(hdll, "loadModel");
			if (!pfnInitMF || !pfnLoadModel) {
				release_impl();
				error_msg = "Whisper.dll is incompatible or from the wrong build.";
				return;
			}
		}

		if (!mf) {
			HRESULT hr = pfnInitMF(&mf);
			if (FAILED(hr)) { error_msg = "Failed to initialize Windows Media Foundation."; return; }
		}

		if (ps->IsCancelled()) return;

		if (!model || model_path != model_wpath || model_impl != desired_impl) {
			if (model) { model->Release(); model = nullptr; }
			ps->SetMessage("Loading model...");
			ps->SetProgress(0, 100);

			Whisper::sModelSetup setup{};
			setup.impl = desired_impl;

			Whisper::sLoadModelCallbacks load_cbs{};
			load_cbs.pv       = ps;
			load_cbs.progress = on_load_progress;
			load_cbs.cancel   = on_load_cancel;

			HRESULT hr = pfnLoadModel(model_wpath.c_str(), setup, &load_cbs, &model);
			if (FAILED(hr)) {
				if (!ps->IsCancelled())
					error_msg = "Failed to load Whisper model.\n"
					            "Ensure the model .bin file is valid and the GPU supports DirectX 12.";
				return;
			}
			model_path = model_wpath;
			model_impl = desired_impl;
		}

		loaded.store(true, std::memory_order_release);

		if (ps->IsCancelled()) return;

		// Open a streaming reader seeking directly to start_ms — avoids
		// decoding the entire audio file into RAM (unlike loadAudioFile).
		ComPtr<Whisper::iAudioReader> reader;
		{
			HRESULT hr = mf->openAudioFile(audio_wpath.c_str(), /*stereo=*/false, &reader.p, start_ms);
			if (FAILED(hr)) { error_msg = "Failed to open audio file for streaming."; return; }
		}

		if (ps->IsCancelled()) return;

		ComPtr<Whisper::iContext> ctx;
		{
			HRESULT hr = model->createContext(&ctx.p);
			if (FAILED(hr)) { error_msg = "Failed to create inference context."; return; }
		}

		Whisper::sFullParams params{};
		{
			HRESULT hr = ctx->fullDefaultParams(Whisper::eSamplingStrategy::Greedy, &params);
			if (FAILED(hr)) { error_msg = "Failed to retrieve default inference parameters."; return; }
		}

		// offset_ms tells the model the absolute timestamp of the reader's
		// start position (for correct output timestamps); duration_ms caps
		// how much audio runStreamed will consume from the reader.
		params.offset_ms                      = start_ms;
		params.duration_ms                    = duration_ms;
		params.language                       = language;
		params.new_segment_callback           = on_new_segment;
		params.new_segment_callback_user_data = ps;

		ps->SetMessage("Transcribing...");
		ps->SetProgress(0, 100);

		// runStreamed feeds audio from the reader to the GPU in chunks,
		// overlapping decode and inference (pipeline parallelism).
		Whisper::sProgressSink progress_sink{ on_stream_progress, ps };
		{
			HRESULT hr = ctx->runStreamed(params, progress_sink, reader.p);
			if (FAILED(hr)) {
				if (!ps->IsCancelled())
					error_msg = "Transcription failed.";
				return;
			}
		}

		ComPtr<Whisper::iTranscribeResult> result;
		{
			HRESULT hr = ctx->getResults(
				Whisper::eResultFlags::Timestamps | Whisper::eResultFlags::NewObject,
				&result.p);
			if (FAILED(hr)) { error_msg = "Failed to retrieve transcription results."; return; }
		}

		Whisper::sTranscribeLength len{};
		result->getSize(len);
		const Whisper::sSegment *segs = result->getSegments();

		segments.reserve(len.countSegments);
		for (uint32_t i = 0; i < len.countSegments; ++i) {
			const auto &seg = segs[i];
			if (!seg.text || seg.text[0] == '\0') continue;

			std::string text(seg.text);
			// Trim leading/trailing whitespace that Whisper sometimes includes.
			auto s = text.find_first_not_of(" \t\r\n");
			if (s == std::string::npos) continue;
			auto e = text.find_last_not_of(" \t\r\n");
			text = text.substr(s, e - s + 1);
			if (text.empty()) continue;

			segments.push_back({
				(int)(seg.time.begin.ticks / 10000),
				(int)(seg.time.end.ticks   / 10000),
				std::move(text)
			});
		}
	}

	// Called from inside a dispatch() lambda (on the worker thread).
	void release(agi::ProgressSink *ps) {
		ps->SetMessage("Releasing Whisper resources...");
		ps->SetIndeterminate();
		release_impl();
	}
};

// The worker is created on first use and lives until program exit.
// Its destructor joins the thread and releases all COM objects correctly.
static WhisperWorker& get_worker() {
	static WhisperWorker w;
	return w;
}

struct subtitle_whisper_transcribe final : public Command {
	CMD_NAME("subtitle/whisper/transcribe")
	STR_MENU("Transcribe with &Whisper")
	STR_DISP("Transcribe with Whisper")
	STR_HELP("Transcribe audio within the selected lines' time range using Whisper AI and insert results")
	CMD_TYPE(COMMAND_VALIDATE)

	bool Validate(const agi::Context *c) override {
		return c->project->AudioProvider() != nullptr
		    && !c->selectionController->GetSelectedSet().empty();
	}

	void operator()(agi::Context *c) override {
		std::string model_path_str = OPT_GET("Path/Whisper/Model")->GetString();
		if (model_path_str.empty()) {
			wxMessageBox(
				_("Please set the Whisper model file path in:\nPreferences → Advanced → Whisper"),
				_("Whisper"), wxOK | wxICON_INFORMATION, c->parent);
			return;
		}

		agi::fs::path audio_path = c->project->AudioName();
		if (audio_path.empty()) {
			wxMessageBox(_("No audio file is loaded."), _("Whisper"), wxOK | wxICON_ERROR, c->parent);
			return;
		}

		AssDialogue *active_line = c->selectionController->GetActiveLine();
		if (!active_line) return;

		std::string style_name = active_line->Style;

		// Compute the time span covering all selected lines.
		int start_ms = active_line->Start;
		int end_ms   = active_line->End;
		for (AssDialogue *sel_line : c->selectionController->GetSelectedSet()) {
			if ((int)sel_line->Start < start_ms) start_ms = sel_line->Start;
			if ((int)sel_line->End   > end_ms)   end_ms   = sel_line->End;
		}
		int duration_ms = end_ms - start_ms;

		std::wstring model_wpath = agi::fs::path(model_path_str).wstring();
		std::wstring audio_wpath = audio_path.wstring();

		Whisper::eModelImplementation desired_impl =
			parse_impl(OPT_GET("Whisper/Implementation")->GetString());
		uint32_t language =
			parse_language(OPT_GET("Whisper/Language")->GetString());

		std::vector<Segment> segments;
		std::string          error_msg;

		auto& w = get_worker();

		// DialogProgress runs its lambda on a background thread (T_dialog).
		// T_dialog calls w.dispatch(), which submits work to the persistent
		// worker thread (T_worker) and blocks until T_worker is done.
		// All COM calls happen on T_worker — satisfying the thread-affinity rule.
		DialogProgress dlg(c->parent, _("Whisper"), _("Initializing..."));
		dlg.Run([&](agi::ProgressSink *ps) {
			ps->SetTitle("Whisper Speech Recognition");
			w.dispatch([&]{
				w.transcribe(model_wpath, audio_wpath, start_ms, duration_ms,
				             desired_impl, language, ps, error_msg, segments);
			});
		});

		if (!error_msg.empty()) {
			wxMessageBox(to_wx(error_msg), _("Whisper Error"), wxOK | wxICON_ERROR, c->parent);
			return;
		}

		if (segments.empty()) return;

		// Insert all transcribed segments as new dialogue lines after active_line,
		// preserving the active line's style.
		auto pos = c->ass->iterator_to(*active_line);
		++pos;

		AssDialogue *first_new = nullptr;
		for (auto &seg : segments) {
			auto *line  = new AssDialogue;
			line->Style = style_name;
			line->Start = seg.start_ms;
			line->End   = seg.end_ms;
			line->Text  = seg.text;
			if (!first_new) first_new = line;
			c->ass->Events.insert(pos, *line);
		}

		c->ass->Commit(_("Whisper transcription"), AssFile::COMMIT_DIAG_ADDREM);

		if (first_new)
			c->selectionController->SetSelectionAndActive({ first_new }, first_new);
	}
};

struct subtitle_whisper_release final : public Command {
	CMD_NAME("subtitle/whisper/release")
	STR_MENU("Release &Whisper Resources")
	STR_DISP("Release Whisper Resources")
	STR_HELP("Free the loaded Whisper model and audio buffer from memory")
	CMD_TYPE(COMMAND_VALIDATE)

	bool Validate(const agi::Context *) override {
		// Check the atomic without creating the worker if it hasn't been used yet.
		// get_worker() is safe here: the static is trivially initialised on first call.
		return get_worker().loaded.load(std::memory_order_acquire);
	}

	void operator()(agi::Context *c) override {
		auto& w = get_worker();
		// Run release on the worker thread so COM Release() calls stay on the
		// same thread that created the objects.
		DialogProgress dlg(c->parent, _("Whisper"), _("Releasing resources..."));
		dlg.Run([&](agi::ProgressSink *ps) {
			ps->SetTitle("Whisper");
			w.dispatch([&]{
				w.release(ps);
			});
		});
	}
};

} // anonymous namespace

#endif // _WIN32

namespace cmd {
	void init_whisper() {
#ifdef _WIN32
		reg(std::make_unique<subtitle_whisper_transcribe>());
		reg(std::make_unique<subtitle_whisper_release>());
#endif
	}
}
