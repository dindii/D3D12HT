//Our Direct3D/rendering stuff
#include <d3d12.h>

//Infrastructure to setup stuff for d3d, like detecting user's GPUs,
//showing stuff in the screen (it does requires some interaction with the OS),
//handling full screen transitions. Details that are not necessarily rendering
#include <dxgi1_6.h>

//Needed for Microsoft::WRL::ComPtr<> that is basically a smart pointer for d3d objects.
//They ask a lot for a pointer to a pointer (just an address of a raw * pointer is ok)
//but we can just pass the smart pointer and pretend that it is a normal pointer or even an object.
//There is some pitfalls when using it (e.g: using & in a ComPtr will release it...), 
//so I strongly recommend you to have a read about COM Pointers.
#include <wrl.h>

//Usually we compile our shaders in compile time, doing all combinations beforehand
//but for now we will compile it in runtime like opengl just for sake of simplicity
//#NOTE we have to link against d3dcompiler.lib and copy the .dll to the same folder.
#include <d3dcompiler.h>

//Include helper structs for Direct3D 12. Like when you want to transition a resource barrier and you have to fill up a struct with a lot of info.
//This header will include a struct that you will only need to say what was the state before and the state after. Quite useful.
//You are not missing anything important with this (knowledge-wise), we just avoid some boilerplate code.
//If you are having problems to build this, probably you are on VS2017 and thus with an old Windows 10 SDK version.
//#NOTE Find why in the hell VS are not finding this file even with include paths set. I hate having to hard code this.
#include <d3dx12.h>

//Lets import a basic assert.
#include <util/simpleAssert.h>

//We add this to check if our HRESULTs are fine or not.
#include <util/d3dFailureCheck.h>

//Kinda boring to write Microsoft::WRL::ComPtr<> every time.
using namespace Microsoft::WRL;

//To ease the number of header files included by windows
#define WIN32_LEAN_AND_MEAN

#include <Windows.h>

//For general utilities, like the "max" function (better than include the whole <algorithm>) (=
#include <util/utils.h>

//This is the number of backbuffers we have. This is, how many targets we are rendering while a target is being shown
//While the program is presenting a frame to the screen, we are drawing another one under the hood.
//i.e 2 = double buffering, 3 = triple buffering etc...
const uint8_t g_NumFrames = 3;

//Our initial window dimensions, we will be updating this once we take the screen size.
uint32_t g_WindowWidth  = 1280;
uint32_t g_WindowHeight = 720;

//Set to true once the DX12 objects have been initialized.
bool g_IsInitialized = false;

//Our Windows window handle, this window will be used to display our rendered image.
HWND g_hWnd; 

//Window rectangle. Is contains coordinates of all points of a rectangle, we will be using this to toggle full screen.
//When switching to full screen, we will use this variable to store the old size of our window so we know where to get back
//when switching back to window.
RECT g_WindowRect;

//-------------- DirectX 12 Objects

//The device is the virtual handle of the DirectX in the GPU. We will create everything DX12 related from a Device.
ID3D12Device2* g_Device = nullptr;

//A command allocator contains all of our commands. We will use a command list to record commands in this allocator
//then, we will send this allocator to the command queue so all the commands inside it will be executed.
//We have an array of allocators because each allocator have commands to draw a frame. So, if we have 3 frames,
//then we will have 3 allocators. When the Command Queue are executing one allocator (drawing and then showing a screen),
//we are recording on another.
ID3D12CommandAllocator* g_CommandAllocators[g_NumFrames] = {};

//The command list will record all of our commands (inside command allocators)
ID3D12GraphicsCommandList* g_CommandList = nullptr;

//A command queue will execute all of our commands inside command allocators (a Draw is a command, for example)
//It can execute other commands as well, not necessarily inside a command allocator.
ID3D12CommandQueue* g_CommandQueue = nullptr;

//This structure will be responsible to handle all "show to screen" part for us. You can notice that is not a D3D12 object
//but a IDXGI, this is because the swap chain will show our image (D3D) to our Window (OS), so it will make this bridge for us,
//this being part of the infrastructure.
IDXGISwapChain4* g_SwapChain = nullptr;

//Almost everything in DirectX is a resource. In this case, the textures (our render targets) will be a texture.
ID3D12Resource* g_BackBuffers[g_NumFrames] = {};

//A descriptor heap is a place where we store descriptors. Whenever we have a resource, we have a struct that describes it
//Like, what is the format of the texture? How many channels? How larger is it? Where is it in memory? 
//We will store all of this inside a descriptor.
ID3D12DescriptorHeap* g_RTVDescriptorHeap = nullptr;

//Descriptors can have different size based on its type and vendor (amd, nvidia etc...). We are querying the size of a RTV descriptor so we can create a
//descriptor heap that knows the size of each of its slots. (And this one will only store RTV's or other descriptors with
//the same size)
uint32_t g_RTVDescriptorSize = 0;

//We need to take count in which backbuffer we are drawing/showing. After sending the backbuffer 0 to be executed and shown
//we will increment this, and in the next iteration, we will be drawing/recording commands in the backbuffer 1.
//Not always the back buffers will be sequential (depending on the flip model of the swap chain) so the swap chain will return to us the next index to use.
uint32_t g_CurrentBackBufferIndex = 0;
// --------------

// -------------- DX12 Synchronization Objects

//Let's say you are writing to a texture in the CPU so the GPU can use this texture.
//We have right now 3 command allocators, those command allocators will have read or even draw commands to this texture.
//When a command queue are executing commands inside a command allocators, one of those command can be a read/draw command to this texture.
//If the command queue are reading a texture and we write this same texture in the CPU side, we will have a problem because the GPU
//didn't finish to use this resource. So, when the GPU is running and using a resource, we must wait on CPU before we can modify/delete it.
//We don't need to worry for now about other command allocators using the same resource because they are sequential and all of them represents the GPU
//But when we have, for example, some Compute commands to write a texture for us, we can also synchronize between queues in the GPU side.
ID3D12Fence* g_Fence = nullptr;

//When we are in the beginning of the main loop (usually in the render part) we increment the g_FenceValue (in the CPU/C++ side)
//and issue a command to the GPU to update its internal fence value to our actual fence value (g_FenceValue). This is done through a command.
//Since we have a lot of commands to execute (like clear, draw, write etc), our command to update the GPU internal value will be placed at last in the queue.
//And then we check if the internal GPU fence value is equal our actual g_FenceValue, if so, we are up to date with the CPU, and if not, the
//CPU have to wait until all commands of the GPU are executed. This way we can ensure all commands are executed, because the update fence
//command is the last one, so we know that we executed everything.
uint64_t     g_FenceValue = 0;

//Each frame will have its own fence, this is, each allocator will have its own value to be updated at the last place (fence value)
//to be compared with the CPU value.
uint64_t     g_FrameFenceValues[g_NumFrames] = {};

//This will be an OS event, Windows will tell us that our GPU fence value has reached our CPU fence value, through this event.
HANDLE       g_FenceEvent;
// --------------

//If we are going to use VSync.
bool g_VSync = true;


//Sometimes we want to use a custom vsync technology, we can let the tearing occur so the application can decide when the vertical refresh should be done
bool g_TearingSupported = false;

//#TODO: DEFINE THIS BELOW!!! (with proper names etc)
//This function will handle OS events/messages 
LRESULT WndProc(HWND a, UINT b, WPARAM c, LPARAM d) { return DefWindowProc(a, b, c, d); };

int main()
{
	//Before creating everything, we must create our debug layer. This is a helper feature of DX12, the API will try to give us hints in wrong stuff we did. 
	//We have to create it before our ID3D12Device or it will not create the device with the right properties and it will remove the device on runtime.
	//Also, before doing anything related to DX12, it is recommended to initialize the debug layer. So we can have
	//messages in case anything went wrong. This includes the creation of the device, so we can have more info in case of failure.

#ifdef _DEBUG
	ID3D12Debug* debugInterface;

	//Get the Debug Interface and enable the Debug Layer.
	//IID_PPV_ARGS is just a macro that looks the type of the variable we are sending in order to compute its IID (like an UIID)
	//once the IID is computed and passed, it retrieves the interface pointer and assign our variable to it, in this case, it makes debugInterface
	//to point to the internal debug interface.
	//Everytime we have something that requires a separate IID and a interface pointer, we must use this macro. A lot of confusion can occur when 
	//trying to do this by hand. This macro ensures that we are being persistent on the type of the variable, pointer and interface.
	D3D12GetDebugInterface(IID_PPV_ARGS(&debugInterface));
	debugInterface->EnableDebugLayer();
#endif

	// -------------- Windows Window Creation 
	//Before creating our Window instance, we must fill a layout (class) that we want our Window to have. Some sort of properties.
	//A lot of features we will not be using, since we will render in the whole screen ourselves. Like Menu feature, background color and its brushes etc...
	
	WNDCLASSEX windowClass = {};

	HINSTANCE hInstance = GetModuleHandle(nullptr);

	windowClass.cbSize        = sizeof(WNDCLASSEX);             // The size in bytes of this structure.
	windowClass.style         = CS_HREDRAW | CS_VREDRAW;		// Class style. CS_HREDRAW means that we will redraw all the window if we change the window width (and CS_VREDRAW for height)
	windowClass.lpfnWndProc   = &WndProc;						// A pointer to the function that will handle the events of this window. We forward decleared it above.
	windowClass.cbClsExtra    = 0;								// Number of extra bytes to allocate for this class structure, we will not use this.
	windowClass.cbWndExtra    = 0;								// Number of extra bytes to allocate for this window instance, we will not use this.
	windowClass.hInstance     = hInstance;						// A handle to the instance that contains the window procedure for the class. We also use the hInstance to identify in case more than one .dll uses the same class name. 
																// A very simply but informative resource on that (hInstance): https://devblogs.microsoft.com/oldnewthing/20050418-59/?p=35873
	windowClass.hIcon         = LoadIcon(hInstance, NULL);		// The icon of the window to be loaded, in the top left corner or in the taskbar
	windowClass.hCursor       = LoadCursor(NULL, IDC_ARROW);	// The cursor inside the window, we will be using the default
	windowClass.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);		// The color of the background or the handle to the brush used to paint the background. We will not use this as we will be doing the paint process ourselves.
	windowClass.lpszMenuName  = NULL;							// Resource name of the window menu class. We will use the default.
	windowClass.lpszClassName = "D3D12 Hello Triangle Window";	// The name of the window class, this is important, we will use this class name to create the window. This is basically the name of our layout/style/class.
	windowClass.hIconSm       = LoadIcon(hInstance, NULL);		// A handle to a small icon that this class will be using. If NULL, it will try to search the icon resource specified by the hIcon for an icon of the appropriate size to use as the small icon

	//Let's try to register our window layout.
	ATOM registerResult = RegisterClassEx(&windowClass);
	
	D3D_ASSERT(registerResult > 0, "failed to register Window class.");
	
	//We use the GetSystemMetrics function to retrieve a specific system information. In this case, SM_CXSCREEN and SM_CYSCREEN are used
	//to retrieve the width and height of the primary display monitor screen in pixels. So we update our screen width and height. It will take the whole screen but it will not be fullscreen.
	g_WindowWidth = GetSystemMetrics(SM_CXSCREEN);
	g_WindowHeight = GetSystemMetrics(SM_CYSCREEN);

	//#NOTE: Usually we do some calculations to ensure that the window will always be inside screen bounds and at least at the middle (when not occupying the whole screen)
	//I will not bother to do this here because our main topic is to learn DX12 and this window is good enough for everything we want.
	//It will be a window with almost the size of the main display screen and it will still have the control bars above it.

	//The WS_OVERLAPPEDWINDOW basically defines a window with a thick frame. (WS_EX_OVERLAPPEDWINDOW combines WS_EX_WINDOWEDGE with WS_EX_CLIENTEDGE, all of this is about the window frame border)
	//We pass the name of our created style/layout (it identifies using the name and not an ID or so)
	//Then we define our window Style, in the first parameter, we set things about the window frame border. Now, we define that we want a window with that bar at the top with minimize, maximize and close functions.
	//Styles are pretty trivial, you can have a easy read here https://docs.microsoft.com/en-us/previous-versions/ms960010(v=msdn.10), see what each one does and even combine them.
	//We use CW_USEDEFAULT so the OS can decide where the upper-left corner of the screen will be placed.
	//Then we pass the width, height, the parent window (NULL), the menu class (NULL), our hInstance and we pass nothing (last NULL) as being custom data.
	g_hWnd = CreateWindowEx(WS_EX_OVERLAPPEDWINDOW, windowClass.lpszClassName, "Hello Triangle!", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, g_WindowWidth / 2, g_WindowHeight / 2, NULL, NULL, hInstance, nullptr);
	
	D3D_ASSERT(g_hWnd, "Failed to create window!");

	//Now that we have a created window, we can continue to create our D3D12 pipeline. Further on, we will show the window.
	//It was a short introduction since window creation it is not our focus here but you should find plenty of information on Windows window creation. 
	// --------------
	

	//Let's begin to create our D3D12 components!

	//Firstly, we have to look for the best GPU in our system that supports D3D12. After finding this GPU, we will create the D3D12 handle for this GPU, this is, we will create the "handle of D3D12 of this GPU".
	//And use this handle to access of all features of D3D12 that will run on this GPU.

	//Let's get the best GPU that supports D3D12:

	//Before querying for available adapters (GPUs), we must create a DXGI Factory, this will let us to create other important DXGI objects.
	//As said before, the DXGI is for stuff that is not related to the graphics API itself but for infrastructure. 
	//Looking for and retrieving handles to availables GPUs and its stats (GPU memory, clock, supported API versions etc...) is something related to infrastructure. 
	IDXGIFactory4* dxgiFactory = nullptr;
	uint32_t createFactoryFlags = 0;
	
	//When enabling this debug flag, we are able to get errors when the factory fails to do an action (like creating a device or querying for adapters)
#ifdef _DEBUG
	createFactoryFlags = DXGI_CREATE_FACTORY_DEBUG;
#endif

	//Let's actually create our factory and check if everything went fine.
	Check(CreateDXGIFactory2(createFactoryFlags, IID_PPV_ARGS(&dxgiFactory)), "Failed to create DXGIFactory!");

	//Now we will use this factory to query for a good GPU candidate.

	//Create a pointer to an adapter and let's fill this pointer
	IDXGIAdapter1* adapter1 = nullptr;

	//Adapter4 is an Adapter1 but with more features on it. Each AdapterN inherits from AdapterN-1 thus getting its features and adding more.
	//EnumAdapters requires an Adapter1, so we will pass an Adapter1 and then cast this for an Adapter4, so we can use all features of Adapter4.
	IDXGIAdapter4* adapter4 = nullptr;

	//Usually, a safe parameter of a video card being better than other, is the available memory. 
	//With this variable, we will try to get the GPU with the biggest dedicated video memory.
	uint32_t maxDedicatedVideoMemory = 0;

	//EnumAdapters will retrieve an Adapter in the provided index. This is, if we have 4 adapters, we can get the first GPU by calling EnumAdapers with 0 as index and so on.
	//We will iterate the GPU list in order to get the best GPU. Eventually, when we try to get a GPU that doesn't exist (e.g: index 4 in the list of 4 GPUs range[0,3]) it will return DXGI_ERROR_NOT_FOUND for us.
	for (uint32_t i = 0; dxgiFactory->EnumAdapters1(i, &adapter1) != DXGI_ERROR_NOT_FOUND; i++)
	{
		//Let's query this adapter for a descriptor. A descriptor... describes the adapter. We can get important information through it.
		DXGI_ADAPTER_DESC1 adapterDesc1; 
		adapter1->GetDesc1(&adapterDesc1);
		
		//Then, we check if this adapter is not a software adapter (this is, not an onboard GPUs) and if this adapter has higher memory quantity than the actual one
		//this way, we will end up with the bigger memory GPU.
		if ((adapterDesc1.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0 && adapterDesc1.DedicatedVideoMemory > maxDedicatedVideoMemory)
		{
			//With a good GPU candidate, we check if this GPU supports DX12. 
			//For this, we will simulate a Device creation on this GPU.
			//As said before, the device is the handle of the DirectX in the specified GPU.
			//The device creation, asks for a pointer to an ID3D12Device so it can fill this pointer with the device object, but
			//since we are simulating it, we can just pass nullptr as the argument.
			HRESULT testCreation = D3D12CreateDevice(adapter1, D3D_FEATURE_LEVEL_12_0, __uuidof(ID3D12Device), nullptr);

			//Why we are checking if this is S_FALSE if we want to know if our device creation was succeed?
			//Well, in MSDN documentation, it is specified that, when we are passing nullptr for the device pointer AND the creation is succeeded, then it returns S_FALSE.
			//So, D3D12CreateDevice knows when we are just testing if the adapter supports D3D12 and return  S_FALSE (1) to us when the adapter supports it.
			if (testCreation == S_FALSE)
			{
				//If so, we just set it as our new best GPU and cast it to the equivalent Adapter4.
				maxDedicatedVideoMemory = (uint32_t)adapterDesc1.DedicatedVideoMemory;
				Check(adapter1->QueryInterface<IDXGIAdapter4>(&adapter4));
			}
		}
	}

	//Let's go to our actual device creation (our DX12 Handle to this GPU, so we can use all features of this GPU using DX12)
	//Mainly, our device will be used to create DX12 objects for our GPU. It will not be directly used to issue draw or dispatch commands
	//but it will be used to create the command queue and the command list, that will be responsible for those commands.
	//The device can be considered a memory context that tracks allocations in GPU memory. If you destroy the context, then, everything allocated by it
	//will be destroyed as well.

	//Create the device and check if it succeeds
	Check(D3D12CreateDevice(adapter4, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&g_Device)));

#ifdef _DEBUG
	ID3D12InfoQueue* pInfoQueue = nullptr;

	g_Device->QueryInterface<ID3D12InfoQueue>(&pInfoQueue);
	
	if (pInfoQueue)
	{
		pInfoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, TRUE);
		pInfoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, TRUE);
		pInfoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_WARNING, TRUE);
	}
#endif

	//Now, we will create our command queue. 
	D3D12_COMMAND_QUEUE_DESC commandQueueDescription = {};

	//By setting the type of the command queue, we are saying for what this command queue will be used for.
	//DIRECT type is basically everything. We can use it for draw, copy and compute commands. 
	//We have other types of queues, like queues that are only made for compute commands (for compute shading) or copy commands.
	//Sometimes it is useful to have separate queues for those operations and then sync them up at the end.
	commandQueueDescription.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;

	//Priority work just as fine on normal. To have a global realtime priority, the application would need those rights as well as support from the
	//hardware.
	commandQueueDescription.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;

	//We don't really have to set any flags for this. Also, we don't need any useful flags for now.
	commandQueueDescription.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;

	//NodeMask is a concept of DX12 to identify more than one GPU. If you are using only one GPU, then you can use NodeMask == 0.
	commandQueueDescription.NodeMask = 0;

	//Create our command queue.
	Check(g_Device->CreateCommandQueue(&commandQueueDescription, IID_PPV_ARGS(&g_CommandQueue)));



	//Before creating our swap chain, let's support variable refresh rate displays (Nvidia G-Sync and AMD FreeSync)
	//We will query if the display supports it and make some changes on the swap chain to match this support.
	//To make this, we must allow tearing to be done, this way, the "v-sync" will be done by the display itself
	
	//We then, query for the IDXGIFactory5 interface in order to be able to use CheckFeatureSupport()
	IDXGIFactory5* dxgiFactory5 = nullptr;
	Check(dxgiFactory->QueryInterface<IDXGIFactory5>(&dxgiFactory5));
	
	BOOL tearingSupported = FALSE;
	dxgiFactory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearingSupported, sizeof(tearingSupported));

	g_TearingSupported = (bool)tearingSupported;

	//Now that we know if we support tearing, let's create our swap chain.

	//Usually, we draw our scene into a texture, a simply image. But, if we want to present this image to the screen, then, we have to
	//somehow communicate with the OS to show our image in one of its windows.
	//The job of the swap chain is exactly to present our images to the screen. 
	//The swap chain is fully optmized to do this. When creating it, we can set several options that better match to our application style.

	//When rendering images with the swapchain, usually we have a back-buffer and a front-buffer. While we are presenting an image (called as front-buffer), we are drawing another one in the background (called back-buffer).
	//When the back-buffer image is finally done, then, we just need to swap both.
	//So now, the front-image is the one we just draw, and the back-buffer is the previous presented image (that we are probably erasing it all and drawing new stuff on it)

	DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
	swapChainDesc.Width  = g_WindowWidth;				         //The width  of the images we are going to write-to/present
	swapChainDesc.Height = g_WindowHeight;				         // ^  height ^   ^    ^    ^   ^    ^    ^   ^          ^
	swapChainDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;           //The structure that describes the display format. For this, each R, G, B and A will have 8 bits. (0-255)
	swapChainDesc.Stereo = FALSE;                                //We set this to true if we are using 3D glasses... I guess we are not...
	swapChainDesc.SampleDesc = { 1, 0 };				         //The quality of the anti-aliasing. Since we are using the swap FLIP model, this must be {1, 0}. 
	swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; //With this, we tells DXGI for what we are using this swap chain. Since we are using it to present images to the screen, the usage is indeed DXGI_USAGE_RENDER_TARGET_OUTPUT
	swapChainDesc.BufferCount = 2;                               //Specify how many buffers to create. We will set this to 2, as we will be using one for front and another for back buffer (double buffering).
	swapChainDesc.Scaling = DXGI_SCALING_STRETCH;                //If the image is smaller than the screen, then, this option will specify to DXGI to strech the image to cover the whole screen. This is usually necessary if you choose a custom resolution
	swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;    //This specifies what DXGI should do with the buffers once they have been shown and are no longer of use. FLIP Discard tells it that we are erasing our buffer in order to draw again on it. 
																 //You also could specify to maintain the buffers content (this would be good if we want to edit an image or just to add more stuff on top of it)
	swapChainDesc.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;       //Indicates how we are going to handle transparency for the buffers. For now, we will not be using this.
	
	swapChainDesc.Flags = g_TearingSupported ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0; //Tells to the swap chain if we are allowing tearing in order to use variable refresh rate.


	//Let's instantiate our swap chain object
	IDXGISwapChain1* swapChain1 = nullptr;
	Check(dxgiFactory->CreateSwapChainForHwnd(g_CommandQueue, g_hWnd, &swapChainDesc, nullptr, nullptr, &swapChain1));
	
	Check(dxgiFactory->MakeWindowAssociation(g_hWnd, DXGI_MWA_NO_ALT_ENTER));

	Check(swapChain1->QueryInterface<IDXGISwapChain4>(&g_SwapChain));



	return 0;
}