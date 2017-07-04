// AvidECSArchiveDriver.cpp : Defines the initialization routines for the DLL.
//

#include "stdafx.h"
#include "CommonDef.h"
#include "ECSConnection.h"
#include "AvidECSArchiveDriver.h"

#ifdef _DEBUG
#define new DEBUG_NEW
#endif

BEGIN_MESSAGE_MAP(CAvidECSArchiveDriverApp, CWinApp)
END_MESSAGE_MAP()

// CAvidECSArchiveDriverApp construction
CAvidECSArchiveDriverApp::CAvidECSArchiveDriverApp()
{
	// TODO: add construction code here,
	// Place all significant initialization in InitInstance

	//ENable Logging
	FILE* pFile = fopen(LOGFILENAME, "a");
	Output2FILE::Stream() = pFile;
	FILELog::ReportingLevel() = FILELog::FromString("DEBUG");

	FILE_LOG(logDEBUG) << "App Initialized";
	FILE_LOG(logDEBUG) << "About to init ECS";

	//Open ECS Connector
	CECSConnection::Init();

	FILE_LOG(logDEBUG) << "ECS Init Complete";
}

// The one and only CAvidECSArchiveDriverApp object
CAvidECSArchiveDriverApp theApp;

// CAvidECSArchiveDriverApp initialization
BOOL CAvidECSArchiveDriverApp::InitInstance()
{
	CWinApp::InitInstance();

	return TRUE;
}

class DummyClass {
	static void Dummy() {

	}
};
