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
#include "../vendor/d3dx12.h"

//Lets import a basic assert.
#include "util/simpleAssert.h"

//We add this to check if our HRESULTs are fine or not.
#include "util/d3dFailureCheck.h"

//Kinda boring to write Microsoft::WRL::ComPtr<> every time.
using namespace Microsoft::WRL;

//To ease the number of header files included by windows
#define WIN32_LEAN_AND_MEAN

#include <Windows.h>


//This is the number of backbuffers we have. This is, how many targets we are rendering while a target is being shown
//While the program is presenting a frame to the screen, we are drawing another one under the hood.
//i.e 2 = double buffering, 3 = triple buffering etc...
const uint8_t g_NumFrames = 3;

//Our initial client dimensions.
//client area = the area of a window that is not the border (like where it shows the window's name etc)
uint32_t g_ClientWidth = 1280;
uint32_t g_ClientHeight = 720;

//Set to true once the DX12 objects have been initialized.
bool g_IsInitialized = false;

//Our Windows window handle, this window will be used to display our rendered image.
HWND g_hWnd; 

//Window rectangle. Is contains coordinates of all points of a rectangle, we will be using this to toggle full screen.
//When switching to full screen, we will use this variable to store the old size of our window so we know where to get back
//when switching back to window.
RECT g_WindowRect;

//-- DirectX 12 Objects --

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


// ------- DX12 Synchronization Objects

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

//If we are going to use VSync.
bool g_VSync = true;


//Sometimes we want to use a custom vsync technology, we can let the tearing occur so the application can decide when the vertical refresh should be done
bool g_TearingSupported = false;

//Fullscreen toggle variable.
bool g_Fullscreen = false;

LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);

int main()
{
	
	return 0;
}