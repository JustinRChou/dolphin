// Copyright (C) 2003 Dolphin Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official SVN repository and contact information can be found at
// http://code.google.com/p/dolphin-emu/

#include <tchar.h>
#include <windows.h>
#include <d3dx11.h>

#include <wx/wx.h>
#include <wx/notebook.h>

#include "Common.h"
#include "Atomic.h"
#include "Thread.h"
#include "LogManager.h"

#include "VideoConfig.h"
#include "Fifo.h"
#include "OpcodeDecoding.h"
#include "BPStructs.h"
#include "VertexLoaderManager.h"
#include "VertexShaderManager.h"
#include "PixelShaderManager.h"
#include "CommandProcessor.h"
#include "PixelEngine.h"
#include "OnScreenDisplay.h"
#include "VideoState.h"
#include "XFBConvert.h"
#include "Render.h"

#include "MainBase.h"
#include "main.h"
#include "resource.h"
#include "VideoConfigDiag.h"
#include "TextureCache.h"
#include "VertexManager.h"
#include "VertexShaderCache.h"
#include "PixelShaderCache.h"
#include "D3DTexture.h"
#include "D3DUtil.h"
#include "W32Util/Misc.h"
#include "EmuWindow.h"
#include "FramebufferManager.h"
#include "DLCache.h"

HINSTANCE g_hInstance = NULL;

bool IsD3D()
{
	return true;
}

// This is used for the functions right below here which use wxwidgets
#if defined(HAVE_WX) && HAVE_WX
WXDLLIMPEXP_BASE void wxSetInstance(HINSTANCE hInst);
#endif

void *DllDebugger(void *_hParent, bool Show)
{
	return NULL;
}

#if defined(HAVE_WX) && HAVE_WX
	class wxDLLApp : public wxApp
	{
		bool OnInit()
		{
			return true;
		}
	};
	IMPLEMENT_APP_NO_MAIN(wxDLLApp)
	WXDLLIMPEXP_BASE void wxSetInstance(HINSTANCE hInst);
#endif

BOOL APIENTRY DllMain(HINSTANCE hinstDLL, DWORD dwReason, LPVOID lpvReserved)
{
	switch (dwReason)
	{
	case DLL_PROCESS_ATTACH:
		{
#if defined(HAVE_WX) && HAVE_WX
			wxSetInstance((HINSTANCE)hinstDLL);
			wxInitialize();
#endif
		}
		break;
	case DLL_PROCESS_DETACH:
#if defined(HAVE_WX) && HAVE_WX
		wxUninitialize();
#endif
		break;
	}

	g_hInstance = hinstDLL;
	return TRUE;
}

unsigned int Callback_PeekMessages()
{
	MSG msg;
	while (PeekMessage(&msg, 0, 0, 0, PM_REMOVE))
	{
		if (msg.message == WM_QUIT)
			return FALSE;
		TranslateMessage(&msg);
		DispatchMessage(&msg);
	}
	return TRUE;
}


void UpdateFPSDisplay(const char *text)
{
	char temp[512];
	sprintf_s(temp, 512, "SVN R%s: DX11: %s", svn_rev_str, text);
	SetWindowTextA(EmuWindow::GetWnd(), temp);
}

void GetDllInfo(PLUGIN_INFO* _PluginInfo)
{
	_PluginInfo->Version = 0x0100;
	_PluginInfo->Type = PLUGIN_TYPE_VIDEO;
#ifdef DEBUGFAST
	sprintf_s(_PluginInfo->Name, 100, "Dolphin Direct3D11 (DebugFast)");
#elif defined _DEBUG
	sprintf_s(_PluginInfo->Name, 100, "Dolphin Direct3D11 (Debug)");
#else
	sprintf_s(_PluginInfo->Name, 100, "Dolphin Direct3D11");
#endif
}

void SetDllGlobals(PLUGIN_GLOBALS* _pPluginGlobals)
{
	globals = _pPluginGlobals;
	LogManager::SetInstance((LogManager*)globals->logManager);
}

void DllConfig(void *_hParent)
{
	std::vector<std::string> adapters;
	
	IDXGIFactory* factory;
	IDXGIAdapter* ad;
	const HRESULT hr = CreateDXGIFactory(__uuidof(IDXGIFactory), (void**)&factory);
	if (FAILED(hr))
		PanicAlert("Failed to create IDXGIFactory object");

	char tmpstr[512] = {};

	DXGI_ADAPTER_DESC desc;
	while (factory->EnumAdapters((UINT)adapters.size(), &ad) != DXGI_ERROR_NOT_FOUND)
	{
		ad->GetDesc(&desc);
		WideCharToMultiByte(/*CP_UTF8*/CP_ACP, 0, desc.Description, -1, tmpstr, 512, 0, false);
		adapters.push_back(tmpstr);
	}

	VideoConfigDiag *const diag = new VideoConfigDiag((wxWindow*)_hParent, "Direct3D11", adapters);
	diag->ShowModal();
	diag->Destroy();

	g_Config.Save((std::string(File::GetUserPath(D_CONFIG_IDX)) + "gfx_dx11.ini").c_str());
}

void Initialize(void *init)
{
	frameCount = 0;
	SVideoInitialize *_pVideoInitialize = (SVideoInitialize*)init;
	// Create a shortcut to _pVideoInitialize that can also update it
	g_VideoInitialize = *(_pVideoInitialize);
	InitXFBConvTables();

	g_Config.Load((std::string(File::GetUserPath(D_CONFIG_IDX)) + "gfx_dx11.ini").c_str());
	g_Config.GameIniLoad(globals->game_ini);
	UpdateActiveConfig();

	g_VideoInitialize.pWindowHandle = (void*)EmuWindow::Create((HWND)g_VideoInitialize.pWindowHandle, g_hInstance, _T("Loading - Please wait."));
	if (g_VideoInitialize.pWindowHandle == NULL)
	{
		ERROR_LOG(VIDEO, "An error has occurred while trying to create the window.");
		return;
	}

	g_VideoInitialize.pPeekMessages = &Callback_PeekMessages;
	g_VideoInitialize.pUpdateFPSDisplay = &UpdateFPSDisplay;

	_pVideoInitialize->pPeekMessages = g_VideoInitialize.pPeekMessages;
	_pVideoInitialize->pUpdateFPSDisplay = g_VideoInitialize.pUpdateFPSDisplay;

	// Now the window handle is written
	_pVideoInitialize->pWindowHandle = g_VideoInitialize.pWindowHandle;

	OSD::AddMessage("Dolphin Direct3D11 Video Plugin.", 5000);
	s_PluginInitialized = true;
}

void Video_Prepare()
{
	// Better be safe...
	s_efbAccessRequested = FALSE;
	s_FifoShuttingDown = FALSE;
	s_swapRequested = FALSE;

	// internal interfaces
	g_renderer = new DX11::Renderer;
	g_texture_cache = new DX11::TextureCache;
	g_vertex_manager = new DX11::VertexManager;
	VertexShaderCache::Init();
	PixelShaderCache::Init();
	D3D::InitUtils();

	// VideoCommon
	BPInit();
	Fifo_Init();
	VertexLoaderManager::Init();
	OpcodeDecoder_Init();
	VertexShaderManager::Init();
	PixelShaderManager::Init();
	CommandProcessor::Init();
	PixelEngine::Init();
	DLCache::Init();

	// Tell the host that the window is ready
	g_VideoInitialize.pCoreMessage(WM_USER_CREATE);
}

void Shutdown()
{
	s_PluginInitialized = false;

	s_efbAccessRequested = FALSE;
	s_FifoShuttingDown = FALSE;
	s_swapRequested = FALSE;

	// VideoCommon
	DLCache::Shutdown();
	Fifo_Shutdown();
	CommandProcessor::Shutdown();
	PixelShaderManager::Shutdown();
	VertexShaderManager::Shutdown();
	OpcodeDecoder_Shutdown();
	VertexLoaderManager::Shutdown();

	// internal interfaces
	D3D::ShutdownUtils();
	PixelShaderCache::Shutdown();
	VertexShaderCache::Shutdown();
	delete g_vertex_manager;
	delete g_texture_cache;
	delete g_renderer;
	EmuWindow::Close();

	s_PluginInitialized = false;
}

void DoState(unsigned char **ptr, int mode)
{
	// Clear texture cache because it might have written to RAM
	CommandProcessor::FifoCriticalEnter();
	TextureCache::Invalidate(false);
	CommandProcessor::FifoCriticalLeave();
	// No need to clear shader caches
	PointerWrap p(ptr, mode);
	VideoCommon_DoState(p);
}
