#include <iostream>

//We will not worry about performance since it is just an assert and it is not meant to be called every frame or so. With this in mind, let's have some flexibility.
#define D3D_ASSERT(Expr, Msg) \
    __M_Assert(Expr, __FILE__, __LINE__, Msg)

inline void __M_Assert(bool expr, const char* file, int line, const char* msg)
{
	if (!expr)
	{
		std::cerr << "Assert failed:\t" << msg << "\n" << "Source:\t\t" << file << ", line " << line << "\n";
		__debugbreak();
	}
}