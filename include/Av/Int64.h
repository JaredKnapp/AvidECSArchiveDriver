////////////////////////////////////////////////////////////////////////////////
// Av/Int64.h
//
#pragma once
#ifndef AVINT64
#define AVINT64


#ifdef _WIN32

namespace Av
{
	typedef __int64 Int64;
}

#endif

#elif macintosh
#include <ToolUtils.h>
namespace Av
{
	typedef Int64bit Int64;
}


#endif
