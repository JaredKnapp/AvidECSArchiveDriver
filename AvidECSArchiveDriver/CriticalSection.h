#pragma once

#include "stdafx.h"

class CriticalSection
{
public:
	//Constructor and destructor
	CriticalSection() { InitializeCriticalSection(&CriticalSectionStruct); };
	virtual ~CriticalSection() { DeleteCriticalSection(&CriticalSectionStruct); };

	void Enter() { EnterCriticalSection(&CriticalSectionStruct); };
	void Leave() { LeaveCriticalSection(&CriticalSectionStruct); };


private:
	CRITICAL_SECTION CriticalSectionStruct;
};