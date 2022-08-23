namespace HelloTriangle
{
	template<typename T>
	const inline T& HTMax(const T& a, const T& b)
	{
		return (a < b) ? b : a;
	}

}