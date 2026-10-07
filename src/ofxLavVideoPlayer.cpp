// ofxLavVideoPlayer
//
// Rewrite of ofDirectShowPlayer.cpp (openFrameworks 0.11.2) that builds the
// DirectShow graph with LAV Filters loaded straight from their .ax files,
// without COM registration or a codec pack installer.
//
// Original: DirectShowVideo and ofDirectShowPlayer written by Theodore Watson, Jan 2014
// openFrameworks is released under the MIT License.
//
// Changes from the original:
//  - Everything lives in namespace ofxLav, so nothing collides with the core
//    ofDirectShowPlayer.cpp (global helpers / the DirectShowVideo class).
//  - LAV Splitter Source / LAV Video / LAV Audio are created via LoadLibrary +
//    DllGetClassObject and inserted into the graph before it is connected.
//  - Optional fallback to the system filters (the original RenderFile path).
//  - Paths are converted with MultiByteToWideChar (UTF-8, then ANSI code page),
//    so file names containing Japanese characters can be opened.
//  - The default renderer is found through the grabber's connected pin instead
//    of by the name "Video Renderer", and a few COM leaks were fixed.

#include "ofxLavVideoPlayer.h"

#ifdef TARGET_WIN32

#include "ofPixels.h"
#include "ofMath.h"
#include "ofUtils.h"
#include "ofFileUtils.h"
#include "ofLog.h"

//-------------------------------------------------------------------------------------------------------------------------------------------------------------
// DirectShow includes
//-------------------------------------------------------------------------------------------------------------------------------------------------------------

#include <dshow.h>
#pragma include_alias( "dxtrans.h", "qedit.h" )
#define __IDxtCompositor_INTERFACE_DEFINED__
#define __IDxtAlphaSetter_INTERFACE_DEFINED__
#define __IDxtJpeg_INTERFACE_DEFINED__
#define __IDxtKey_INTERFACE_DEFINED__
#include <aviriff.h>
#include <windows.h>

//for threading
#include <process.h>

#include <functional>
#include <mutex>
#include <unordered_map>

#pragma comment(lib, "strmiids.lib")

// These come from strmiids.lib and must keep C linkage, so they stay global.
EXTERN_C const CLSID CLSID_SampleGrabber;
EXTERN_C const IID IID_ISampleGrabber;
EXTERN_C const CLSID CLSID_NullRenderer;

namespace ofxLav {

	//-------------------------------------------------------------------------------------------------------------------------------------------------------------
	// Sample Grabber interfaces
	// Due to a missing qedit.h in recent Platform SDKs, the relevant contents are replicated here.
	//-------------------------------------------------------------------------------------------------------------------------------------------------------------

	MIDL_INTERFACE("0579154A-2B53-4994-B0D0-E773148EFF85")
		ISampleGrabberCB : public IUnknown
	{
	public:
		virtual HRESULT STDMETHODCALLTYPE SampleCB(
			double SampleTime,
			IMediaSample * pSample) = 0;

		virtual HRESULT STDMETHODCALLTYPE BufferCB(
			double SampleTime,
			BYTE* pBuffer,
			long BufferLen) = 0;
	};

	MIDL_INTERFACE("6B652FFF-11FE-4fce-92AD-0266B5D7C78F")
		ISampleGrabber : public IUnknown
	{
	public:
		virtual HRESULT STDMETHODCALLTYPE SetOneShot(
			BOOL OneShot) = 0;

		virtual HRESULT STDMETHODCALLTYPE SetMediaType(
			const AM_MEDIA_TYPE* pType) = 0;

		virtual HRESULT STDMETHODCALLTYPE GetConnectedMediaType(
			AM_MEDIA_TYPE* pType) = 0;

		virtual HRESULT STDMETHODCALLTYPE SetBufferSamples(
			BOOL BufferThem) = 0;

		virtual HRESULT STDMETHODCALLTYPE GetCurrentBuffer(
			/* [out][in] */ long* pBufferSize,
			/* [out] */ long* pBuffer) = 0;

		virtual HRESULT STDMETHODCALLTYPE GetCurrentSample(
			/* [retval][out] */ IMediaSample** ppSample) = 0;

		virtual HRESULT STDMETHODCALLTYPE SetCallback(
			ISampleGrabberCB* pCallback,
			long WhichMethodToCallback) = 0;
	};

	//-------------------------------------------------------------------------------------------------------------------------------------------------------------
	// LAV Filters CLSIDs (taken from the LAV Filters sources; stable across releases)
	//-------------------------------------------------------------------------------------------------------------------------------------------------------------

	// {B98D13E7-55DB-4385-A33D-09FD1BA26338}
	static const GUID CLSID_LAVSplitterSource =
	{ 0xB98D13E7, 0x55DB, 0x4385, { 0xA3, 0x3D, 0x09, 0xFD, 0x1B, 0xA2, 0x63, 0x38 } };
	// {EE30215D-164F-4A92-A4EB-9D4C13390F9F}
	static const GUID CLSID_LAVVideoDecoder =
	{ 0xEE30215D, 0x164F, 0x4A92, { 0xA4, 0xEB, 0x9D, 0x4C, 0x13, 0x39, 0x0F, 0x9F } };
	// {E8E73B6B-4CB3-44A4-BE99-4F7BCB96E491}
	static const GUID CLSID_LAVAudioDecoder =
	{ 0xE8E73B6B, 0x4CB3, 0x44A4, { 0xBE, 0x99, 0x4F, 0x7B, 0xCB, 0x96, 0xE4, 0x91 } };

	static const wchar_t* LAV_SPLITTER_FILE = L"LAVSplitter.ax";
	static const wchar_t* LAV_VIDEO_FILE = L"LAVVideo.ax";
	static const wchar_t* LAV_AUDIO_FILE = L"LAVAudio.ax";

	//-------------------------------------------------------------------------------------------------------------------------------------------------------------
	// String / path helpers
	//-------------------------------------------------------------------------------------------------------------------------------------------------------------

	// Converts a narrow string to UTF-16. Tries UTF-8 first, then the ANSI code page
	// (e.g. CP932 on Japanese Windows), which is what ofToDataPath returns in 0.11.2.
	std::wstring toWide(const std::string& str) {
		if (str.empty()) {
			return std::wstring();
		}
		const UINT codePages[2] = { CP_UTF8, CP_ACP };
		for (UINT codePage : codePages) {
			DWORD flags = (codePage == CP_UTF8) ? MB_ERR_INVALID_CHARS : 0;
			int len = MultiByteToWideChar(codePage, flags, str.data(), (int)str.size(), nullptr, 0);
			if (len > 0) {
				std::wstring wide(len, L'\0');
				MultiByteToWideChar(codePage, flags, str.data(), (int)str.size(), &wide[0], len);
				return wide;
			}
		}
		return std::wstring(str.begin(), str.end());
	}

	// Converts UTF-16 to UTF-8 (used for log messages).
	std::string toUtf8(const std::wstring& wide) {
		if (wide.empty()) {
			return std::string();
		}
		int len = WideCharToMultiByte(CP_UTF8, 0, wide.data(), (int)wide.size(), nullptr, 0, nullptr, nullptr);
		std::string str(len, '\0');
		WideCharToMultiByte(CP_UTF8, 0, wide.data(), (int)wide.size(), &str[0], len, nullptr, nullptr);
		return str;
	}

	bool fileExists(const std::wstring& path) {
		DWORD attributes = GetFileAttributesW(path.c_str());
		return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
	}

	void ensureTrailingSeparator(std::wstring& dir) {
		if (!dir.empty() && dir.back() != L'\\' && dir.back() != L'/') {
			dir += L'\\';
		}
	}

	std::wstring getExeDirectoryW() {
		std::wstring buffer(MAX_PATH, L'\0');
		DWORD len = 0;
		while (true) {
			len = GetModuleFileNameW(nullptr, &buffer[0], (DWORD)buffer.size());
			if (len < buffer.size()) {
				break;
			}
			buffer.resize(buffer.size() * 2);
		}
		buffer.resize(len);
		size_t pos = buffer.find_last_of(L"\\/");
		if (pos != std::wstring::npos) {
			buffer.resize(pos + 1);
		}
		return buffer;
	}

	//-------------------------------------------------------------------------------------------------------------------------------------------------------------
	// Global settings and COM lifetime
	//-------------------------------------------------------------------------------------------------------------------------------------------------------------

	namespace {
		std::string codecDirectorySetting;
		bool fallbackToSystemFilters = true;

		int comRefCount = 0;
		bool comInitialized = false;

		void retainCom() {
			if (comRefCount == 0) {
				// S_FALSE (already initialized) also has to be balanced by CoUninitialize.
				// RPC_E_CHANGED_MODE means another apartment model is active; don't uninitialize then.
				HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
				comInitialized = SUCCEEDED(hr);
			}
			comRefCount++;
		}

		void releaseCom() {
			comRefCount--;
			if (comRefCount == 0 && comInitialized) {
				CoUninitialize();
				comInitialized = false;
			}
		}

		void releaseSample(IMediaSample* sample) {
			sample->Release();
		}
	}

	// Resolves the folder that holds the LAV .ax files (always ends with a separator).
	std::wstring getCodecDirectoryW() {
		std::wstring dir;
		if (!codecDirectorySetting.empty()) {
			std::string path = ofFilePath::isAbsolute(codecDirectorySetting)
				? codecDirectorySetting
				: ofToDataPath(codecDirectorySetting, true);
			dir = toWide(path);
		}
		else {
			// Default: bin/data/codecs/
			dir = toWide(ofToDataPath("codecs", true));
		}
		ensureTrailingSeparator(dir);
		return dir;
	}

	//-------------------------------------------------------------------------------------------------------------------------------------------------------------
	// Loading filters from .ax files without COM registration
	//-------------------------------------------------------------------------------------------------------------------------------------------------------------

	// Loads an .ax module once and keeps it loaded for the lifetime of the process.
	// Filters created from it must never outlive the module, so it is never freed.
	HMODULE loadCodecModule(const std::wstring& axPath) {
		static std::mutex mutex;
		static std::unordered_map<std::wstring, HMODULE> modules;

		std::lock_guard<std::mutex> lock(mutex);
		auto it = modules.find(axPath);
		if (it != modules.end()) {
			return it->second;
		}

		if (!fileExists(axPath)) {
			ofLogWarning("ofxLavVideoPlayer") << "codec not found: " << toUtf8(axPath);
			return nullptr;
		}

		// LOAD_WITH_ALTERED_SEARCH_PATH lets Windows resolve the FFmpeg DLLs
		// (avcodec-lav-*.dll etc.) that sit next to the .ax file. Requires an absolute path.
		HMODULE module = LoadLibraryExW(axPath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
		if (!module) {
			DWORD error = GetLastError();
			std::string hint;
			switch (error) {
			case ERROR_SXS_CANT_GEN_ACTCTX: // 14001
				// Each LAV .ax embeds a manifest that depends on the private assembly
				// "LAVFilters.Dependencies", defined by LAVFilters.Dependencies.manifest
				// next to the .ax, which in turn lists the FFmpeg DLLs by exact name.
				hint = "LAVFilters.Dependencies.manifest is missing next to the .ax, or a DLL it lists"
					" is missing / from a different LAV version. Copy the whole portable zip contents"
					" into one folder. Details: Event Viewer > Windows Logs > Application, source 'SideBySide'.";
				break;
			case ERROR_BAD_EXE_FORMAT: // 193
				hint = "architecture mismatch: use the x64 LAV build for an x64 app, x86 for Win32.";
				break;
			case ERROR_MOD_NOT_FOUND: // 126
				hint = "a dependent DLL could not be found. Copy all DLLs from the LAV portable zip next to the .ax.";
				break;
			default:
				hint = "check that all files of the LAV portable zip are next to the .ax.";
				break;
			}
			ofLogError("ofxLavVideoPlayer") << "LoadLibrary failed for " << toUtf8(axPath)
				<< " (error " << error << "): " << hint;
			return nullptr;
		}
		modules[axPath] = module;
		return module;
	}

	// Creates a DirectShow filter directly from an .ax file via its DllGetClassObject.
	HRESULT createFilterFromFile(const std::wstring& axPath, REFCLSID clsid, IBaseFilter** outFilter) {
		using DllGetClassObjectFn = HRESULT(STDAPICALLTYPE*)(REFCLSID, REFIID, LPVOID*);

		*outFilter = nullptr;
		HMODULE module = loadCodecModule(axPath);
		if (!module) {
			return HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND);
		}

		auto getClassObject = reinterpret_cast<DllGetClassObjectFn>(GetProcAddress(module, "DllGetClassObject"));
		if (!getClassObject) {
			ofLogError("ofxLavVideoPlayer") << "DllGetClassObject not exported by " << toUtf8(axPath);
			return E_FAIL;
		}

		IClassFactory* factory = nullptr;
		HRESULT hr = getClassObject(clsid, IID_IClassFactory, reinterpret_cast<void**>(&factory));
		if (FAILED(hr)) {
			ofLogError("ofxLavVideoPlayer") << "DllGetClassObject failed for " << toUtf8(axPath) << " hr=0x" << ofToHex((int)hr);
			return hr;
		}

		hr = factory->CreateInstance(nullptr, IID_IBaseFilter, reinterpret_cast<void**>(outFilter));
		factory->Release();
		return hr;
	}

	//-------------------------------------------------------------------------------------------------------------------------------------------------------------
	// DirectShow helper methods (same as the original, but namespaced)
	//-------------------------------------------------------------------------------------------------------------------------------------------------------------

	// GetUnconnectedPin
	//    Finds an unconnected pin on a filter in the desired direction
	HRESULT GetUnconnectedPin(
		IBaseFilter* pFilter,   // Pointer to the filter.
		PIN_DIRECTION PinDir,   // Direction of the pin to find.
		IPin** ppPin)           // Receives a pointer to the pin.
	{
		*ppPin = 0;
		IEnumPins* pEnum = 0;
		IPin* pPin = 0;
		HRESULT hr = pFilter->EnumPins(&pEnum);
		if (FAILED(hr))
		{
			return hr;
		}
		while (pEnum->Next(1, &pPin, NULL) == S_OK)
		{
			PIN_DIRECTION ThisPinDir;
			pPin->QueryDirection(&ThisPinDir);
			if (ThisPinDir == PinDir)
			{
				IPin* pTmp = 0;
				hr = pPin->ConnectedTo(&pTmp);
				if (SUCCEEDED(hr))  // Already connected, not the pin we want.
				{
					pTmp->Release();
				}
				else  // Unconnected, this is the pin we want.
				{
					pEnum->Release();
					*ppPin = pPin;
					return S_OK;
				}
			}
			pPin->Release();
		}
		pEnum->Release();
		// Did not find a matching pin.
		return E_FAIL;
	}

	// Disconnect any connections to the filter.
	HRESULT DisconnectPins(IBaseFilter* pFilter)
	{
		IEnumPins* pEnum = 0;
		IPin* pPin = 0;
		HRESULT hr = pFilter->EnumPins(&pEnum);
		if (FAILED(hr))
		{
			return hr;
		}

		while (pEnum->Next(1, &pPin, NULL) == S_OK)
		{
			pPin->Disconnect();
			pPin->Release();
		}
		pEnum->Release();

		return S_OK;
	}

	// ConnectFilters
	//    Connects a pin of an upstream filter to the pDest downstream filter
	HRESULT ConnectFilters(
		IGraphBuilder* pGraph, // Filter Graph Manager.
		IPin* pOut,            // Output pin on the upstream filter.
		IBaseFilter* pDest)    // Downstream filter.
	{
		if ((pGraph == NULL) || (pOut == NULL) || (pDest == NULL))
		{
			return E_POINTER;
		}

		// Find an input pin on the downstream filter.
		IPin* pIn = 0;
		HRESULT hr = GetUnconnectedPin(pDest, PINDIR_INPUT, &pIn);
		if (FAILED(hr))
		{
			return hr;
		}
		// Try to connect them.
		hr = pGraph->Connect(pOut, pIn);
		pIn->Release();
		return hr;
	}

	// ConnectFilters
	//    Connects two filters
	HRESULT ConnectFilters(
		IGraphBuilder* pGraph,
		IBaseFilter* pSrc,
		IBaseFilter* pDest)
	{
		if ((pGraph == NULL) || (pSrc == NULL) || (pDest == NULL))
		{
			return E_POINTER;
		}

		// Find an output pin on the first filter.
		IPin* pOut = 0;
		HRESULT hr = GetUnconnectedPin(pSrc, PINDIR_OUTPUT, &pOut);
		if (FAILED(hr))
		{
			return hr;
		}
		hr = ConnectFilters(pGraph, pOut, pDest);
		pOut->Release();
		return hr;
	}

	// LocalFreeMediaType
	//    Free the format buffer in the media type
	void LocalFreeMediaType(AM_MEDIA_TYPE& mt)
	{
		if (mt.cbFormat != 0)
		{
			CoTaskMemFree((PVOID)mt.pbFormat);
			mt.cbFormat = 0;
			mt.pbFormat = NULL;
		}
		if (mt.pUnk != NULL)
		{
			// Unecessary because pUnk should not be used, but safest.
			mt.pUnk->Release();
			mt.pUnk = NULL;
		}
	}

	// LocalDeleteMediaType
	//    Free the format buffer in the media type,
	//    then delete the MediaType ptr itself
	void LocalDeleteMediaType(AM_MEDIA_TYPE* pmt)
	{
		if (pmt != NULL)
		{
			LocalFreeMediaType(*pmt);
			CoTaskMemFree(pmt);
		}
	}

	// Saves the graph as a .grf file that can be opened in GraphStudioNext / GraphEdit (debugging).
	HRESULT SaveGraphFile(IGraphBuilder* pGraph, const WCHAR* wszPath)
	{
		const WCHAR wszStreamName[] = L"ActiveMovieGraph";
		HRESULT hr;

		IStorage* pStorage = NULL;
		hr = StgCreateDocfile(
			wszPath,
			STGM_CREATE | STGM_TRANSACTED | STGM_READWRITE | STGM_SHARE_EXCLUSIVE,
			0, &pStorage);
		if (FAILED(hr))
		{
			return hr;
		}

		IStream* pStream;
		hr = pStorage->CreateStream(
			wszStreamName,
			STGM_WRITE | STGM_CREATE | STGM_SHARE_EXCLUSIVE,
			0, 0, &pStream);
		if (FAILED(hr))
		{
			pStorage->Release();
			return hr;
		}

		IPersistStream* pPersist = NULL;
		pGraph->QueryInterface(IID_IPersistStream, (void**)&pPersist);
		hr = pPersist->Save(pStream, TRUE);
		pStream->Release();
		pPersist->Release();
		if (SUCCEEDED(hr))
		{
			hr = pStorage->Commit(STGC_DEFAULT);
		}
		pStorage->Release();
		return hr;
	}

	//-------------------------------------------------------------------------------------------------------------------------------------------------------------
	// Graph building with LAV Filters
	//-------------------------------------------------------------------------------------------------------------------------------------------------------------

	// Adds a LAV filter to the graph. Returns true if it was added.
	bool addLavFilter(IGraphBuilder* graph, const std::wstring& axPath, REFCLSID clsid, const wchar_t* name) {
		IBaseFilter* filter = nullptr;
		if (FAILED(createFilterFromFile(axPath, clsid, &filter))) {
			return false;
		}
		HRESULT hr = graph->AddFilter(filter, name);
		filter->Release(); // the graph keeps its own reference
		return SUCCEEDED(hr);
	}

	// Renders every output pin of the source (video, audio, ...) like RenderFile would.
	// Returns S_OK if at least one pin could be rendered.
	HRESULT renderAllOutputPins(IGraphBuilder* graph, IBaseFilter* source) {
		IEnumPins* pins = nullptr;
		HRESULT hr = source->EnumPins(&pins);
		if (FAILED(hr)) {
			return hr;
		}

		bool anyRendered = false;
		IPin* pin = nullptr;
		while (pins->Next(1, &pin, nullptr) == S_OK) {
			PIN_DIRECTION dir;
			pin->QueryDirection(&dir);
			if (dir == PINDIR_OUTPUT) {
				// Subtitle or data pins may fail to render; that's fine as long as something did.
				if (SUCCEEDED(graph->Render(pin))) {
					anyRendered = true;
				}
			}
			pin->Release();
		}
		pins->Release();
		return anyRendered ? S_OK : VFW_E_CANNOT_RENDER;
	}

	// Builds the graph for filePath.
	//  useLav == true : LAV decoders are pre-inserted, and LAV Splitter Source opens the file
	//                   (if LAVSplitter.ax is missing, the system source filter is used with the LAV decoders).
	//  useLav == false: plain RenderFile with the filters installed on the system (original behaviour).
	// outSource receives the LAV source filter when it was used (released by the caller).
	HRESULT buildGraph(IGraphBuilder* graph, const std::wstring& filePath, const std::wstring& codecDir,
		bool useLav, IBaseFilter** outSource, bool& outUsedLavSplitter) {
		*outSource = nullptr;
		outUsedLavSplitter = false;

		if (!useLav) {
			return graph->RenderFile(filePath.c_str(), NULL);
		}

		// Filters already in the graph are tried first by Intelligent Connect,
		// so the LAV decoders win over whatever is registered on the system.
		addLavFilter(graph, codecDir + LAV_VIDEO_FILE, CLSID_LAVVideoDecoder, L"LAV Video Decoder");
		addLavFilter(graph, codecDir + LAV_AUDIO_FILE, CLSID_LAVAudioDecoder, L"LAV Audio Decoder");

		IBaseFilter* source = nullptr;
		HRESULT hr = createFilterFromFile(codecDir + LAV_SPLITTER_FILE, CLSID_LAVSplitterSource, &source);
		if (FAILED(hr)) {
			// No splitter: let the system pick the source filter, but still decode with LAV.
			return graph->RenderFile(filePath.c_str(), NULL);
		}

		hr = graph->AddFilter(source, L"LAV Splitter Source");
		if (FAILED(hr)) {
			source->Release();
			return hr;
		}

		IFileSourceFilter* fileSource = nullptr;
		hr = source->QueryInterface(IID_IFileSourceFilter, reinterpret_cast<void**>(&fileSource));
		if (SUCCEEDED(hr)) {
			hr = fileSource->Load(filePath.c_str(), nullptr);
			fileSource->Release();
		}
		if (FAILED(hr)) {
			ofLogWarning("ofxLavVideoPlayer") << "LAV Splitter Source could not open " << toUtf8(filePath)
				<< " hr=0x" << ofToHex((int)hr);
			graph->RemoveFilter(source);
			source->Release();
			return hr;
		}

		hr = renderAllOutputPins(graph, source);
		if (FAILED(hr)) {
			graph->RemoveFilter(source);
			source->Release();
			return hr;
		}

		*outSource = source;
		outUsedLavSplitter = true;
		return S_OK;
	}

	//-------------------------------------------------------------------------------------------------------------------------------------------------------------
	// DirectShowVideo - contains a simple directshow video player implementation
	//-------------------------------------------------------------------------------------------------------------------------------------------------------------

	class DirectShowVideo : public ISampleGrabberCB {
	public:

		DirectShowVideo() {
			retainCom();
			clearValues();
			InitializeCriticalSection(&critSection);
		}

		~DirectShowVideo() {
			tearDown();
			middleSample.reset();
			backSample.reset();
			releaseCom();
			DeleteCriticalSection(&critSection);
		}

		void tearDown() {
			if (m_pControl) {
				m_pControl->Release();
			}
			if (m_pEvent) {
				m_pEvent->Release();
			}
			if (m_pSeek) {
				m_pSeek->Release();
			}
			if (m_pAudio) {
				m_pAudio->Release();
			}
			if (m_pBasicVideo) {
				m_pBasicVideo->Release();
			}
			if (m_pGrabber) {
				m_pGrabber->Release();
			}
			if (m_pGrabberF) {
				m_pGrabberF->Release();
			}
			if (m_pGraph) {
				m_pGraph->Release();
			}
			if (m_pNullRenderer) {
				m_pNullRenderer->Release();
			}
			if (m_pSourceFile) {
				m_pSourceFile->Release();
			}
			if (m_pPosition) {
				m_pPosition->Release();
			}
			clearValues();
		}

		void clearValues() {
			hr = 0;

			m_pGraph = NULL;
			m_pControl = NULL;
			m_pEvent = NULL;
			m_pSeek = NULL;
			m_pAudio = NULL;
			m_pGrabber = NULL;
			m_pGrabberF = NULL;
			m_pBasicVideo = NULL;
			m_pNullRenderer = NULL;
			m_pSourceFile = NULL;
			m_pPosition = NULL;

			timeNow = 0;
			lPositionInSecs = 0;
			lDurationInNanoSecs = 0;
			lTotalDuration = 0;
			rtNew = 0;
			lPosition = 0;
			lvolume = -1000;
			evCode = 0;
			width = height = 0;
			bVideoOpened = false;
			bLoop = true;
			bPaused = false;
			bPlaying = false;
			bEndReached = false;
			bNewPixels = false;
			bFrameNew = false;
			bUsingLav = false;
			curMovieFrame = -1;
			frameCount = -1;

			movieRate = 1.0;
			averageTimePerFrame = 1.0 / 30.0;
		}

		//------------------------------------------------
		// This object is owned by a shared_ptr, so COM reference counting is a no-op.
		STDMETHODIMP_(ULONG) AddRef() { return 1; }
		STDMETHODIMP_(ULONG) Release() { return 2; }

		//------------------------------------------------
		STDMETHODIMP QueryInterface(REFIID riid, void** ppvObject) {
			*ppvObject = static_cast<ISampleGrabberCB*>(this);
			return S_OK;
		}

		//------------------------------------------------
		// Called from the DirectShow streaming thread for every decoded frame.
		STDMETHODIMP SampleCB(double Time, IMediaSample* pSample) {

			BYTE* ptrBuffer = NULL;
			HRESULT hr = pSample->GetPointer(&ptrBuffer);

			if (hr == S_OK) {
				long latestBufferLength = pSample->GetActualDataLength();
				if (latestBufferLength == (long)pixels.getTotalBytes()) {
					EnterCriticalSection(&critSection);
					pSample->AddRef();
					backSample = std::unique_ptr<IMediaSample, std::function<void(IMediaSample*)>>(pSample, releaseSample);
					bNewPixels = true;

					//this is just so we know if there is a new frame
					frameCount++;

					LeaveCriticalSection(&critSection);
				}
				else {
					ofLogError("ofxLavVideoPlayer") << "SampleCB() - buffer sizes do not match "
						<< latestBufferLength << " " << pixels.getTotalBytes();
				}
			}

			return S_OK;
		}

		//This method is meant to have more overhead
		STDMETHODIMP BufferCB(double Time, BYTE* pBuffer, long BufferLen) {
			return E_NOTIMPL;
		}

		bool loadMovie(const std::wstring& filePathW, ofPixelFormat format, bool useLav, const std::wstring& codecDir) {
			tearDown();
			this->pixelFormat = format;

			// Create the Filter Graph Manager and query for interfaces.
			hr = CoCreateInstance(CLSID_FilterGraph, NULL, CLSCTX_INPROC_SERVER, IID_IGraphBuilder, (void**)&m_pGraph);
			if (FAILED(hr)) {
				tearDown();
				return false;
			}

			hr = m_pGraph->QueryInterface(IID_IMediaSeeking, (void**)&m_pSeek);
			if (FAILED(hr)) {
				tearDown();
				return false;
			}

			hr = m_pGraph->QueryInterface(IID_IMediaPosition, (LPVOID*)&m_pPosition);
			if (FAILED(hr)) {
				tearDown();
				return false;
			}

			hr = m_pGraph->QueryInterface(IID_IBasicAudio, (void**)&m_pAudio);
			if (FAILED(hr)) {
				tearDown();
				return false;
			}

			// Use IGraphBuilder::QueryInterface (inherited from IUnknown) to get the IMediaControl interface.
			hr = m_pGraph->QueryInterface(IID_IMediaControl, (void**)&m_pControl);
			if (FAILED(hr)) {
				tearDown();
				return false;
			}

			// And get the Media Event interface, too.
			hr = m_pGraph->QueryInterface(IID_IMediaEvent, (void**)&m_pEvent);
			if (FAILED(hr)) {
				tearDown();
				return false;
			}

			//SAMPLE GRABBER (ALLOWS US TO GRAB THE BUFFER)//
			// Create the Sample Grabber.
			hr = CoCreateInstance(CLSID_SampleGrabber, NULL, CLSCTX_INPROC_SERVER, IID_IBaseFilter, (void**)&m_pGrabberF);
			if (FAILED(hr)) {
				tearDown();
				return false;
			}

			hr = m_pGraph->AddFilter(m_pGrabberF, L"Sample Grabber");
			if (FAILED(hr)) {
				tearDown();
				return false;
			}

			hr = m_pGrabberF->QueryInterface(IID_ISampleGrabber, (void**)&m_pGrabber);
			if (FAILED(hr)) {
				tearDown();
				return false;
			}

			hr = m_pGrabber->SetCallback(this, 0);
			if (FAILED(hr)) {
				tearDown();
				return false;
			}

			//MEDIA CONVERSION
			//Get video properties from the stream's mediatype and apply to the grabber (otherwise we don't get an RGB image)
			AM_MEDIA_TYPE mt;
			ZeroMemory(&mt, sizeof(AM_MEDIA_TYPE));

			mt.majortype = MEDIATYPE_Video;
			switch (format) {
			case OF_PIXELS_RGB:
			case OF_PIXELS_BGR:
				mt.subtype = MEDIASUBTYPE_RGB24;
				break;
			case OF_PIXELS_BGRA:
			case OF_PIXELS_RGBA:
				mt.subtype = MEDIASUBTYPE_RGB32;
				break;
			default:
				ofLogError("ofxLavVideoPlayer") << "Trying to set unsupported format this is an internal bug, using default RGB";
				mt.subtype = MEDIASUBTYPE_RGB24;
			}

			mt.formattype = FORMAT_VideoInfo;
			hr = m_pGrabber->SetMediaType(&mt);
			if (FAILED(hr)) {
				tearDown();
				return false;
			}

			// Build the rest of the graph: source -> decoders -> Sample Grabber -> default renderer.
			// (The original code used m_pGraph->RenderFile() here.)
			hr = buildGraph(m_pGraph, filePathW, codecDir, useLav, &m_pSourceFile, bUsingLav);

			if (SUCCEEDED(hr)) {

				//Set Params - One Shot should be false unless you want to capture just one buffer
				hr = m_pGrabber->SetOneShot(FALSE);
				if (FAILED(hr)) {
					ofLogError("ofxLavVideoPlayer") << "unable to set one shot";
					tearDown();
					return false;
				}

				//apparently setting to TRUE causes a small memory leak
				hr = m_pGrabber->SetBufferSamples(FALSE);
				if (FAILED(hr)) {
					ofLogError("ofxLavVideoPlayer") << "unable to set buffer samples";
					tearDown();
					return false;
				}

				//NULL RENDERER//
				//used to give the video stream somewhere to go to.
				hr = CoCreateInstance(CLSID_NullRenderer, NULL, CLSCTX_INPROC_SERVER, IID_IBaseFilter, (void**)(&m_pNullRenderer));
				if (FAILED(hr)) {
					ofLogError("ofxLavVideoPlayer") << "null renderer error";
					tearDown();
					return false;
				}

				hr = m_pGraph->AddFilter(m_pNullRenderer, L"Render");
				if (FAILED(hr)) {
					ofLogError("ofxLavVideoPlayer") << "unable to add null renderer";
					tearDown();
					return false;
				}

				AM_MEDIA_TYPE connectedType;
				ZeroMemory(&connectedType, sizeof(AM_MEDIA_TYPE));

				hr = m_pGrabber->GetConnectedMediaType(&connectedType);
				if (FAILED(hr)) {
					ofLogError("ofxLavVideoPlayer") << "unable to call GetConnectedMediaType (no video stream reached the Sample Grabber)";
					tearDown();
					return false;
				}

				VIDEOINFOHEADER* infoheader = (VIDEOINFOHEADER*)connectedType.pbFormat;
				width = infoheader->bmiHeader.biWidth;
				height = infoheader->bmiHeader.biHeight;
				averageTimePerFrame = infoheader->AvgTimePerFrame / 10000000.0;
				LocalFreeMediaType(connectedType);
				pixels.allocate(width, height, pixelFormat);

				//we need to manually change the output from the renderer window to the null renderer
				IPin* pinIn = 0;
				IPin* pinOut = 0;

				//find the output pin of the sample grabber
				hr = m_pGrabberF->FindPin(L"Out", &pinOut);
				if (FAILED(hr)) {
					ofLogError("ofxLavVideoPlayer") << "failed to find the sample grabber output pin";
					tearDown();
					return false;
				}

				//find the renderer connected to it (more robust than searching for the name "Video Renderer")
				IBaseFilter* pVideoRenderer = NULL;
				IPin* pinRenderer = NULL;
				if (SUCCEEDED(pinOut->ConnectedTo(&pinRenderer))) {
					PIN_INFO info;
					if (SUCCEEDED(pinRenderer->QueryPinInfo(&info))) {
						pVideoRenderer = info.pFilter; // QueryPinInfo AddRef'd it
					}
					pinRenderer->Release();
				}
				else {
					m_pGraph->FindFilterByName(L"Video Renderer", &pVideoRenderer);
				}

				//disconnect the video renderer window
				hr = pinOut->Disconnect();
				if (FAILED(hr)) {
					ofLogError("ofxLavVideoPlayer") << "failed to disconnect grabber output pin";
					if (pVideoRenderer) pVideoRenderer->Release();
					pinOut->Release();
					tearDown();
					return false;
				}

				//we have to remove it as well otherwise the graph builder will reconnect it
				if (pVideoRenderer) {
					hr = m_pGraph->RemoveFilter(pVideoRenderer);
					pVideoRenderer->Release();
					if (FAILED(hr)) {
						ofLogError("ofxLavVideoPlayer") << "failed to remove the default renderer";
						pinOut->Release();
						tearDown();
						return false;
					}
				}

				//now connect the null renderer to the grabber output, if we don't do this no frames will be captured
				hr = m_pNullRenderer->FindPin(L"In", &pinIn);
				if (FAILED(hr)) {
					ofLogError("ofxLavVideoPlayer") << "failed to find the input pin of the null renderer";
					pinOut->Release();
					tearDown();
					return false;
				}

				hr = pinOut->Connect(pinIn, NULL);
				pinIn->Release();
				pinOut->Release();
				if (FAILED(hr)) {
					ofLogError("ofxLavVideoPlayer") << "failed to connect the null renderer";
					tearDown();
					return false;
				}

				//SaveGraphFile(m_pGraph, L"graph.grf"); // uncomment to inspect the graph in GraphStudioNext

				// Run the graph, then stop it again so it's ready to play.
				hr = m_pControl->Run();
				hr = m_pControl->Stop();
				updatePlayState();

				if (FAILED(hr) || width == 0 || height == 0) {
					tearDown();
					ofLogError("ofxLavVideoPlayer") << "Error occured while playing or pausing or opening the file";
					return false;
				}
			}
			else {
				tearDown();
				return false;
			}

			bVideoOpened = true;
			return true;
		}

		void update() {
			if (bVideoOpened) {

				long eventCode = 0;
#ifdef _WIN64
				long long ptrParam1 = 0;
				long long ptrParam2 = 0;
#else
				long ptrParam1 = 0;
				long ptrParam2 = 0;
#endif

				if (curMovieFrame != frameCount) {
					bFrameNew = true;
				}
				else {
					bFrameNew = false;
				}
				curMovieFrame = frameCount;

				while (S_OK == m_pEvent->GetEvent(&eventCode, &ptrParam1, &ptrParam2, 0)) {
					if (eventCode == EC_COMPLETE) {
						if (bLoop) {
							setPosition(0.0);
						}
						else {
							bEndReached = true;
							stop();
							updatePlayState();
						}
					}
					m_pEvent->FreeEventParams(eventCode, ptrParam1, ptrParam2);
				}
			}
		}

		bool isLoaded() {
			return bVideoOpened;
		}

		bool isUsingLav() {
			return bVideoOpened && bUsingLav;
		}

		//volume has to be log corrected/converted
		void setVolume(float volPct) {
			if (isLoaded()) {
				if (volPct < 0) volPct = 0.0;
				if (volPct > 1) volPct = 1.0;

				long vol = log10(volPct) * 4000.0;
				if (vol < -8000) {
					vol = -10000;
				}
				m_pAudio->put_Volume(vol);
			}
		}

		float getVolume() {
			float volPct = 0.0;
			if (isLoaded()) {
				long vol = 0;
				m_pAudio->get_Volume(&vol);
				volPct = powf(10, (float)vol / 4000.0);
			}
			return volPct;
		}

		double getDurationInSeconds() {
			if (isLoaded()) {
				long long lDurationInNanoSecs = 0;
				m_pSeek->GetDuration(&lDurationInNanoSecs);
				double timeInSeconds = (double)lDurationInNanoSecs / 10000000.0;

				return timeInSeconds;
			}
			return 0.0;
		}

		double getCurrentTimeInSeconds() {
			if (isLoaded()) {
				long long lCurrentTimeInNanoSecs = 0;
				m_pSeek->GetCurrentPosition(&lCurrentTimeInNanoSecs);
				double timeInSeconds = (double)lCurrentTimeInNanoSecs / 10000000.0;

				return timeInSeconds;
			}
			return 0.0;
		}

		void setPosition(float pct) {
			if (bVideoOpened) {
				if (pct < 0.0) pct = 0.0;
				if (pct > 1.0) pct = 1.0;

				long long lDurationInNanoSecs = 0;
				m_pSeek->GetDuration(&lDurationInNanoSecs);

				rtNew = ((float)lDurationInNanoSecs * pct);
				hr = m_pSeek->SetPositions(&rtNew, AM_SEEKING_AbsolutePositioning, NULL, AM_SEEKING_NoPositioning);
			}
		}

		float getPosition() {
			if (bVideoOpened) {
				float timeDur = getDurationInSeconds();
				if (timeDur > 0.0) {
					return getCurrentTimeInSeconds() / timeDur;
				}
			}
			return 0.0;
		}

		void setSpeed(float speed) {
			if (bVideoOpened) {
				m_pPosition->put_Rate(speed);
				m_pPosition->get_Rate(&movieRate);
			}
		}

		double getSpeed() {
			return movieRate;
		}

		bool needsRBSwap(ofPixelFormat srcFormat, ofPixelFormat dstFormat) {
			return
				((srcFormat == OF_PIXELS_BGR || srcFormat == OF_PIXELS_BGRA) && (dstFormat == OF_PIXELS_RGB || dstFormat == OF_PIXELS_RGBA)) ||
				((srcFormat == OF_PIXELS_RGB || srcFormat == OF_PIXELS_RGBA) && (dstFormat == OF_PIXELS_BGR || dstFormat == OF_PIXELS_BGRA));
		}

		// DirectShow delivers bottom-up BGR(A) images: flip vertically and swap R/B if needed.
		void processPixels(ofPixels& src, ofPixels& dst) {
			if (needsRBSwap(src.getPixelFormat(), dst.getPixelFormat())) {
				if (src.getPixelFormat() == OF_PIXELS_BGR) {
					dst.allocate(src.getWidth(), src.getHeight(), OF_PIXELS_RGB);
					auto dstLine = dst.getLines().begin();
					auto srcLine = --src.getLines().end();
					auto endLine = dst.getLines().end();
					for (; dstLine != endLine; dstLine++, srcLine--) {
						auto dstPixel = dstLine.getPixels().begin();
						auto srcPixel = srcLine.getPixels().begin();
						auto endPixel = dstLine.getPixels().end();
						for (; dstPixel != endPixel; dstPixel++, srcPixel++) {
							dstPixel[0] = srcPixel[2];
							dstPixel[1] = srcPixel[1];
							dstPixel[2] = srcPixel[0];
						}
					}
				}
				else if (src.getPixelFormat() == OF_PIXELS_BGRA) {
					dst.allocate(src.getWidth(), src.getHeight(), OF_PIXELS_RGBA);
					auto dstLine = dst.getLines().begin();
					auto srcLine = --src.getLines().end();
					auto endLine = dst.getLines().end();
					for (; dstLine != endLine; dstLine++, srcLine--) {
						auto dstPixel = dstLine.getPixels().begin();
						auto srcPixel = srcLine.getPixels().begin();
						auto endPixel = dstLine.getPixels().end();
						for (; dstPixel != endPixel; dstPixel++, srcPixel++) {
							dstPixel[0] = srcPixel[2];
							dstPixel[1] = srcPixel[1];
							dstPixel[2] = srcPixel[0];
							dstPixel[3] = srcPixel[3];
						}
					}
				}
			}
			else {
				src.mirrorTo(dst, true, false);
			}
		}

		void play() {
			if (bVideoOpened) {
				m_pControl->Run();
				bEndReached = false;
				updatePlayState();
			}
		}

		void stop() {
			if (bVideoOpened) {
				if (isPlaying()) {
					setPosition(0.0);
				}
				m_pControl->Stop();
				updatePlayState();
			}
		}

		void setPaused(bool bPaused) {
			if (bVideoOpened) {
				if (bPaused) {
					m_pControl->Pause();
				}
				else {
					m_pControl->Run();
				}
				updatePlayState();
			}
		}

		void updatePlayState() {
			if (bVideoOpened) {
				FILTER_STATE fs;
				hr = m_pControl->GetState(4000, (OAFilterState*)&fs);
				if (hr == S_OK) {
					if (fs == State_Running) {
						bPlaying = true;
						bPaused = false;
					}
					else if (fs == State_Paused) {
						bPlaying = false;
						bPaused = true;
					}
					else if (fs == State_Stopped) {
						bPlaying = false;
						bPaused = false;
					}
				}
			}
		}

		bool isPlaying() {
			return bPlaying;
		}

		bool isPaused() {
			return bPaused;
		}

		bool isLooping() {
			return bLoop;
		}

		void setLoop(bool loop) {
			bLoop = loop;
		}

		bool isMovieDone() {
			return bEndReached;
		}

		float getWidth() {
			return width;
		}

		float getHeight() {
			return height;
		}

		bool isFrameNew() {
			return bFrameNew;
		}

		void nextFrame() {
			//we have to do it like this as the frame based approach is not very accurate
			if (bVideoOpened && (isPlaying() || isPaused())) {
				int curFrame = getCurrentFrameNo();
				float curFrameF = curFrame;
				for (int i = 1; i < 20; i++) {
					setAproximateFrameF(curFrameF + 0.3 * (float)i);
					if (getCurrentFrameNo() >= curFrame + 1) {
						break;
					}
				}
			}
		}

		void preFrame() {
			//we have to do it like this as the frame based approach is not very accurate
			if (bVideoOpened && (isPlaying() || isPaused())) {
				int curFrame = getCurrentFrameNo();
				float curFrameF = curFrame;
				for (int i = 1; i < 20; i++) {
					setAproximateFrameF(curFrameF - 0.3 * (float)i);
					if (getCurrentFrameNo() <= curFrame - 1) {
						break;
					}
				}
			}
		}

		void setAproximateFrameF(float frameF) {
			if (bVideoOpened) {
				float pct = frameF / (float)getAproximateNoFrames();
				if (pct > 1.0) pct = 1.0;
				if (pct < 0.0) pct = 0.0;
				setPosition(pct);
			}
		}

		void setAproximateFrame(int frame) {
			if (bVideoOpened) {
				float pct = (float)frame / (float)getAproximateNoFrames();
				if (pct > 1.0) pct = 1.0;
				if (pct < 0.0) pct = 0.0;
				setPosition(pct);
			}
		}

		int getCurrentFrameNo() {
			if (bVideoOpened) {
				return getPosition() * (float)getAproximateNoFrames();
			}
			return 0;
		}

		int getAproximateNoFrames() {
			if (bVideoOpened && averageTimePerFrame > 0.0) {
				return getDurationInSeconds() / averageTimePerFrame;
			}
			return 0;
		}

		ofPixels& getPixels() {
			if (bVideoOpened && bNewPixels) {
				EnterCriticalSection(&critSection);
				std::swap(backSample, middleSample);
				bNewPixels = false;
				LeaveCriticalSection(&critSection);
				BYTE* ptrBuffer = NULL;
				HRESULT hr = middleSample->GetPointer(&ptrBuffer);
				if (SUCCEEDED(hr)) {
					ofPixels srcBuffer;
					switch (pixelFormat) {
					case OF_PIXELS_RGB:
					case OF_PIXELS_BGR:
						srcBuffer.setFromExternalPixels(ptrBuffer, width, height, OF_PIXELS_BGR);
						break;
					case OF_PIXELS_RGBA:
					case OF_PIXELS_BGRA:
						srcBuffer.setFromExternalPixels(ptrBuffer, width, height, OF_PIXELS_BGRA);
						break;
					default:
						break;
					}
					processPixels(srcBuffer, pixels);
				}
			}
			return pixels;
		}

	protected:

		HRESULT hr;                         // COM return value
		IGraphBuilder* m_pGraph;            // Graph Builder interface
		IMediaControl* m_pControl;          // Media Control interface
		IMediaEvent* m_pEvent;            // Media Event interface
		IMediaSeeking* m_pSeek;             // Media Seeking interface
		IMediaPosition* m_pPosition;
		IBasicAudio* m_pAudio;            // Audio Settings interface
		ISampleGrabber* m_pGrabber;
		IBaseFilter* m_pSourceFile;        // LAV Splitter Source (only when LAV was used)
		IBaseFilter* m_pGrabberF;
		IBasicVideo* m_pBasicVideo;
		IBaseFilter* m_pNullRenderer;

		REFERENCE_TIME timeNow;             // Used for FF & REW of movie, current time
		LONGLONG lPositionInSecs;           // Time in  seconds
		LONGLONG lDurationInNanoSecs;       // Duration in nanoseconds
		LONGLONG lTotalDuration;            // Total duration
		REFERENCE_TIME rtNew;               // Reference time of movie
		long lPosition;                     // Desired position of movie used in FF & REW
		long lvolume;                       // The volume level in 1/100ths dB Valid values range from -10,000 (silence) to 0 (full volume)
		long evCode;                        // event variable, used to in file to complete wait.

		long width, height;

		double averageTimePerFrame;

		bool bFrameNew;
		bool bNewPixels;
		bool bVideoOpened;
		bool bPlaying;
		bool bPaused;
		bool bLoop;
		bool bEndReached;
		bool bUsingLav;
		double movieRate;
		int curMovieFrame;
		int frameCount;

		CRITICAL_SECTION critSection;
		std::unique_ptr<IMediaSample, std::function<void(IMediaSample*)>> backSample;
		std::unique_ptr<IMediaSample, std::function<void(IMediaSample*)>> middleSample;
		ofPixels pixels;
		ofPixelFormat pixelFormat;
	};

} // namespace ofxLav

//----------------------------------------------------------------------------------------------------------------------------------------------------------------
// OF SPECIFIC IMPLEMENTATION BELOW
//----------------------------------------------------------------------------------------------------------------------------------------------------------------

namespace {
	// Returned by getPixels() when nothing is loaded, instead of dereferencing a null player.
	ofPixels& emptyPixels() {
		static ofPixels pixels;
		return pixels;
	}
}

void ofxLavVideoPlayer::setCodecDirectory(const std::string& directory) {
	ofxLav::codecDirectorySetting = directory;
}

std::string ofxLavVideoPlayer::getCodecDirectory() {
	return ofxLav::toUtf8(ofxLav::getCodecDirectoryW());
}

void ofxLavVideoPlayer::setFallbackToSystemFilters(bool fallback) {
	ofxLav::fallbackToSystemFilters = fallback;
}

bool ofxLavVideoPlayer::getFallbackToSystemFilters() {
	return ofxLav::fallbackToSystemFilters;
}

ofxLavVideoPlayer::ofxLavVideoPlayer()
	:pixelFormat(OF_PIXELS_RGB) {

}

ofxLavVideoPlayer::ofxLavVideoPlayer(ofxLavVideoPlayer&& other)
	:player(std::move(other.player))
	, pixelFormat(std::move(other.pixelFormat)) {

}

ofxLavVideoPlayer& ofxLavVideoPlayer::operator=(ofxLavVideoPlayer&& other) {
	if (&other == this) {
		return *this;
	}

	player = std::move(other.player);
	pixelFormat = std::move(other.pixelFormat);
	return *this;
}

bool ofxLavVideoPlayer::load(std::string path) {
	// LAV Splitter Source needs an absolute path; URLs are passed through untouched.
	bool isUrl = path.find("://") != std::string::npos;
	if (!isUrl) {
		path = ofToDataPath(path, true);
	}

	close();
	std::wstring pathW = ofxLav::toWide(path);
	std::wstring codecDir = ofxLav::getCodecDirectoryW();

	player = std::make_shared<ofxLav::DirectShowVideo>();
	bool loadOk = player->loadMovie(pathW, pixelFormat, true, codecDir);

	if (!loadOk && ofxLav::fallbackToSystemFilters) {
		ofLogWarning("ofxLavVideoPlayer") << "LAV Filters could not open \"" << path << "\", trying the system filters";
		loadOk = player->loadMovie(pathW, pixelFormat, false, codecDir);
	}

	if (!loadOk) {
		ofLogError("ofxLavVideoPlayer") << "Cannot load video \"" << path << "\". Codec folder: "
			<< ofxLav::toUtf8(codecDir);
	}
	else if (!player->isUsingLav()) {
		ofLogNotice("ofxLavVideoPlayer") << "\"" << path << "\" is played without LAV Splitter Source";
	}
	return loadOk;
}

void ofxLavVideoPlayer::close() {
	player.reset();
}

void ofxLavVideoPlayer::update() {
	if (player && player->isLoaded()) {
		player->update();
	}
}

void ofxLavVideoPlayer::play() {
	if (player && player->isLoaded()) {
		player->play();
	}
}

void ofxLavVideoPlayer::stop() {
	if (player && player->isLoaded()) {
		player->stop();
	}
}

bool ofxLavVideoPlayer::isFrameNew() const {
	return (player && player->isFrameNew());
}

const ofPixels& ofxLavVideoPlayer::getPixels() const {
	if (!player) {
		return emptyPixels();
	}
	return player->getPixels();
}

ofPixels& ofxLavVideoPlayer::getPixels() {
	if (!player) {
		return emptyPixels();
	}
	return player->getPixels();
}

float ofxLavVideoPlayer::getWidth() const {
	if (player && player->isLoaded()) {
		return player->getWidth();
	}
	return 0.0;
}

float ofxLavVideoPlayer::getHeight() const {
	if (player && player->isLoaded()) {
		return player->getHeight();
	}
	return 0.0;
}

bool ofxLavVideoPlayer::isPaused() const {
	return (player && player->isPaused());
}

bool ofxLavVideoPlayer::isLoaded() const {
	return (player && player->isLoaded());
}

bool ofxLavVideoPlayer::isPlaying() const {
	return (player && player->isPlaying());
}

bool ofxLavVideoPlayer::setPixelFormat(ofPixelFormat pixelFormat) {
	switch (pixelFormat) {
	case OF_PIXELS_RGB:
	case OF_PIXELS_BGR:
	case OF_PIXELS_BGRA:
	case OF_PIXELS_RGBA:
		this->pixelFormat = pixelFormat;
		return true;
	default:
		return false;
	}
}

ofPixelFormat ofxLavVideoPlayer::getPixelFormat() const {
	return this->pixelFormat;
}

float ofxLavVideoPlayer::getPosition() const {
	if (player && player->isLoaded()) {
		return player->getPosition();
	}
	return 0.0;
}

float ofxLavVideoPlayer::getSpeed() const {
	if (player && player->isLoaded()) {
		return player->getSpeed();
	}
	return 0.0;
}

float ofxLavVideoPlayer::getDuration() const {
	if (player && player->isLoaded()) {
		return player->getDurationInSeconds();
	}
	return 0.0;
}

bool ofxLavVideoPlayer::getIsMovieDone() const {
	return (player && player->isMovieDone());
}

void ofxLavVideoPlayer::setPaused(bool bPause) {
	if (player && player->isLoaded()) {
		player->setPaused(bPause);
	}
}

void ofxLavVideoPlayer::setPosition(float pct) {
	if (player && player->isLoaded()) {
		player->setPosition(pct);
	}
}

void ofxLavVideoPlayer::setVolume(float volume) {
	if (player && player->isLoaded()) {
		player->setVolume(volume);
	}
}

void ofxLavVideoPlayer::setLoopState(ofLoopType state) {
	if (player) {
		if (state == OF_LOOP_NONE) {
			player->setLoop(false);
		}
		else if (state == OF_LOOP_NORMAL) {
			player->setLoop(true);
		}
		else {
			ofLogError("ofxLavVideoPlayer") << " cannot set loop of type palindrome ";
		}
	}
}

void ofxLavVideoPlayer::setSpeed(float speed) {
	if (player && player->isLoaded()) {
		player->setSpeed(speed);
	}
}

int ofxLavVideoPlayer::getCurrentFrame() const {
	if (player && player->isLoaded()) {
		return player->getCurrentFrameNo();
	}
	return 0;
}

int ofxLavVideoPlayer::getTotalNumFrames() const {
	if (player && player->isLoaded()) {
		return player->getAproximateNoFrames();
	}
	return 0;
}

ofLoopType ofxLavVideoPlayer::getLoopState() const {
	if (player) {
		if (player->isLooping()) {
			return OF_LOOP_NORMAL;
		}
	}
	return OF_LOOP_NONE;
}

void ofxLavVideoPlayer::setFrame(int frame) {
	if (player && player->isLoaded()) {
		frame = ofClamp(frame, 0, getTotalNumFrames());
		player->setAproximateFrame(frame);
	}
}  // frame 0 = first frame...

void ofxLavVideoPlayer::firstFrame() {
	setPosition(0.0);
}

void ofxLavVideoPlayer::nextFrame() {
	if (player && player->isLoaded()) {
		player->nextFrame();
	}
}

void ofxLavVideoPlayer::previousFrame() {
	if (player && player->isLoaded()) {
		player->preFrame();
	}
}

bool ofxLavVideoPlayer::isUsingLav() const {
	return (player && player->isUsingLav());
}

#endif // TARGET_WIN32