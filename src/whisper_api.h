// Consolidated API header for WhisperDesktop's Whisper.dll
// Sourced from: WhisperDesktop/Whisper/API/
// Only the subset needed for transcription is declared here.
#pragma once
#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <objbase.h>
#include <stdint.h>

namespace Whisper {

// ---- Enums ----------------------------------------------------------------

enum struct eSamplingStrategy : int {
	Greedy     = 0,
	BeamSearch = 1,
};

enum struct eModelImplementation : uint32_t {
	GPU       = 1,
	Hybrid    = 2,
	Reference = 3,
};

enum struct eFullParamsFlags : uint32_t {
	Translate       = 1,
	NoContext       = 2,
	SingleSegment   = 4,
	PrintSpecial    = 8,
	PrintProgress   = 0x10,
	PrintRealtime   = 0x20,
	PrintTimestamps = 0x40,
	TokenTimestamps = 0x100,
	SpeedupAudio    = 0x200,
};
inline eFullParamsFlags operator|(eFullParamsFlags a, eFullParamsFlags b) {
	return (eFullParamsFlags)((uint32_t)a | (uint32_t)b);
}
inline void operator|=(eFullParamsFlags &a, eFullParamsFlags b) { a = a | b; }

enum struct eResultFlags : uint32_t {
	None      = 0,
	Tokens    = 1,
	Timestamps= 2,
	NewObject = 0x100,
};
inline eResultFlags operator|(eResultFlags a, eResultFlags b) {
	return (eResultFlags)((uint32_t)a | (uint32_t)b);
}

enum struct eSpeakerChannel : uint8_t {
	Unsure = 0, Left = 1, Right = 2, NoStereoData = 0xFF,
};

// ---- Time structs ---------------------------------------------------------

// Timestamps are stored in 100-nanosecond ticks (same as FILETIME / .NET TimeSpan).
// Convert to milliseconds: ticks / 10000
struct sTimeSpan    { uint64_t ticks; };
struct sTimeInterval { sTimeSpan begin, end; };

// ---- Result structs -------------------------------------------------------

struct sSegment {
	const char *text;          // UTF-8, null-terminated
	sTimeInterval time;        // absolute position in the audio file
	uint32_t firstToken, countTokens;
};

struct sToken {
	const char *text;
	sTimeInterval time;
	float probability;
	float probabilityTimestamp;
	float ptsum;
	float vlen;
	int   id;
	uint32_t flags;
};

struct sTranscribeLength {
	uint32_t countSegments, countTokens;
};

// ---- Parameter structs ----------------------------------------------------

using whisper_token = int;

// Forward declarations needed for callback signatures
struct iContext;
struct iAudioReader;
struct iAudioCapture;
struct iModel;

using pfnNewSegment   = HRESULT(__cdecl *)(iContext *ctx, uint32_t n_new, void *user_data) noexcept;
using pfnEncoderBegin = HRESULT(__cdecl *)(iContext *ctx, void *user_data) noexcept;
using pfnReportProgress = HRESULT(__stdcall *)(double val, iContext *ctx, void *pv) noexcept;
using pfnListAdapters   = void(__stdcall *)(const wchar_t *name, void *pv);
using pfnDecodedTokens  = void(__stdcall *)(const int *tokens, int tokensLength, void *pv);

struct sProgressSink {
	pfnReportProgress pfn;
	void *pv;
};

struct sFullParams {
	eSamplingStrategy strategy;
	int cpuThreads;
	int n_max_text_ctx;
	int offset_ms;           // start offset in ms
	int duration_ms;         // audio duration to process in ms (0 = to end)
	eFullParamsFlags flags;
	uint32_t language;
	float thold_pt;
	float thold_ptsum;
	int   max_len;
	int   max_tokens;
	struct { int n_past; } greedy;
	struct { int n_past; int beam_width; int n_best; } beam_search;
	int   audio_ctx;
	const whisper_token *prompt_tokens;
	int   prompt_n_tokens;
	pfnNewSegment   new_segment_callback;
	void           *new_segment_callback_user_data;
	pfnEncoderBegin encoder_begin_callback;
	void           *encoder_begin_callback_user_data;
};

struct sModelSetup {
	eModelImplementation impl  = eModelImplementation::GPU;
	uint32_t             flags = 0;
	const wchar_t       *adapter = nullptr;
};

using pfnLoadProgress = HRESULT(__stdcall *)(double val, void *pv) noexcept;
using pfnCancel       = HRESULT(__stdcall *)(void *pv) noexcept;

struct sLoadModelCallbacks {
	pfnLoadProgress progress;
	pfnCancel       cancel;
	void           *pv;
};

struct sLoggerSetup {
	void    *sink    = nullptr;
	void    *context = nullptr;
	uint8_t  level   = 0;
	uint8_t  flags   = 0;
};

// MF capture structs (needed for iMediaFoundation vtable completeness)
struct sCaptureDevice {
	const wchar_t *displayName;
	const wchar_t *endpoint;
};
using pfnFoundCaptureDevices = HRESULT(__stdcall *)(int len, const sCaptureDevice *buffer, void *pv);

struct sCaptureParams {
	float    minDuration       = 2.0f;
	float    maxDuration       = 3.0f;
	float    dropStartSilence  = 0.25f;
	float    pauseDuration     = 0.333f;
	uint32_t flags             = 0;
};

using pfnShouldCancel  = HRESULT(__stdcall *)(void *pv) noexcept;
using pfnCaptureStatus = HRESULT(__stdcall *)(void *pv, uint8_t status) noexcept;

struct sCaptureCallbacks {
	pfnShouldCancel  shouldCancel;
	pfnCaptureStatus captureStatus;
	void            *pv;
};

// ---- COM Interfaces -------------------------------------------------------
// Method order MUST match the DLL's vtable exactly.
// IUnknown methods (QueryInterface/AddRef/Release) are inherited first,
// followed by interface-specific methods in declaration order.

struct iTranscribeResult : IUnknown {
	virtual HRESULT       __stdcall getSize(sTranscribeLength &rdi) const = 0;
	virtual const sSegment* __stdcall getSegments() const = 0;
	virtual const sToken*   __stdcall getTokens() const = 0;
};

struct iAudioBuffer : IUnknown {
	virtual uint32_t     __stdcall countSamples() const = 0;
	virtual const float* __stdcall getPcmMono() const = 0;
	virtual const float* __stdcall getPcmStereo() const = 0;
	virtual HRESULT      __stdcall getTime(int64_t &rdi) const = 0;
};

struct iContext : IUnknown {
	virtual HRESULT __stdcall runFull(const sFullParams &params, const iAudioBuffer *buffer) = 0;
	virtual HRESULT __stdcall runStreamed(const sFullParams &params, const sProgressSink &progress, const iAudioReader *reader) = 0;
	virtual HRESULT __stdcall runCapture(const sFullParams &params, const sCaptureCallbacks &callbacks, const iAudioCapture *reader) = 0;
	virtual HRESULT __stdcall getResults(eResultFlags flags, iTranscribeResult **pp) const = 0;
	virtual HRESULT __stdcall detectSpeaker(const sTimeInterval &time, eSpeakerChannel &result) const = 0;
	virtual HRESULT __stdcall getModel(iModel **pp) = 0;
	virtual HRESULT __stdcall fullDefaultParams(eSamplingStrategy strategy, sFullParams *rdi) = 0;
	virtual HRESULT __stdcall timingsPrint() = 0;
	virtual HRESULT __stdcall timingsReset() = 0;
};

struct iModel : IUnknown {
	virtual HRESULT      __stdcall createContext(iContext **pp) = 0;
	virtual HRESULT      __stdcall tokenize(const char *text, pfnDecodedTokens pfn, void *pv) = 0;
	virtual HRESULT      __stdcall isMultilingual() = 0;
	virtual HRESULT      __stdcall getSpecialTokens(void *rdi) = 0;  // SpecialTokens&
	virtual const char*  __stdcall stringFromToken(whisper_token token) = 0;
	virtual HRESULT      __stdcall clone(iModel **rdi) = 0;
};

struct iMediaFoundation : IUnknown {
	virtual HRESULT __stdcall loadAudioFile(LPCWSTR path, bool stereo, iAudioBuffer **pp) const = 0;
	virtual HRESULT __stdcall openAudioFile(LPCWSTR path, bool stereo, iAudioReader **pp, int start_time_ms = 0) = 0;
	virtual HRESULT __stdcall loadAudioFileData(const void *data, uint64_t size, bool stereo, iAudioReader **pp) = 0;
	virtual HRESULT __stdcall listCaptureDevices(pfnFoundCaptureDevices pfn, void *pv) = 0;
	virtual HRESULT __stdcall openCaptureDevice(LPCWSTR endpoint, const sCaptureParams &captureParams, iAudioCapture **pp) = 0;
};

// ---- Exported function pointer types -------------------------------------

using pfn_initMediaFoundation = HRESULT(__stdcall *)(iMediaFoundation **pp);
using pfn_loadModel           = HRESULT(__stdcall *)(const wchar_t *path, const sModelSetup &setup, const sLoadModelCallbacks *callbacks, iModel **pp);

} // namespace Whisper

#endif // _WIN32
