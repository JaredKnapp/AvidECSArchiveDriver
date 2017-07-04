// AvidECSArchiveDriver.h : main header file for the AvidECSArchiveDriver DLL
//

#pragma once

#ifndef __AFXWIN_H__
	#error "include 'stdafx.h' before including this file for PCH"
#endif

#include "resource.h"		// main symbols


// CAvidECSArchiveDriverApp
// See AvidECSArchiveDriver.cpp for the implementation of this class
//

class CAvidECSArchiveDriverApp : public CWinApp
{
public:
	CAvidECSArchiveDriverApp();

// Overrides
public:
	virtual BOOL InitInstance();

	DECLARE_MESSAGE_MAP()
};
