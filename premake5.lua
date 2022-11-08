workspace "D3D12HT"
		architecture "x64"
		startproject "D3D12HT"


configurations
{
	"Debug",
	"Release",
	"Dist"
}

characterset ("MBCS")

outputdir = "%{cfg.buildcfg}-%{cfg.system}-%{cfg.architecture}"


project "D3D12HT"
	location "D3D12HT"
	kind "ConsoleApp"
	language "C++"
	cppdialect "C++17"
	staticruntime "on"

	targetdir("bin/" .. outputdir .. "/%{prj.name}")
	objdir("bin-int/" .. outputdir .. "/%{prj.name}")

	files
	{
		"%{prj.name}/src/**.h",
		"%{prj.name}/src/**.cpp",
		"%{prj.name}/vendor/**.h",
		"%{prj.name}/vendor/**.cpp",
	}

	links
	{
		"d3d12.lib",
		"DXGI.lib",
	}

	includedirs
	{
		"%{prj.name}/src",
		"%{prj.name}/vendor",
	}

	filter "system:windows"
	systemversion "latest"

	defines
	{
		"D3D12HT_PLATFORM_WINDOWS"
	}

	filter "configurations:Debug"
	defines "D3D12HT_DEBUG"
	runtime "Debug"
	symbols "on"

	filter "configurations:Release"
	defines "D3D12HT_RELEASE"
	runtime "Release"
	optimize "Debug"

	filter "configurations:Dist"
	defines "D3D12HT_DIST"
	runtime "Release"
	symbols "Off"
	optimize "Full"