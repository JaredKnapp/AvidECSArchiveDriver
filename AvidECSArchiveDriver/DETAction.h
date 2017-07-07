#pragma once

#include <Av\DETEx.h>
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

private:
	Av::Int64 DETAction::CalculateTransferSize();
	Av::DETEx::eError DETAction::RollbackState(bool& stateSet, CString msg);
	void SetStatus(Av::DETEx::eError Code, Av::Int64 FileSize = 0, Av::DETEx::eErrorType ErrType = Av::DETEx::ketSuccess);

	static CriticalSection m_CriticalSection;

	int m_CntFilesXfer;
	Av::Int64 m_iBytesXferred;
	Av::Int64 m_iTotalBytesToXfer;

	HANDLE m_hndActionThread;

	DETActionData m_Data;
	Av::DETEx::Status m_Status;
	DETState *m_pState;
	CString m_sLastError;
	CString m_sResultXML;
};

