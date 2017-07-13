// DriverDlg.cpp : implementation file
//

// this is necessary for SetDllDirectory, new in WinXP SP1
#define _WIN32_WINNT 0x0502 

#include <strsafe.h>

#include "stdafx.h"
#include "Driver.h"

#include "DriverDlg.h"
#include <windows.h>
#include <process.h>
#include ".\driverdlg.h"

#ifdef _DEBUG
#define new DEBUG_NEW
#endif

#define MAX_ERRORS 128

void ErrorExit(LPTSTR lpszFunction)
{
	// Retrieve the system error message for the last-error code

	LPVOID lpMsgBuf;
	LPVOID lpDisplayBuf;
	DWORD dw = GetLastError();

	FormatMessage(
		FORMAT_MESSAGE_ALLOCATE_BUFFER |
		FORMAT_MESSAGE_FROM_SYSTEM |
		FORMAT_MESSAGE_IGNORE_INSERTS,
		NULL,
		dw,
		MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
		(LPTSTR)&lpMsgBuf,
		0, NULL);

	// Display the error message and exit the process

	lpDisplayBuf = (LPVOID)LocalAlloc(LMEM_ZEROINIT, (lstrlen((LPCTSTR)lpMsgBuf) + lstrlen((LPCTSTR)lpszFunction) + 40) * sizeof(TCHAR));
	//StringCchPrintf((LPTSTR)lpDisplayBuf, LocalSize(lpDisplayBuf) / sizeof(TCHAR), TEXT("%s failed with error %d: %s"), lpszFunction, dw, lpMsgBuf);
	//MessageBox(NULL, (LPCTSTR)lpDisplayBuf, TEXT("Error"), MB_OK);

	LocalFree(lpMsgBuf);
	LocalFree(lpDisplayBuf);
}

static CString retrieveErrorString(int ret)
{
	CString tmp;

	switch (ret)
	{
	case Av::DETEx::keNoError: tmp = "Success";break;
	case Av::DETEx::keUnimplemented: tmp = "Failed: keUnimplemented";break;
	case Av::DETEx::keCancel: tmp = "Failed: keCancel";break;
	case Av::DETEx::keNetwork: tmp = "Failed: keNetwork";break;
	case Av::DETEx::keInternalError: tmp = "Failed: keInternalError";break;
	case Av::DETEx::keInvalidXML: tmp = "Failed: keInvalidXML";break;
	case Av::DETEx::keDiskFull: tmp = "Failed: keDiskFull";break;
	case Av::DETEx::keNotSuspended: tmp = "Failed: keNotSuspended";break;
	case Av::DETEx::keMissingFile: tmp = "Failed: keMissingFile";break;
	case Av::DETEx::keNoTransaction: tmp = "Failed: keNoTransaction";break;
	case Av::DETEx::keIterationFinished: tmp = "Failed: keIterationFinished";break;
	case Av::DETEx::keSessionFailed: tmp = "Failed: keSessionFailed";break;
	case Av::DETEx::kePartialTransfer: tmp = "Failed: kePartialTransfer";break;
	case Av::DETEx::keVendorFailedToStart: tmp = "Failed: keVendorFailedToStart";break;
	case Av::DETEx::keVendorTerminated: tmp = "Failed: keVendorTerminated";break;
	case -99: tmp = "Failed: Initialization Failed";break;
	case -100: tmp = "Failed: XML file not found";break;
	default:
		tmp = "UNKNOWN";
	}
	return tmp;
}

typedef void* (CALLBACK* LPFNDLLFUNC1)();
		
void* LoadCreateObjectPtr(HINSTANCE dllHandle, const char* funcName)
{
	void* objAddr = NULL;
	// locate the address of the create function

	LPFNDLLFUNC1 pfCreate = (LPFNDLLFUNC1)GetProcAddress(dllHandle, funcName);
	if (NULL != pfCreate)
	{
		objAddr = (*pfCreate)();
	}

	return objAddr;
}

bool CDriverDlg::IsInitialized()
{
	return (m_DETBaseExPtr != NULL) ? true : false;
}


bool CDriverDlg::CallDETGetStatus(Av::DETEx::Status* s)
{
	if (IsInitialized())
	{
		m_DETBaseExPtr->GetStatusEx(s);
		return true;
	}
	else
		return false;
}

// CAboutDlg dialog used for App About

class CAboutDlg : public CDialog
{
public:
	CAboutDlg();

// Dialog Data
	enum { IDD = IDD_ABOUTBOX };

	protected:
	virtual void DoDataExchange(CDataExchange* pDX);    // DDX/DDV support

// Implementation
protected:
	DECLARE_MESSAGE_MAP()
};

CAboutDlg::CAboutDlg() : CDialog(CAboutDlg::IDD)
{
}

void CAboutDlg::DoDataExchange(CDataExchange* pDX)
{
	CDialog::DoDataExchange(pDX);
}

BEGIN_MESSAGE_MAP(CAboutDlg, CDialog)
END_MESSAGE_MAP()


// CDriverDlg dialog



CDriverDlg::CDriverDlg(CWnd* pParent /*=NULL*/)
	: CDialog(CDriverDlg::IDD, pParent),
	m_DETBaseExPtr(NULL),
	m_RunStatusThread(false), m_StatusThreadHandle(INVALID_HANDLE_VALUE)
{
	m_hIcon = AfxGetApp()->LoadIcon(IDR_MAINFRAME);
}

void CDriverDlg::DoDataExchange(CDataExchange* pDX)
{
	CDialog::DoDataExchange(pDX);
	DDX_Control(pDX, IDC_EDIT2, m_Edit1);
	DDX_Control(pDX, IDC_LIST1, m_Listbox);
	DDX_Control(pDX, IDC_FileNameEdit, m_FilenameEdit);
	DDX_Control(pDX, IDC_StatusListBox, m_StatusListBox);
	DDX_Control(pDX, IDC_FileNameEdit2, m_DestXMLFilenameEdit);
}

BEGIN_MESSAGE_MAP(CDriverDlg, CDialog)
	ON_WM_SYSCOMMAND()
	ON_WM_PAINT()
	ON_WM_QUERYDRAGICON()
	//}}AFX_MSG_MAP
	ON_BN_CLICKED(IDC_Init1, OnBnClickedOpen1)
	ON_BN_CLICKED(IDC_Start1, OnBnClickedAction1)
	ON_BN_CLICKED(IDC_Stop1, OnBnClickedCancel1)
	ON_BN_CLICKED(IDC_Terminate1, OnBnClickedClose1)
	ON_BN_CLICKED(IDC_GetError1, OnBnClickedGeterror1)
	ON_BN_CLICKED(IDC_ClearStatus, OnBnClickedClearstatus)
	ON_BN_CLICKED(IDC_Pause, OnBnClickedPause)
	ON_BN_CLICKED(IDC_Resume, OnBnClickedResume)
	ON_BN_CLICKED(IDC_GetResult, OnBnClickedGetresult)
END_MESSAGE_MAP()


// CDriverDlg message handlers

BOOL CDriverDlg::OnInitDialog()
{
	CDialog::OnInitDialog();

	// Add "About..." menu item to system menu.

	// IDM_ABOUTBOX must be in the system command range.
	ASSERT((IDM_ABOUTBOX & 0xFFF0) == IDM_ABOUTBOX);
	ASSERT(IDM_ABOUTBOX < 0xF000);

	CMenu* pSysMenu = GetSystemMenu(FALSE);
	if (pSysMenu != NULL)
	{
		CString strAboutMenu;
		strAboutMenu.LoadString(IDS_ABOUTBOX);
		if (!strAboutMenu.IsEmpty())
		{
			pSysMenu->AppendMenu(MF_SEPARATOR);
			pSysMenu->AppendMenu(MF_STRING, IDM_ABOUTBOX, strAboutMenu);
		}
	}

	// Set the icon for this dialog.  The framework does this automatically
	//  when the application's main window is not a dialog
	SetIcon(m_hIcon, TRUE);			// Set big icon
	SetIcon(m_hIcon, FALSE);		// Set small icon

	m_Listbox.InsertString(Av::DETEx::Push,"Push");
	m_Listbox.InsertString(Av::DETEx::Pull,"Pull");
	m_Listbox.InsertString(Av::DETEx::Remove,"Remove");
	m_Listbox.InsertString(Av::DETEx::Iterate,"Iterate");

	m_Listbox.SetCurSel(Av::DETEx::Push);

	m_DestXMLFilenameEdit.SetWindowText("C:\\temp\\cookie.xml");

	return TRUE;  // return TRUE  unless you set the focus to a control
}

void CDriverDlg::OnCancel()
{
	TerminateThread(m_StatusThreadHandle, 1);
	CDialog::OnCancel();
}

void CDriverDlg::OnSysCommand(UINT nID, LPARAM lParam)
{
	if ((nID & 0xFFF0) == IDM_ABOUTBOX)
	{
		CAboutDlg dlgAbout;
		dlgAbout.DoModal();
	}
	else
	{
		CDialog::OnSysCommand(nID, lParam);
	}
}

// If you add a minimize button to your dialog, you will need the code below
//  to draw the icon.  For MFC applications using the document/view model,
//  this is automatically done for you by the framework.

void CDriverDlg::OnPaint() 
{
	if (IsIconic())
	{
		CPaintDC dc(this); // device context for painting

		SendMessage(WM_ICONERASEBKGND, reinterpret_cast<WPARAM>(dc.GetSafeHdc()), 0);

		// Center icon in client rectangle
		int cxIcon = GetSystemMetrics(SM_CXICON);
		int cyIcon = GetSystemMetrics(SM_CYICON);
		CRect rect;
		GetClientRect(&rect);
		int x = (rect.Width() - cxIcon + 1) / 2;
		int y = (rect.Height() - cyIcon + 1) / 2;

		// Draw the icon
		dc.DrawIcon(x, y, m_hIcon);
	}
	else
	{
		CDialog::OnPaint();
	}
}

// The system calls this function to obtain the cursor to display while the user drags
//  the minimized window.
HCURSOR CDriverDlg::OnQueryDragIcon()
{
	return static_cast<HCURSOR>(m_hIcon);
}

std::string CDriverDlg::FindVendorDllName()
{
	char buff[MAX_SIZE];
	DWORD valSize = MAX_SIZE;
	std::string dllPath;
	HKEY key;

	if (ERROR_SUCCESS == RegOpenKeyEx(HKEY_LOCAL_MACHINE, DET_VENDOR_SUBKEY,
 				 0, KEY_READ| KEY_WOW64_64KEY,&key))
	{
		if (ERROR_SUCCESS == RegQueryValueEx(key, 
								VENDOR_DIRECTORY, 
								0, 
								NULL, 
								(unsigned char *)buff, 
								&valSize))
		{
			dllPath = buff;
			if (ERROR_SUCCESS == RegQueryValueEx(key, 
									VENDOR_DLL_FILENAME, 
									0, 
									NULL, 
									(unsigned char *)buff, 
									&valSize))
			{
				dllPath += "\\";
				dllPath += buff;
			}
		}
		RegCloseKey(key);
	}
	return dllPath;
}

static void parsePath(LPCTSTR filepath, LPTSTR dirname, size_t dirSize)
{
	TCHAR drive[MAX_SIZE];
	TCHAR dir[MAX_SIZE];
	TCHAR fname[MAX_SIZE];
	TCHAR ext[MAX_SIZE];

	_tsplitpath_s( filepath, drive, MAX_SIZE, dir, MAX_SIZE, fname, MAX_SIZE, ext, MAX_SIZE);
    _tmakepath_s( dirname, dirSize, drive, dir, NULL, NULL);
}

void CDriverDlg::OnBnClickedOpen1()
{
	int ret = -99;
	CString tmp;
	long version = -1;

	std::string dllname = FindVendorDllName();
	if (dllname.size() <= 0)
	{
		AfxMessageBox("Failed to find vendor dll from registry");
		return;
	}

	TCHAR dirname[2048];
	parsePath(dllname.c_str(), dirname, 2048);
	SetDllDirectory(dirname);

	m_dllHandle = LoadLibrary(dllname.c_str());
	if (m_dllHandle == NULL) ret = Av::DETEx::keMissingFile;

	void* objPtr = LoadCreateObjectPtr(m_dllHandle, DETBASEEX_CREATE_FUNCTION_NAME);
	if (objPtr != NULL)
	{
		m_DETBaseExPtr = reinterpret_cast<Av::DETEx::DETBaseEx *>(objPtr);

		m_DETBaseExPtr->VersionEx(&version);
		ret = m_DETBaseExPtr->OpenEx((Av::DETEx::ActionType)m_Listbox.GetCurSel());
	}
	else
	{
		ErrorExit(TEXT("Init"));
		ret = Av::DETEx::keMissingFile;
	}
	 
	tmp.Format(_T("%s. Version: %#x"), retrieveErrorString(ret), version);
	m_Edit1.SetWindowText(tmp);
	UpdateData();
}


 
static bool readInputXML(const char* filepath, unsigned char* buff, unsigned long maxSize)
{
	FILE* fp;
	errno_t err = fopen_s(&fp, filepath, "r");

	if (err == 0)
	{
		
		size_t size = fread(buff, 1, maxSize, fp);
		buff[size] = '\0';
		buff[size+1] = '\0';
		fclose(fp);
		return true;
	}
	else
		return false;
}

bool CDriverDlg::GetRunStatus()
{
	return m_RunStatusThread;
}

static std::string GetStateMapping(int s)
{
	std::string tmp = "UNKNOWN";

	switch (s)
	{
	case Av::DETEx::krsIdle: tmp = "Idle"; break;
	case Av::DETEx::krsRun: tmp = "Run"; break;
	case Av::DETEx::krsPause: tmp = "Pause"; break;
	case Av::DETEx::krsUnknown: tmp = "Unknown"; break;
	case Av::DETEx::krsCancel: tmp = "Cancel"; break;
	case Av::DETEx::krsFinish: tmp = "Finish"; break;
	case Av::DETEx::krsNone: tmp = "None"; break;
	}
	return tmp;
}

static void runStatusThread(void* p)
{
	CDriverDlg* dlgPtr = (CDriverDlg*)p;

	while (true)
	{
		if (dlgPtr->GetRunStatus())
		{
			Av::DETEx::Status s;
			CString statString;
			int count = dlgPtr->m_StatusListBox.GetCount();

			if (dlgPtr->CallDETGetStatus(&s))
			{
				std::string stateMapping = GetStateMapping(s.State);
				std::string errorStr = retrieveErrorString(s.Code);
				statString.Format("%d)%s State, \t%s, \t%d%%, \t%I64d/%I64d KB, \t%d FilesXferred", 
					count,stateMapping.c_str(), errorStr.c_str(), s.PercentXferComplete,
					s.KBytesXferred / 1024, s.TotalKBytesToXfer / 1024, s.NumFilesXfer);
				dlgPtr->m_StatusListBox.InsertString(0, statString);
			}
			else
				dlgPtr->m_StatusListBox.InsertString(0, "Failed to retrieve status");

			Sleep(1000);

		}
		else
			break;
	}
}

#define MAX_XML_SIZE 512000
#define XML_FILEPATH "c:\\DET2\\push2.xml"

void CDriverDlg::OnBnClickedAction1()
{
	int ret = -99;
	unsigned char buff[MAX_XML_SIZE];
	CString fn;

	if (!IsInitialized()) {AfxMessageBox("Must init first"); return;}

	m_FilenameEdit.GetWindowText(fn);
	if (fn.IsEmpty()) fn = XML_FILEPATH;
	if (readInputXML(fn, buff, MAX_XML_SIZE)) 
	{ 
		ret = m_DETBaseExPtr->ActionEx((const char*)buff);
		if (ret == Av::DETEx::keNoError)
		{
			// start a status monitor thread
			m_RunStatusThread = true;
			m_StatusThreadHandle = (HANDLE)_beginthread(runStatusThread, 0, this);
		}
	}
	else
		ret = -100;

	m_Edit1.SetWindowText(retrieveErrorString(ret));
	UpdateData();
}

void CDriverDlg::OnBnClickedCancel1()
{
	int ret = -99;
	if (!IsInitialized()) {AfxMessageBox("Must init first"); return;}
	ret = m_DETBaseExPtr->CancelEx();

	m_Edit1.SetWindowText(retrieveErrorString(ret));
	UpdateData();

}

void CDriverDlg::OnBnClickedClose1()
{
	if (!IsInitialized()) {AfxMessageBox("Must init first"); return;}
	Av::DETEx::eError ret = m_DETBaseExPtr->CloseEx();
	delete m_DETBaseExPtr;
	m_DETBaseExPtr = NULL;
	m_RunStatusThread = false;

	m_Edit1.SetWindowText(retrieveErrorString(ret));
	UpdateData();
}

void CDriverDlg::OnBnClickedGeterror1()
{
	if (!IsInitialized()) {AfxMessageBox("Must init first"); return;}
	char buff[MAX_SIZE];
	unsigned long size = MAX_SIZE;
	m_DETBaseExPtr->GetLastErrorEx(buff, &size);

	m_Edit1.SetWindowText(buff);
	UpdateData();	
}

void CDriverDlg::OnBnClickedClearstatus()
{
	m_Edit1.SetWindowText(NULL);
	m_StatusListBox.ResetContent();
}

void CDriverDlg::OnBnClickedPause()
{
	int ret = -99;
	if (!IsInitialized()) {AfxMessageBox("Must init first"); return;}
	ret = m_DETBaseExPtr->PauseEx();

	m_Edit1.SetWindowText(retrieveErrorString(ret));
	UpdateData();
}

void CDriverDlg::OnBnClickedResume()
{
	int ret = -99;
	if (!IsInitialized()) {AfxMessageBox("Must init first"); return;}
	ret = m_DETBaseExPtr->ResumeEx();

	m_Edit1.SetWindowText(retrieveErrorString(ret));
	UpdateData();
}

void CDriverDlg::OnBnClickedGetresult()
{
	if (!IsInitialized()) {AfxMessageBox("Must init first"); return;}
	char* buff = NULL;
	unsigned long size = 0;

	int ret = m_DETBaseExPtr->GetResultEx(buff, &size);
	if (size > 0)
	{
		size += 1;
		buff = new char[size];
		m_DETBaseExPtr->GetResultEx(buff, &size);

		CString fn;

		m_DestXMLFilenameEdit.GetWindowText(fn);
		CFile f(fn, CFile::modeCreate | CFile::modeWrite);
		f.Write(buff, (UINT)strlen(buff));
		f.Close();

		delete [] buff;

		CString msg;
		msg.Format("Result XML has been written to %s", fn);
		m_Edit1.SetWindowText(msg);
	}
	else
	{
		m_Edit1.SetWindowText(retrieveErrorString(ret));
	}

	UpdateData();	

}
