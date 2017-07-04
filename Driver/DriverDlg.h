// DriverDlg.h : header file
//

#pragma once
#include "afxwin.h"

#include <map>
#include <Av/DETBaseEx.h>

#define DETBASEEX_CREATE_FUNCTION_NAME _T("DETBaseExCreate")

#ifdef _DEBUG
#define THIRD_PARTY_DLL_NAME _T("DETRefImplV2d.dll")
#else
#define THIRD_PARTY_DLL_NAME _T("DETRefImplV2.dll")
#endif
#define MAX_SIZE 1024


// CDriverDlg dialog
class CDriverDlg : public CDialog
{
// Construction
public:
	CDriverDlg(CWnd* pParent = NULL);	// standard constructor

// Dialog Data
	enum { IDD = IDD_DRIVER_DIALOG };

	protected:
	virtual void DoDataExchange(CDataExchange* pDX);	// DDX/DDV support


// Implementation
protected:
	HICON m_hIcon;

	// Generated message map functions
	virtual BOOL OnInitDialog();
	afx_msg void OnSysCommand(UINT nID, LPARAM lParam);
	afx_msg void OnPaint();
	afx_msg HCURSOR OnQueryDragIcon();
	virtual void OnCancel();
	DECLARE_MESSAGE_MAP()
public:
	afx_msg void OnBnClickedOpen1();
	afx_msg void OnBnClickedAction1();
	afx_msg void OnBnClickedCancel1();
	afx_msg void OnBnClickedClose1();
	afx_msg void OnBnClickedGeterror1();
	CEdit m_Edit1;
	CEdit m_FilenameEdit;
	CListBox m_Listbox;
public:
	bool GetRunStatus();
	bool CDriverDlg::CallDETGetStatus(Av::DETEx::Status* s);
	bool IsInitialized();

private:
	HINSTANCE m_dllHandle;
	int m_CurSel;

	//File move thread handle
	HANDLE m_StatusThreadHandle;
	bool m_RunStatusThread;
	
	Av::DETEx::DETBaseEx* m_DETBaseExPtr;

public:
	CListBox m_StatusListBox;
	afx_msg void OnBnClickedClearstatus();
	afx_msg void OnBnClickedPause();
	afx_msg void OnBnClickedResume();
	afx_msg void OnBnClickedGetresult();
	CEdit m_DestXMLFilenameEdit;

private:
	std::string FindVendorDllName();

};
