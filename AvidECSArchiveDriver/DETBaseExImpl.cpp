
#include "stdafx.h"

#include "Messages.h"
#include "DETActionFactory.h"
#include "DETBaseExImpl.h"

/****************************
* Application Starting Point
*****************************/
DETEXPORT Av::DETEx::DETBaseEx *DETBaseExCreate()
{
	FILE_LOG(logDEBUG) << "Calling DETBaseEx: DETEXPORT DETBaseExCreate() - Starting";

	try
	{
		return new DETBaseExImpl();
	}
	catch (...)
	{
		FILE_LOG(logERROR) << sUnknownException;
	}

	return NULL;
}

/****************************
* DETBaseExImpl - implementation
*****************************/
DETBaseExImpl::DETBaseExImpl() : Av::DETEx::DETBaseEx(), m_pDETAction(NULL)
{
}

DETBaseExImpl::~DETBaseExImpl()
{
}

Av::DETEx::eError DETBaseExImpl::VersionEx(long * pwVersion)
{
	FILE_LOG(logDEBUG) << "Calling: DETBaseExImpl::VersionEx(" << pwVersion << ")";
	*pwVersion = 0x00010002;
	return Av::DETEx::keNoError;
}

Av::DETEx::eError DETBaseExImpl::OpenEx(Av::DETEx::ActionType type)
{
	FILE_LOG(logDEBUG) << "Calling: DETBaseExImpl::OpenEx(" << type << ")";
	Av::DETEx::eError retVal = Av::DETEx::keNoError;

	m_pDETAction = DETActionFactory::create(type);
	if (m_pDETAction == NULL)
		retVal = Av::DETEx::keUnimplemented;

	return retVal;
}

Av::DETEx::eError DETBaseExImpl::ActionEx(const char * lpXML)
{
	FILE_LOG(logDEBUG) << "Calling: DETBaseExImpl::ActionEx(" << lpXML << ")";
	return Av::DETEx::keUnimplemented;
}

Av::DETEx::eError DETBaseExImpl::CloseEx()
{
	FILE_LOG(logDEBUG) << "Calling: DETBaseExImpl::CloseEx()";
	if (m_pDETAction != NULL)
	{
		delete m_pDETAction;
		m_pDETAction = NULL;
		return Av::DETEx::keNoError;
	}
	else
		return Av::DETEx::keNoTransaction;
}

Av::DETEx::eError DETBaseExImpl::GetResultEx(char * lpBuffer, unsigned long * nSize)
{
	FILE_LOG(logDEBUG) << "Calling: DETBaseExImpl::GetResultEx(" << lpBuffer << "," << nSize << ")";
	return Av::DETEx::keUnimplemented;
}

Av::DETEx::eError DETBaseExImpl::GetLastErrorEx(char * lpBuffer, unsigned long * nSize)
{
	FILE_LOG(logDEBUG) << "Calling: DETBaseExImpl::GetLastErrorEx(" << lpBuffer << "," << nSize << ")";
	return Av::DETEx::keUnimplemented;
}

Av::DETEx::eError DETBaseExImpl::GetStatusEx(Av::DETEx::Status * stat)
{
	FILE_LOG(logDEBUG) << "Calling: DETBaseExImpl::GetStatusEx(" << stat << ")";
	if (m_pDETAction != NULL)
	{
		return m_pDETAction->GetStatus(stat);
	}
	else
		return Av::DETEx::keNoTransaction;
}

Av::DETEx::eError DETBaseExImpl::PauseEx()
{
	FILE_LOG(logDEBUG) << "Calling: DETBaseExImpl::PauseEx()";
	if (m_pDETAction != NULL)
	{
		return m_pDETAction->Pause();
	}
	else
		return Av::DETEx::keNoTransaction;
}

Av::DETEx::eError DETBaseExImpl::ResumeEx()
{
	FILE_LOG(logDEBUG) << "Calling: DETBaseExImpl::ResumeEx()";
	if (m_pDETAction != NULL)
	{
		return m_pDETAction->Resume();
	}
	else
		return Av::DETEx::keNoTransaction;
}

Av::DETEx::eError DETBaseExImpl::CancelEx()
{
	FILE_LOG(logDEBUG) << "Calling: DETBaseExImpl::CancelEx()";
	if (m_pDETAction != NULL)
	{
		return m_pDETAction->Cancel();
	}
	else
		return Av::DETEx::keNoTransaction;
}
