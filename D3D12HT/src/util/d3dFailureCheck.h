#pragma once

#define WIN32_LEAN_AND_MEAN
#include <Windows.h> // For HRESULT

inline bool Check(HRESULT hr, const char* msg = "DirectX 12 Check Failed!")
{
	if (hr != S_OK)
	{
		std::cerr << "HRESULT has failed: " << msg << "\n";
		__debugbreak();

		return false;
	}

	return true;
}