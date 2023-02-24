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

//to use timers and get the actual time
#include <chrono>

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
	ID3D12Debug* debugInterface = nullptr;

	//Get the Debug Interface and enable the Debug Layer.
	//IID_PPV_ARGS is just a macro that looks the type of the variable we are sending in order to compute its IID (like an UIID)
	//once the IID is computed and passed, it retrieves the interface pointer and assign our variable to it, in this case, it makes debugInterface
	//to point to the internal debug interface.
	//Every time we have something that requires a separate IID and a interface pointer, we must use this macro. A lot of confusion can occur when 
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

	//Show the window just for testing purposes
	ShowWindow(g_hWnd, SW_SHOW);

	//Now that we have a created window, we can continue to create our D3D12 pipeline. Further on, we will show the window.
	//It was a short introduction since window creation it is not our focus here but you should find plenty of information on Windows window creation. 
	// --------------
	

	//Let's begin to create our D3D12 components!

	//Firstly, we have to look for the best GPU in our system that supports D3D12. After finding this GPU, we will create the D3D12 handle for this GPU, this is, we will create the "handle of D3D12 of this GPU".
	//And use this handle to access of all features of D3D12 that will run on this GPU.

	//Let's get the best GPU that supports D3D12:

	//Before querying for available adapters (GPUs), we must create a DXGI Factory, this will let us to create other important DXGI objects.
	//As said before, the DXGI is for stuff that is not related to the graphics API itself but for infrastructure. 
	//Looking for and retrieving handles to available GPUs and its stats (GPU memory, clock, supported API versions etc...) is something related to infrastructure. 
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
		
		//Then, we check if this adapter is not a software adapter (this is, not an on board GPUs) and if this adapter has higher memory quantity than the actual one
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
	//The swap chain is fully optimized to do this. When creating it, we can set several options that better match to our application style.

	//When rendering images with the swap chain, usually we have a back-buffer and a front-buffer. While we are presenting an image (called as front-buffer), we are drawing another one in the background (called back-buffer).
	//When the back-buffer image is finally done, then, we just need to swap both.
	//So now, the front-image is the one we just draw, and the back-buffer is the previous presented image (that we are probably erasing it all and drawing new stuff on it)

	DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
	swapChainDesc.Width  = g_WindowWidth;				         //The width  of the images we are going to write-to/present
	swapChainDesc.Height = g_WindowHeight;				         // ^  height ^   ^    ^    ^   ^    ^    ^   ^          ^
	swapChainDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;           //The structure that describes the display format. For this, each R, G, B and A will have 8 bits. (0-255)
	swapChainDesc.Stereo = FALSE;                                //We set this to true if we are using 3D glasses... I guess we are not...
	swapChainDesc.SampleDesc = { 1, 0 };				         //The quality of the anti-aliasing. Since we are using the swap FLIP model, this must be {1, 0}. 
	swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; //With this, we tells DXGI for what we are using this swap chain. Since we are using it to present images to the screen, the usage is indeed DXGI_USAGE_RENDER_TARGET_OUTPUT
	swapChainDesc.BufferCount = g_NumFrames;                               //Specify how many buffers to create. We will set this to 2, as we will be using one for front and another for back buffer (double buffering).
	swapChainDesc.Scaling = DXGI_SCALING_STRETCH;                //If the image is smaller than the screen, then, this option will specify to DXGI to stretch the image to cover the whole screen. This is usually necessary if you choose a custom resolution
	swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;    //This specifies what DXGI should do with the buffers once they have been shown and as no longer of use. FLIP Discard tells it that we are erasing our buffer in order to draw again on it. 
																 //You also could specify to maintain the buffers content (this would be good if we want to edit an image or just to add more stuff on top of it)
	swapChainDesc.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;       //Indicates how we are going to handle transparency for the buffers. For now, we will not be using this.
	
	swapChainDesc.Flags = g_TearingSupported ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0; //Tells to the swap chain if we are allowing tearing in order to use variable refresh rate.


	//Let's instantiate our swap chain object
	IDXGISwapChain1* swapChain1 = nullptr;
	Check(dxgiFactory->CreateSwapChainForHwnd(g_CommandQueue, g_hWnd, &swapChainDesc, nullptr, nullptr, &swapChain1));
	/* CreateSwapChainForHwnd arguments:
	*  1 - A command queue of who we are creating this swap chain for
	*  2 - The handle of the window that we are going to present to 
	*  3 - The swap chain description
	*  4 - The swap chain full screen description (set to null to create a windowed swap chain)
 	*  5 - To restrict the content to a specific window. An example of output is a monitor  
	*  6 - An object to store the reference of the swap chain
	*/

	//We will handle the full screen switch manually. So we are disabling the ALT + ENTER command.
	Check(dxgiFactory->MakeWindowAssociation(g_hWnd, DXGI_MWA_NO_ALT_ENTER));

	//Let's cast our swap chain to a IDXGISwapChain4 and we are done!
	Check(swapChain1->QueryInterface<IDXGISwapChain4>(&g_SwapChain));


	//Now that we have our swap chain, we need to create the descriptors for the swap chain back buffers
	//the descriptor basically describes a resource, this way the GPU knows how to process that resource.
	//In our case, we will describe that our resource is a output target, its format and stuff like this
	
	//We need to store our descriptors somewhere. For this we have the Descriptor Heap.
	//We will then create a descriptor heap and store our descriptors inside it.
	//We have several types of views (or resources), they are:
	// Render Target View (RTV), Shader Resource View (SRV), Unordered Access View (UAV)
	// Constant Buffer View (CBV) and a Depth Stencil View (DSV)
	//The CBV, SRV and UAV have the same size, so they can be stored in the same heap.
	//But for RTV and Samplers, we have to create another heap for them.

	//A resource is just a block of memory, a block of bytes, and a view tells us how we can interpret all of those data.
	//If we have a resource that is a texture, we don't know if the texture is RGBA, or RGB, or even a R only texture.
	//If we have a RGB data but the view thinks that our data is 1 channel only (R), it would interpret a RGB data as 3 different colors 
	//and this is totally wrong. 

	//Let's create our descriptor heap:

	D3D12_DESCRIPTOR_HEAP_DESC descriptorHeapDesc = {};
	descriptorHeapDesc.NumDescriptors = g_NumFrames;            //The number of descriptors in the heap
	descriptorHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;   //The type of views that we are going to store
	descriptorHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE; // The only other option is the D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE flag
	//When we are creating descriptors, the application can decide if it want to store the descriptor in the CPU before copying it to the GPU (to be shade accessible)
	//with this flag, we say to the application to create descriptors directly to the shader visible descriptor heaps without staging anything on the CPU.
	//This flag only works with CBV, SRV and UAV
	descriptorHeapDesc.NodeMask = 0; //We can create a heap for a specific GPU. Since we are using only one GPU, let's keep this on zero.

	//Create our heap
	Check(g_Device->CreateDescriptorHeap(&descriptorHeapDesc, IID_PPV_ARGS(&g_RTVDescriptorHeap)));
	
	//Now we can proceed to create our views (descriptors).
	//Let's create our Render Target View (a resource where we are gong to render out screen to)

	//Get the size of a RTV on this device. (size is vendor specific)
	g_RTVDescriptorSize = g_Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

	//We get the first handle of the heap, and we will use it to iterate our heap
	//It is the same idea as taking the first element pointer of an array and adding + 1 to it
	//Now, we have a pointer (inside this structure) to a descriptor inside the descriptor heap
	CD3DX12_CPU_DESCRIPTOR_HANDLE firstRTVHandleIndex(g_RTVDescriptorHeap->GetCPUDescriptorHandleForHeapStart());

	//One descriptor for each render target buffer 
	for (uint32_t i = 0; i < g_NumFrames; i++)
	{
		//Now, we get all the resources (all the backbuffers/render targets) that was created inside our swap chain
		ID3D12Resource* renderTarget = nullptr;
		Check(g_SwapChain->GetBuffer(i, IID_PPV_ARGS((&renderTarget))));

		//Now we just need to create the render target view for the swap chain backbuffer resource.
		//The first parameter is the resource that we are creating the descriptor to
		//the second one is the description of the resource. Setting it to nullptr will make it to create a default descriptor for the resource
		//In this case, the resource's internal description is used to create the RTV (when you create a resource, it asks for a bunch of details, it will use those details).
		//The third parameter is only where we will store the descriptor. We will store it in this specific handle of the heap.
		g_Device->CreateRenderTargetView(renderTarget, nullptr, firstRTVHandleIndex);

		//Now that our render target resources are complete, we can save them for later use.
		g_BackBuffers[i] = renderTarget;

		//Let's advance the handles to the available index. So we get a new handle next time. (basically ptr + g_RTVDescriptorSize)
		firstRTVHandleIndex.Offset(g_RTVDescriptorSize);
	}
	
	//Create a Command Allocator
	//I already explained what a command allocator is, but as an additional detail, when the GPU finishes to consume all commands inside it
	//we can reclaim or memory back by calling CommandAllocator->Reset(). We can only call Reset if the GPU finished to use all command allocator commands
	//We will know that the GPU is done, through a fence.
	//D3D12_COMMAND_LIST_TYPE_DIRECT defines that this command allocator will have regular commands that the GPU can execute.
	//beside the type DIRECT we also have the type COMPUTE (for compute dispatches), BUNDLE and COPY.
	for (uint32_t i = 0; i < g_NumFrames; i++)
		Check(g_Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_CommandAllocators[i])));

	//Now, let's create our command list.
	Check(g_Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_CommandAllocators[0], nullptr, IID_PPV_ARGS(&g_CommandList)));

	//A command list is created in Recording state. The very first thing you want to do in the render loop is to Reset the command list.
	//Because of the command list is created in recording state, we need to specify a command allocator that this command list will record to
	//when creating the command list (CreateCommandList)
	//We also will need to do this with the Reset() function, because the Reset() will set the command list to the recording state.
	//Before reseting a command list, we must close it (the very last thing). So, let's change its state to Closed so it can be reset in the first iteration of the loop.
	Check(g_CommandList->Close());

	//In order to know when the GPU finished to execute all commands of a command allocator, we must setup a fence
	//so the GPU can signal this fence for us.
	//Let's create our fences!

	//We should create the fence with 0 as being the value.
	Check(g_Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_Fence)));

	//When the Fence reaches a specified value, it will trigger an event. As being the CPU, we can wait for this event to be triggered, thus knowing that the GPU finished all the job
	//for this, let's create the event
	//We also can wait as being the GPU, we can use the function CommandQueue::Wait(). But since usually the CPU is the bottleneck, let's begin with it
	g_FenceEvent = ::CreateEvent(NULL, FALSE, FALSE, NULL);
	/* CreateEvent parameters:
	* 1 - SECURITY_ATTRIBUTES, if null, the handle cannot be inherited by child processes.
	* 2 - If TRUE, this event has to be reset manually. By false, it will be reseted after the thread is released
	* 3 - If TRUE, the initial state is signaled. We want it to be non-signaled
	* 4 - A simple name to it
	*/

	D3D_ASSERT(g_FenceEvent, "Failed to create fence event!");
	
	//For the tutorial's linearity sake, I will not declare and define a function above the main() function. Instead, I will "define a function inside main" through this 
	//lambda and function pointer
	//So we can follow along all the tutorial instead of having to place a function and say "we will come later here, just ignore for now".
	//And since this is a snippet of code that we will be using frequently, it worths to create a function just for it
	auto SignalFence = [](ID3D12CommandQueue* commandQueue, ID3D12Fence* fence, uint64_t& fenceValue ) -> uint64_t 
	{
		//Get the actual fence value and increment in the CPU
		uint64_t fenceValueForSignal = ++fenceValue;

		//Ask for the GPU to update its fence with this CPU value. (this is run on the GPU, the CPU will not stop here, we will continue through our function)
		Check(commandQueue->Signal(fence, fenceValueForSignal));

		//Returns the value that we want the GPU to be on. We then will use this value to compare if the GPU has reached our fence. This is, we will stall until this fence is
		//equal fenceValueForSignal.
		return fenceValueForSignal;
	};

	auto WaitForFenceValue = [](ID3D12Fence* fence, uint64_t fenceValueToWait, HANDLE fenceEvent)
	{
		//We check if the GPU has updated our fence to the CPU fence value
		if (fence->GetCompletedValue() < fenceValueToWait)
		{
			//If not, we will ask this fence to trigger this event once it reached the desired fence (usually, the CPU fence value)
			Check(fence->SetEventOnCompletion(fenceValueToWait, fenceEvent));

			//And now, we will stall the CPU until this event is triggered (i.e: until the GPU finishes its work).
			//You can optionally set for how long you want to wait. In our case, we will wait for millions of years, or in this case, for a INFINITE time.
			::WaitForSingleObject(fenceEvent, INFINITE);
		}
	};

	//Next, a useful function that we may use, is the Flush function. This function just insert a Signal in the Queue and waits for it.
	//This is, we insert a Signal in the Queue, and when the GPU reaches that point, it will trigger an event for us, so we know that the GPU finished everything.
	//This is useful when we want to do something with the resources that are being in use by the GPU. For example, when we want to resize the screen, we need to
	//resize all the buffers of the swap chain, but the GPU could be using those buffers. So we can Flush the GPU, thus knowing when the GPU finished to use everything
	//and now that we know, we can proceed to resize our buffers since there's no buffer being referenced anymore. 
	//After flushing and doing what we want, we can proceed to the default behavior of our pipeline.
	auto FlushCommandQueue = [SignalFence, WaitForFenceValue](ID3D12CommandQueue* commandQueue, ID3D12Fence* fence, uint64_t& currentFence, HANDLE fenceEvent)
	{
		uint64_t fenceValueToWait = SignalFence(commandQueue, fence, currentFence);
		WaitForFenceValue(fence, fenceValueToWait, fenceEvent);
	};

	 //Let's implement Update and Render functions
	

	//The update function will be super simple, it will just display the FPS on the VS debug output 

	//Every time this function is called, we sum the frameCounter and the deltaTime to the elapsedSeconds
	//Eventually, this elapsedSeconds will reach 1 second. Then, we just need to divide the frameCounter to the elapsedSeconds
	//and we have all the frames we got in one second.
	auto Update = []()
	{
		static uint64_t frameCounter = 0;
		static double elapsedSeconds = 0.0f;
		static std::chrono::high_resolution_clock clock;
		static auto timeStart = clock.now();
		
		frameCounter++;
		auto timeEnd = clock.now();
		std::chrono::nanoseconds deltaTime = timeEnd - timeStart;
		timeStart = timeEnd;

		elapsedSeconds += deltaTime.count() * 1e-9;

		if (elapsedSeconds > 1.0f)
		{
			char buffer[500];
			double fps = frameCounter / elapsedSeconds;
			sprintf_s(buffer, 500, "FPS: %f\n", fps);
			OutputDebugString(buffer);

			frameCounter = 0;
			elapsedSeconds = 0.0f;
		}

	};

	//The Draw/Render function will be made of two steps:
	//Clear the back buffer
	//Present the rendered frame
	
	//For simplicity, I will define the Render function below the main function

	auto Render = [&SignalFence, &WaitForFenceValue]()
	{
		ID3D12CommandAllocator* commandAllocator = g_CommandAllocators[g_CurrentBackBufferIndex];
		ID3D12Resource* backBuffer = g_BackBuffers[g_CurrentBackBufferIndex];

		//Clear all commands (memory) so we can reuse this memory for further commands. PS: We must before assure that we have no commands to be executed
		//or else it will fail
		commandAllocator->Reset();

		//Open our commandList for recording. When the CommandList is Reset, it will be open again to command recording, so we need to specify which 
		//commandAllocator we will be using to store them
		g_CommandList->Reset(commandAllocator, nullptr);

		//== Right now, our resource that we are using as a Render Target is on Present State and in order to write to it, we must transition it to Render Target
		//We can do this using the Transition method of the CD3DX12_RESOURCE_BARRIER helper struct.
		CD3DX12_RESOURCE_BARRIER PresentToWriteBarrier = CD3DX12_RESOURCE_BARRIER::Transition(backBuffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
		
		//After we created the barrier, we will issue a command to make this transition to run. Everything will run automatically since this barrier already
		//knows what state to transition what resource
		g_CommandList->ResourceBarrier(1, &PresentToWriteBarrier);


		//Now that our back buffer is ready to write, we will write the whole resource to an specific color. This is called "Clean".
		//we will define a clean color as follows
		float clearColor[] = { 0.4f, 0.6f, 0.9f, 1.0f };

		//We then get the handle of our resource from the Descriptor Heap by passing the start of the heap (like the address of the first element of an array)
		//and then we will pass the index that we want to jump forward and the size that we will be using to jump forward (literally like a pointer) 
		CD3DX12_CPU_DESCRIPTOR_HANDLE rtv(g_RTVDescriptorHeap->GetCPUDescriptorHandleForHeapStart(), g_CurrentBackBufferIndex, g_RTVDescriptorSize);

		//Submit the write command
		g_CommandList->ClearRenderTargetView(rtv, clearColor, 0, nullptr);

		//In order to present our resource to the screen, we must transition again from the Render Target (write) to Present (read)
		CD3DX12_RESOURCE_BARRIER WriteToPresentBarrier = CD3DX12_RESOURCE_BARRIER::Transition(backBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
		g_CommandList->ResourceBarrier(1, &WriteToPresentBarrier);

		//We will not be recording commands anymore to this list, so before we can make use of it, we must close it first.
		Check(g_CommandList->Close());

		//ExecuteCommandLists of our Queue expects a const array of CommandLists, even if we only have one we must create an array of CommandLists
		ID3D12CommandList* const commandLists[] = { g_CommandList };

		//Send the CommandList to be executed by our command queue
		g_CommandQueue->ExecuteCommandLists(_countof(commandLists), commandLists);

		//Before presenting, we have to setup some properties and flags before. 
		//By setting the Sync Interval to True, we are explicit saying that we want to cap our frame using vsync
		uint32_t syncInterval = g_VSync ? 1 : 0;

		//If we are not using vsync and we do support variable refresh rates, we then will use the tearing mode
		uint32_t presentFlags = g_TearingSupported && !g_VSync ? DXGI_PRESENT_ALLOW_TEARING : 0;

		//Ask the swap chain to present it's active back buffer (the actual back buffer index)
		Check(g_SwapChain->Present(syncInterval, presentFlags));

		// We will signal our fence to our current value + 1
		g_FrameFenceValues[g_CurrentBackBufferIndex] = SignalFence(g_CommandQueue, g_Fence, g_FenceValue);
	
		//Get the next render target
		g_CurrentBackBufferIndex = g_SwapChain->GetCurrentBackBufferIndex();

		//Check if this new render target is suitable to use or if we must it to be executed first
		WaitForFenceValue(g_Fence, g_FrameFenceValues[g_CurrentBackBufferIndex], g_FenceEvent);


		/*
		* In general, the GPU is doing a lot of stuff and it will not stop the CPU.
		* That's why we execute the command list, queue the swap chain to present this frame when it is done
		* and right away we get another buffer. This buffer could be in use, so we check it's fence value, if it is free, we will use it to execute commands
		* if all buffers are in use we then stale the CPU.
		*/

		/* Let's think in a scenario using double buffering (two render targets):
		*
		* We record all commands on the buffer 1
		* We execute all commands on the buffer 1 
		* We queue a signal to the fence of the buffer 1 to know when it is done
		* We queue the buffer 1 to be shown when it is done
		* We see if the buffer 2 is available
		* We record all commands in buffer 2
		* We execute all commands in buffer 2
		* We signal the fence on buffer 2
		* We check if the buffer 1 is available
		* If not, we will wait (stale the CPU) until it is done (when the command queue reaches the Signal(buffer1)
		* If so, we will do everything again on the buffer 1 and then get the buffer 2
		* 
		* We will stale the CPU if the commands on Buffer 1 are still being executed (GPU) and we already recorded all commands to the Buffer 2 (CPU)
		* and we need a new buffer to record commands. 
		*
		* PS: Internally, the SwapChain works like a Queue. The Present method is called but the BackBuffer is not instantly presented because it is still being 
		* drawn. All the frames as presented as they are ready to go. As the SwapChain does have more context (as it is a queue), it can do a lot of different
		* stuff with the frames, like, if it is presenting a frame but another frame is already done, it can discard this frame and present the next frame.
		* We can different behaviors based on the type of the presentation model set when we create the swap chain.
		* As the Present method is called, the CPU continues to execute all others instructions after this and let this sync for the GPU. 
		* Our fence is only to ensure that we are not trying to write on a resource that the GPU is reading from. Like writing to a Command Allocator that are
		* still being executed. It works like a read/write fence with a (busy-wait) stall behavior.
		*/
	};

	while (true)
	{
		Update();
		Render();
	}


	return 0;
}

//We will define functions that are not directly related to rendering below. 
