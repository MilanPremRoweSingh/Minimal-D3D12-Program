// Minimises files included by Windows.h
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <shellapi.h> // For CommandLineToArgvW, which converts unicode cmd line string to pointers ala argv/argc

// The min/max macros conflict with like-named member functions.
// Only use std::min and std::max defined in <algorithm>.
#if defined(min)
#undef min
#endif

#if defined(max)
#undef max
#endif

// In order to define a function called CreateWindow, the Windows macro needs to
// be undefined.
#if defined(CreateWindow)
#undef CreateWindow
#endif

// Windows Runtime Library. Needed for Microsoft::WRL::ComPtr<> template class.
// All DX12 objects are COM objects so we need this to create 'smart pointers' to them
#include <wrl.h>
using namespace Microsoft::WRL;

// DirectX 12 specific headers.
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <DirectXMath.h>

// D3D12 extension library.
// Source: https://github.com/Microsoft/DirectX-Graphics-Samples/tree/master/Libraries/D3DX12
#include "d3dx12.h"

// STL Headers
#include <algorithm>
#include <cassert>
#include <chrono>

// Helper functions
#include "Helpers.h"

// The number of swap chain back buffers
const uint8_t g_NumFrames = 3;

// Use WARP adapter, a software rasterizer which allows access to full set of advanced options which may not be available on HW
// Docs: https://docs.microsoft.com/en-us/windows/win32/direct3darticles/directx-warp
bool g_UseWARP = false;

// Client Area Dimensions
uint32_t g_ClientWidth = 1024;
uint32_t g_ClientHeight = 768;

// True once DX12 objs are initialised
bool b_IsInitialized = false;

// Window Handle
HWND g_hWnd;
// Window Rectangle (used to store window dims when going to fullscreen state)
RECT g_WindowRect;

// DirectX 12 Objects
// DirectX Device Object
ComPtr<ID3D12Device> g_Device; 
ComPtr<ID3D12CommandQueue> g_CommandQueue;
// Swap Chain - responsible for presenting rendered image to window
ComPtr<IDXGISwapChain4> g_SwapChain; 
// Traces pointers to back buffers created with swap chain
ComPtr<ID3D12Resource> g_BackBuffers[g_NumFrames]; 
// CmdList for all GPU Commands, only need one since all commands will be submitted from main thread
ComPtr<ID3D12GraphicsCommandList> g_CommandList; 
// Backing memory for recording GPU commands into command list. Need one per back buffer as cannot reset one until all commands for that allocator have been executed by cmd q
// Resetting before all cmds executed results in a COMMAND_ALLOCATOR_SYNC. Therefore must have one allocator per back buffer.
ComPtr<ID3D12CommandAllocator> g_CommandAllocators[g_NumFrames];
// Backbuffer textures of swap chain described with Render Target Views (RTVs). RTVs descrive the location, dimensions and format of texture in GPU memory.
// Used to clear backbuffer of render target, as well as render geometry to screen. RTVs are stored in Descriptor Heap
ComPtr<ID3D12DescriptorHeap> g_RTVDescriptorHeap;
// Size of a descriptor in heap is device dependent. Size of single element needs to be queried and stored during initialisation
UINT g_RTVDescriptorSize;
UINT g_CurrentBackBufferIndex;

// Synchronization Objects
ComPtr<ID3D12Fence> g_Fence; // Only one since we have only one command queue
uint64_t g_FenceValue;
// Stores values used to signal command queue for each frame 'in-flight'
uint64_t g_FrameFenceValues[g_NumFrames] = {};
// Handle to OS event obj used to receive notification that fence has reached a specific value
HANDLE g_FenceEvent;

// Swap Chain Present stuff
// By default, enable V-Sync, toggled with V key
bool g_VSync = true;
bool g_TearingSupported = false;
// By Default, use windowed mode, Alt+Enter or F11 to toggle
bool g_Fullscreen = false;

// Windows Callback Function
LRESULT CALLBACK WndProc( HWND, UINT, WPARAM, LPARAM );

// Update settings based on cmd args
void ParseCommandLineArgs()
{
	int argc;
	wchar_t** argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc);
	for (size_t i = 0; i < argc; i++)
	{
		if (::wcscmp(argv[i], L"-w") == 0 || ::wcscmp(argv[i], L"--width") == 0)
		{
			g_ClientWidth = ::wcstol( argv[++i], nullptr, 10 );
		}
		if (::wcscmp(argv[i], L"-h") == 0 || ::wcscmp(argv[i], L"--height") == 0)
		{
			g_ClientWidth = ::wcstol( argv[++i], nullptr, 10 );
		}
		if (::wcscmp(argv[i], L"-warp") == 0 || ::wcscmp(argv[i], L"--warp") == 0)
		{
			g_UseWARP = true;
		}
	}

	// Free memory allocated by ::GetCommandLineW()
	::LocalFree( argv );
}

// Enable DX12 Debug Layer
void EnableDebugLayer()
{
#if defined(_DEBUG)
	// Enable debug layer before using DX12 so all possible errors generated
	// while creating DX12 objects are caught by the debug layer
	ComPtr<ID3D12Debug> debugInterface;
	ThrowIfFailed(D3D12GetDebugInterface(IID_PPV_ARGS(&debugInterface)));
#endif
}

void RegisterWindowClass( HINSTANCE hInst, const wchar_t* windowClassName )
{
	// Register a window class for creating our render window with.
	WNDCLASSEXW windowClass = {};

	// Size of windowClass
	windowClass.cbSize = sizeof(WNDCLASSEX);
	// CS_HREDRAW -> makes the entire window redraw when width changes, CS_VREDRAW -> same but height
	windowClass.style = CS_HREDRAW | CS_VREDRAW;
	// Pointer to windows procesdure that will handle window messages for window creates with this class
	windowClass.lpfnWndProc = &WndProc;
	// Number of extra bytes to alloc follwing window class structure
	windowClass.cbClsExtra = 0;
	// Number of extra bytes to alloc follwing window instance
	windowClass.cbWndExtra = 0;
	// Handle to instance that contains window proc for this class. Received from WinMain
	windowClass.hInstance = hInst;
	// Handle to Window Class Icon, set to default
	windowClass.hIcon = ::LoadIcon(hInst, NULL);
	// Handle to Window Class Cursor, set to default
	windowClass.hCursor = ::LoadCursor(NULL, IDC_ARROW);
	// Handle to Class Background Brush, which paints the background and all the little window bits (I think)
	windowClass.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
	// String specifying resource name of class menu. No default menu if NULL
	windowClass.lpszMenuName = NULL;
	// Name of window class
	windowClass.lpszClassName = windowClassName;
	// Handle to Window Class Small Icon, set to default
	windowClass.hIconSm = ::LoadIcon(hInst, NULL);

	// Register class, failed if returns 0
	static ATOM atom = ::RegisterClassExW(&windowClass);
	assert(atom > 0);
}

HWND CreateWindow(const wchar_t* windowClassName, HINSTANCE hInst, const wchar_t* windowTitle,
					uint32_t width, uint32_t height)
{
	// Get primary display resolution
	int screenWidth = ::GetSystemMetrics(SM_CXSCREEN);
	int screenHeight = ::GetSystemMetrics(SM_CYSCREEN);

	// RECT = { left, top, right, bottom }
	RECT windowRect = { 0, 0, static_cast<LONG>(width), static_cast<LONG>(height) };
	// Adjust window rect to fit client specified dimensions
	// The WS_OVERLAPPEDWINDOW window style describes a window that can be minimized, and maximized, and has a thick window frame.
	::AdjustWindowRect(&windowRect, WS_OVERLAPPEDWINDOW, FALSE);

	int windowWidth = windowRect.right - windowRect.left;
	int windowHeight = windowRect.bottom - windowRect.top;

	// Center window by specifying x,y of top left corner
	int windowX = std::max<int>(0, (screenWidth - windowWidth) / 2);
	int windowY = std::max<int>(0, (screenHeight - windowHeight) / 2);

	HWND hWnd = ::CreateWindowExW(
		NULL, // Extended window style
		windowClassName,
		windowTitle,
		WS_OVERLAPPEDWINDOW,
		windowX,
		windowY,
		windowWidth,
		windowHeight,
		NULL, // Handle to Window parent
		NULL, // Handle to menu or specifies child window identifier
		hInst,
		nullptr // Pointer to a val passed to window via CREATESTRUCT
	);

	assert(hWnd && "Failed to create window");

	return hWnd;
}

ComPtr<IDXGIAdapter4> GetAdapter(bool useWarp)
{
	// Enabkes creating DXGI Objects
	ComPtr<IDXGIFactory4> dxgiFactory;
	UINT createFactoryFlags = 0;
#if defined(_DEBUG)
	// Enables errors to be caught during device creation and when querying for adapters
	createFactoryFlags = DXGI_CREATE_FACTORY_DEBUG;
#endif

	ThrowIfFailed(CreateDXGIFactory2(createFactoryFlags, IID_PPV_ARGS(&dxgiFactory)));

	ComPtr<IDXGIAdapter1> dxgiAdapter1;
	ComPtr<IDXGIAdapter4> dxgiAdapter4;

	// If WARP device is used, IDXGIFactory4::EnumWarpAdapter can be used to create WARP adapter directly
	if (useWarp)
	{
		// Create warp adapter, takes an Adapter1 pointer
		ThrowIfFailed(dxgiFactory->EnumWarpAdapter(IID_PPV_ARGS(&dxgiAdapter1)));
		// Cast Adapter1 to Adapter4, since GetAdapter returns a pointer to an Adapter4
		// Note us As to cast COM objects, static_cast is not safe or reliable
		ThrowIfFailed(dxgiAdapter1.As(&dxgiAdapter4));
	}
	// Otherwise, DXGI factory is used to query for hardware adapters
	else
	{
		SIZE_T maxDedicatedVideoMemory = 0;
		// IDXGIFactory1::EnumAdapters1 is used to enumerate available GPU adaters in system
		for (UINT i = 0; dxgiFactory->EnumAdapters1(i, &dxgiAdapter1) != DXGI_ERROR_NOT_FOUND; i++)
		{
			// Get description of dxgiAdapter1
			DXGI_ADAPTER_DESC1 dxgiAdapterDesc1;
			dxgiAdapter1->GetDesc1(&dxgiAdapterDesc1);

			// Check if adapter can create a D3D12 Device. Adapter with largest VRAM is favoured
			// Note this does not actually create the D3D12 device
			if ((dxgiAdapterDesc1.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0 && // If adapter is not a software adapter (ala WARP)
				SUCCEEDED(D3D12CreateDevice(dxgiAdapter1.Get(),
					D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), nullptr)) && // If a D3D12 device can be created
				dxgiAdapterDesc1.DedicatedVideoMemory > maxDedicatedVideoMemory ) // Prefer larger dedicated Video Mem
			{
				maxDedicatedVideoMemory = dxgiAdapterDesc1.DedicatedVideoMemory;
				ThrowIfFailed(dxgiAdapter1.As(&dxgiAdapter4));
			}
		}
	}

	return dxgiAdapter4;
}

// Creates DX12 Device. Can be seen as memory context that tracks allocs in GPU memory.
// Used to create resources, but not directly used to issue draw/dispatch commands.
// Note Destroying device makes all resource allocations done by it invalid
ComPtr<ID3D12Device2> CreateDevice(ComPtr<IDXGIAdapter4> adapter)
{
	ComPtr<ID3D12Device2> d3d12Device2;
	ThrowIfFailed(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&d3d12Device2)));

	// Enable debug messages in debug mode
#if defined(_DEBUG)
	ComPtr<ID3D12InfoQueue> pInfoQueue;
	if (SUCCEEDED(d3d12Device2.As(&pInfoQueue)))
	{
		pInfoQueue->GetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR);
		pInfoQueue->GetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_WARNING);
		pInfoQueue->GetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION);

		// Messages can also be suprressed by cateogry, severity or ID
		
		// Supress Categories
		//D3D12_MESSAGE_CATEGORY Categories[] = {}

		// Suppress based on Severity
		D3D12_MESSAGE_SEVERITY Severities[] =
		{
			D3D12_MESSAGE_SEVERITY_INFO
		};

		// Suppress by ID
		D3D12_MESSAGE_ID DenyIDs[] = 
		{
			D3D12_MESSAGE_ID_CLEARDEPTHSTENCILVIEW_MISMATCHINGCLEARVALUE, // Occures when RT is cleared with a clear color other than that specified at resource creation
			D3D12_MESSAGE_ID_MAP_INVALID_NULLRANGE,  // | Occurs when frame is captured using Graphics Debugger 
			D3D12_MESSAGE_ID_UNMAP_INVALID_NULLRANGE // | in Visual Studio. Best to ignore as debugger likely wont be fixed
			// etc.
		};

		// Supress those specified above by a filter
		D3D12_INFO_QUEUE_FILTER NewFilter = {};
		//NewFilter.DenyList.NumCategories = _countof(Categories);
		//NewFilter.DenyList.pCategoryList = Categories;
		NewFilter.DenyList.NumSeverities = _countof(Severities);
		NewFilter.DenyList.pSeverityList = Severities;
		NewFilter.DenyList.NumIDs = _countof(DenyIDs);
		NewFilter.DenyList.pIDList = DenyIDs;

		ThrowIfFailed(pInfoQueue->PushStorageFilter(&NewFilter));
	}
#endif

	return d3d12Device2;
}

ComPtr<ID3D12CommandQueue> CreateCommandQueue(ComPtr<ID3D12Device2> device, D3D12_COMMAND_LIST_TYPE type)
{
	ComPtr<ID3D12CommandQueue> d3d12CommandQueue;

	D3D12_COMMAND_QUEUE_DESC desc = {};
	// Type:
	// - D3D12_COMMAND_LIST_TYPE_COPY : Can exec copy commands
	// - D3D12_COMMAND_LIST_TYPE_DIRECT : SuperSet of COPY + Compute commands
	// - D3D12_COMMAND_LIST_TYPE_DIRECT : SuperSet of COMPUTE + Draw commands
	desc.Type = type;
	// Specifies prority of command Queue
	desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
	// Flags: 
	// - D3D12_COMMAND_QUEUE_FLAG_NONE
	// - D3D12_COMMAND_QUEUE_FLAG_DISABLE_GPU_TIMEOUT
	desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
	// Identifier for when there are mutliple GPU nodes 
	desc.NodeMask = 0;

	ThrowIfFailed(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&d3d12CommandQueue)));

	return d3d12CommandQueue;
}

bool CheckTearingSupport() 
{
	BOOL allowTearing = FALSE;

	// Rather than create the DXGI 1.5 factory interface directly, we create the
	// DXGI 1.4 interface and query for the 1.5 interface. This is to enable the 
	// graphics debugging tools which will not support the 1.5 factory interface 
	// until a future update.
	ComPtr<IDXGIFactory4> factory4;
	if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory4))))
	{
		ComPtr<IDXGIFactory5> factory5;
		// Query DXGI 1.5 factory interface
		if (SUCCEEDED(factory4.As(&factory5)))
		{
			// Check Feature Support
			if (FAILED(factory5->CheckFeatureSupport(
				DXGI_FEATURE_PRESENT_ALLOW_TEARING,
				&allowTearing, sizeof(allowTearing))))
			{
				allowTearing = FALSE;
			}
		}
	}

	return allowTearing == TRUE;
}

ComPtr<IDXGISwapChain4> CreateSwapChain(HWND hWnd, ComPtr<ID3D12CommandQueue> commandQueue,
	uint32_t width, uint32_t height, uint32_t bufferCount)
{
	ComPtr<IDXGISwapChain4> dxgiSwapChain4;
	
	// Create DXGI Factory
	ComPtr<IDXGIFactory4> dxgiFactory4;
	UINT createFactoryFlags = 0;
#if defined(_DEBUG)
	createFactoryFlags = DXGI_CREATE_FACTORY_DEBUG;
#endif
	ThrowIfFailed(CreateDXGIFactory2(createFactoryFlags, IID_PPV_ARGS(&dxgiFactory4)));

	// Setup Swap Chain Desc
	DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
	swapChainDesc.Width = width;
	swapChainDesc.Height = height;
	swapChainDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; 
	// Used for 3D
	swapChainDesc.Stereo = FALSE;
	// Use for bitblt transfer models, must be { 1, 0 } for flip models
	swapChainDesc.SampleDesc = { 1, 0 };
	// Describes surface usage and CPU access options for back buffer.
	//  DXGI_USAGE_RENDER_TARGET_OUTPUT : Use the surface or resource as an output render target.
	swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	// Buffer count. With Fullscreen swap chain, front buffer is typically included in this count
	swapChainDesc.BufferCount = bufferCount;
	// Defines behaviour when back buffer size does not match target output size.
	swapChainDesc.Scaling = DXGI_SCALING_STRETCH;
	// Defines how swapping between front and back buffers.
	// Docs; https://docs.microsoft.com/en-us/windows/win32/api/dxgi/ne-dxgi-dxgi_swap_effect
	swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
	// Identifies transparency behaviour
	// Docs: https://docs.microsoft.com/en-us/windows/win32/api/dxgi1_2/ne-dxgi1_2-dxgi_alpha_mode
	swapChainDesc.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
	// It is recommended to always allow tearing support if tearing support is available
	swapChainDesc.Flags = CheckTearingSupport() ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;

	// Create Swap Chain
	ComPtr<IDXGISwapChain1> swapChain1;
	ThrowIfFailed(dxgiFactory4->CreateSwapChainForHwnd(
		commandQueue.Get(),
		hWnd,
		&swapChainDesc,
		nullptr, // Pointer to desc for full screen swap chain, NULL creates windowed swap chain
		nullptr, // Pointer to IDXGIOutput interface for the output to restrict content to
		&swapChain1
	));

	// Disable Alt+Enter to fullscreen, as we will handle that manually
	ThrowIfFailed(dxgiFactory4->MakeWindowAssociation(hWnd, DXGI_MWA_NO_ALT_ENTER));

	ThrowIfFailed(swapChain1.As(&dxgiSwapChain4));
	return dxgiSwapChain4;
}

// Creates Descriptor Heap
// Descriptor heap can be considered an array of resouce views.
// UAV, SRV, CBV can be stored in the same desc heap, but RTV and Sampler views each require their own
ComPtr<ID3D12DescriptorHeap> CreateDescriptorHeap(ComPtr<ID3D12Device2> device, 
	D3D12_DESCRIPTOR_HEAP_TYPE type, uint32_t numDescriptors)
{
	ComPtr<ID3D12DescriptorHeap> descriptorHeap;

	// Setup Desc Heap Desc
	D3D12_DESCRIPTOR_HEAP_DESC desc = {};
	desc.NumDescriptors = numDescriptors;
	// What will this desc heap store? UAV/SRV/CBV, SAMPLER, RTV or DSV
	desc.Type = type;
	// desc.Flags (https://docs.microsoft.com/en-us/windows/win32/api/d3d12/ne-d3d12-d3d12_descriptor_heap_flags)
	// desc.NodeMask, similar to CommandQueue desc.NodeMask

	ThrowIfFailed(device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&descriptorHeap)));

	return descriptorHeap;
}

void UpdateRenderTargetViews(ComPtr<ID3D12Device2> device, ComPtr<IDXGISwapChain4> swapChain,
	ComPtr<ID3D12DescriptorHeap> descriptorHeap)
{
	// Query the size of a single descriptor in the descheap
	auto rtvDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

	// Get CPU Handle to first descriptor in descheap
	CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(descriptorHeap->GetCPUDescriptorHandleForHeapStart());

	for (int i = 0; i < g_NumFrames; i++)
	{
		// Get ith backbuffer in swap chain
		ComPtr<ID3D12Resource> backBuffer;
		ThrowIfFailed(swapChain->GetBuffer(i, IID_PPV_ARGS(&backBuffer)));

		// Create RTV for that backbuffer, with default desc (specified with nullptr), 
		// placed at the current location pointed to in the descHeap by rtvHandle
		device->CreateRenderTargetView(backBuffer.Get(), nullptr, rtvHandle);

		// Store a pointer to the buffer so that the resource can be transitioned to
		// the proper state later on
		g_BackBuffers[i] = backBuffer;

		// Increment handle to point to next handle in descheap
		rtvHandle.Offset(rtvDescriptorSize);
	}
}

// Creates Command Allocator, which is the backing memory used by a command list.
ComPtr<ID3D12CommandAllocator> CreateCommandAllocator(ComPtr<ID3D12Device2> device,
	D3D12_COMMAND_LIST_TYPE type)
{
	ComPtr<ID3D12CommandAllocator> commandAllocator;
	ThrowIfFailed(device->CreateCommandAllocator(type, IID_PPV_ARGS(&commandAllocator)));

	return commandAllocator;
}

// Creates a Command List. Used for recording commands to be executed on GPU (always deferred).
// i.e. commands in the list will not be executed until the lsit is sent to the command queue.
// Unlike Allocators, CmdLists can be reused before commands are finished executing on the GPU,
// as long as the list is reset before adding any new commands
ComPtr<ID3D12GraphicsCommandList> CreateCommandList(ComPtr<ID3D12Device2> device,
	ComPtr<ID3D12CommandAllocator> commandAllocator, D3D12_COMMAND_LIST_TYPE type)
{
	ComPtr<ID3D12GraphicsCommandList> commandList;
	ThrowIfFailed(device->CreateCommandList(0, type, commandAllocator.Get(), nullptr, IID_PPV_ARGS(&commandList)));

	// Command Lists are initialised in the recording state. For consistency, the first thing done
	// with the command list in the render loop is to reset them, and command lists must be closed 
	// in order to be reset, so reset after creation.
	ThrowIfFailed(commandList->Close());

	return commandList;
}

// Create fence with initial value 0
ComPtr<ID3D12Fence> CreateFence(ComPtr<ID3D12Device2> device)
{
	ComPtr<ID3D12Fence> fence;

	// Flags: https://docs.microsoft.com/en-us/windows/win32/api/d3d12/ne-d3d12-d3d12_fence_flags
	ThrowIfFailed(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));

	return fence;
}

// This returns an OS Event handle needed to block the CPU while waiting for a fence to be signalled
HANDLE CreateEventHandle()
{
	HANDLE fenceEvent;

	// Params:
	// -lpEventAttributes: a pointer to a SECURITY_ATTRIBUTES structure. If NULL, handle may not be inherited by child processes
	//	Docs: https://docs.microsoft.com/en-us/previous-versions/windows/desktop/legacy/aa379560(v=vs.85)
	// -bManualReset: if TRUE, event created must be manually set event state to nonsignaled via ResetEvent
	// -bInitialState: TRUE -> Signaled, else Non-Signaled
	// -lpName: Name of the event object
	fenceEvent = ::CreateEvent(NULL, FALSE, FALSE, NULL);
	assert(fenceEvent && "Failed to create fence event.");

	return fenceEvent;
}

// Used to signal the fence from the GPU by adding a signal event to the provided command queue.
// Note that the Signal happens only once it is reached in the CommandQueue, not immediately.
// Returns the value the CPU Thread should wait for before using any resources that are "in-flight" for that frame on the GPU
uint64_t Signal(ComPtr<ID3D12CommandQueue> commandQueue, ComPtr<ID3D12Fence> fence,
	uint64_t& fenceValue)
{
	uint64_t fenceValueForSignal = ++fenceValue;
	ThrowIfFailed(commandQueue->Signal(fence.Get(), fenceValueForSignal));

	return fenceValueForSignal;
}

// Used to stall the CPU thread until the fence is signalled with the specified value or greater.
// Will wait until the duration is reached (default ~584 million years)
void WaitForFenceValue(ComPtr<ID3D12Fence> fence, uint64_t fenceValue, HANDLE fenceEvent,
	std::chrono::milliseconds duration = std::chrono::milliseconds::max())
{
	// Query current fence value, only wait if our value is gt fence value
	if (fence->GetCompletedValue() < fenceValue)
	{
		ThrowIfFailed(fence->SetEventOnCompletion(fenceValue, fenceEvent));
		::WaitForSingleObject(fenceEvent, static_cast<DWORD>(duration.count()));
	}
}

// Flush ensures that any previously exxecuted commands on the GPU have finished executing
// before the CPU Thread is allowed to continue processing. Is simply a Signal followed by a WaitForFenceValue
// Useful to ensure that commands are finished executing before releasing resources used by them
void Flush(ComPtr<ID3D12CommandQueue> commandQueue, ComPtr<ID3D12Fence> fence,
	uint64_t& fenceValue, HANDLE fenceEvent)
{
	uint64_t fenceValueForSignal = Signal(commandQueue, fence, fenceValue);
	WaitForFenceValue(fence, fenceValueForSignal, fenceEvent);
}