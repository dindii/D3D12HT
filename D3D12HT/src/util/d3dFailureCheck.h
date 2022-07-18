#pragma once

#define WIN32_LEAN_AND_MEAN
#include <Windows.h> // For HRESULT

inline void Check(HRESULT hr, const char* msg)
{
	if (hr != S_OK)
	{
		std::cerr << "HRESULT has failed: " << msg << "\n";
		__debugbreak();
	}
}