#pragma once

#include <Av\DETEx.h>
#include "ECSConnection.h"
#include "CriticalSection.h"
#include "DETActionData.h"

class DETAction
{
public:
	virtual ~DETAction();
	virtual Av::DETEx::eError Action(const char* lpXML);
	virtual Av::DETEx::eError GetError(char* lpBuffer, unsigned long *nSize);
	virtual Av::DETEx::eError GetResult(char* lpBuffer, unsigned long *nSize);
	virtual Av::DETEx::eError GetStatus(Av::DETEx::Status* stat);
	virtual Av::DETEx::eError Pause();
	virtual Av::DETEx::eError Resume();
	virtual Av::DETEx::eError Cancel();

protected:
	DETAction();

	//thread routine called by Send to perform file move
	static unsigned int WINAPI TransferFiles(void *pDETAction);

	virtual CString LookupDirectoryByID(int index);
	virtual CString CreatePath(CString SrcFilePath, CString DestPath);

	//dummy placeholder. Should be implemented by push/pull... Action classes
	virtual bool TransferFile(unsigned long index) { return false; }

	CECSConnection m_ECSConnection;
	DETActionData m_Data;

private:
	Av::Int64 DETAction::CalculateTransferSize();
	Av::DETEx::eError DETAction::RollbackState(bool& stateSet, CString msg);
	void SetStatus(Av::DETEx::eError Code, Av::Int64 FileSize = 0, Av::DETEx::eErrorType ErrType = Av::DETEx::ketSuccess);
	virtual void StoreCookieXML();

	static CriticalSection m_CriticalSection;

	int m_CntFilesXfer;
	Av::Int64 m_iBytesXferred;
	Av::Int64 m_iTotalBytesToXfer;

	HANDLE m_hndActionThread;

	Av::DETEx::Status m_Status;
	DETState *m_pState;
	CString m_sLastError;
	CString m_sResultXML;
};

