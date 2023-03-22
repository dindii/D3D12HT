namespace HTUtils
{
	template<typename T>
	const inline T HTMax(T a, T b)
	{
		return (a < b) ? b : a;
	}

}