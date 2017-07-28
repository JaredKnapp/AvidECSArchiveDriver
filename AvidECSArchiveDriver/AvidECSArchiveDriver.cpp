// AvidECSArchiveDriver.cpp : Defines the initialization routines for the DLL.
//

#include "stdafx.h"
#include "CommonDef.h"
#include "ECSConnection.h"
#include "XMLDomParser.h"

#include "AvidECSArchiveDriver.h"

#ifdef _DEBUG
#define new DEBUG_NEW
#endif

BEGIN_MESSAGE_MAP(CAvidECSArchiveDriverApp, CWinApp)
END_MESSAGE_MAP()



CStringW thisDllDirPath()
{
	CStringW thisPath = L"";
	WCHAR path[MAX_PATH];
	HMODULE hm;
	if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
		GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		(LPWSTR)&thisDllDirPath, &hm))
	{
		GetModuleFileNameW(hm, path, sizeof(path));
		PathRemoveFileSpecW(path);
		thisPath = CStringW(path);
		if (!thisPath.IsEmpty() &&
			thisPath.GetAt(thisPath.GetLength() - 1) != '\\')
			thisPath += L"\\";
	}

	return thisPath;
}

WCHAR * GetThisDllPath()
{
	CStringW thisPath = L"";
	WCHAR path[MAX_PATH];
	HMODULE hm;
	if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
		GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		(LPWSTR)&thisDllDirPath, &hm))
	{
		GetModuleFileNameW(hm, path, sizeof(path));
		PathRemoveFileSpecW(path);
		thisPath = CStringW(path);
		if (!thisPath.IsEmpty() &&
			thisPath.GetAt(thisPath.GetLength() - 1) != '\\')
			thisPath += L"\\";
	}

	return path;
}



// CAvidECSArchiveDriverApp construction
CAvidECSArchiveDriverApp::CAvidECSArchiveDriverApp()
{
	// TODO: add construction code here,
	// Place all significant initialization in InitInstance

	//Logging: https://github.com/SergiusTheBest/plog
	plog::init(plog::info, LOGFILENAME, 1000000, 5);

	//Open ECS Connector
	CECSConnection::Init();
	LOG_DEBUG << "Current Filepath = " << GetThisDllPath();
	LOG_DEBUG << "App Initialized";
}

// The one and only CAvidECSArchiveDriverApp object
CAvidECSArchiveDriverApp theApp;

// CAvidECSArchiveDriverApp initialization
BOOL CAvidECSArchiveDriverApp::InitInstance()
{
	CWinApp::InitInstance();
	return TRUE;
}
