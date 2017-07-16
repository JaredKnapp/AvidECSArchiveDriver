
#include "stdafx.h"

#include "Messages.h"
#include "DETActionFactory.h"
#include "XMLDomParser.h"

#include "DETBaseExImpl.h"

/****************************
* Application Starting Point
*****************************/
DETEXPORT Av::DETEx::DETBaseEx *DETBaseExCreate()
{
	LOG_DEBUG << "Calling DETBaseEx: DETEXPORT DETBaseExCreate() - Starting";

	try
	{
		return new DETBaseExImpl();
	}
	catch (...)
	{
		LOG_ERROR << sUnknownException;
	}

	return NULL;
}

/****************************
* DETBaseExImpl - implementation
*****************************/
DETBaseExImpl::DETBaseExImpl() : Av::DETEx::DETBaseEx(), m_pDETAction(NULL)
{
	XMLDomParser::Initialize();
}

DETBaseExImpl::~DETBaseExImpl()
{
	XMLDomParser::Terminate();
}

Av::DETEx::eError DETBaseExImpl::VersionEx(long * pwVersion)
{
	LOG_DEBUG << "Calling: DETBaseExImpl::VersionEx(" << pwVersion << ")";
	*pwVersion = 0x00010002;
	return Av::DETEx::keNoError;
}

Av::DETEx::eError DETBaseExImpl::OpenEx(Av::DETEx::ActionType type)
{
	LOG_DEBUG << "Calling: DETBaseExImpl::OpenEx(" << type << ")";
	Av::DETEx::eError retVal = Av::DETEx::keNoError;

	m_pDETAction = DETActionFactory::create(type);
	if (m_pDETAction == NULL)
		retVal = Av::DETEx::keUnimplemented;

	return retVal;
}

Av::DETEx::eError DETBaseExImpl::ActionEx(const char * lpXML)
{
	LOG_DEBUG << "Calling: DETBaseExImpl::ActionEx(" << lpXML << ")";
	if (m_pDETAction != NULL)
	{
		return m_pDETAction->Action(lpXML);
	}
	else
		return Av::DETEx::keNoTransaction;
}

Av::DETEx::eError DETBaseExImpl::CloseEx()
{
	LOG_DEBUG << "Calling: DETBaseExImpl::CloseEx()";
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
	LOG_DEBUG << "Calling: DETBaseExImpl::GetResultEx()"; // << lpBuffer << "," << nSize << ")";
	if (m_pDETAction != NULL)
	{
		return m_pDETAction->GetResult(lpBuffer, nSize);
	}
	else
		return Av::DETEx::keNoTransaction;
}

Av::DETEx::eError DETBaseExImpl::GetLastErrorEx(char * lpBuffer, unsigned long * nSize)
{
	LOG_DEBUG << "Calling: DETBaseExImpl::GetLastErrorEx()"; // << lpBuffer << "," << nSize << ")";
	if (m_pDETAction != NULL)
	{
		return m_pDETAction->GetError(lpBuffer, nSize);
	}
	else
		return Av::DETEx::keNoTransaction;
}

Av::DETEx::eError DETBaseExImpl::GetStatusEx(Av::DETEx::Status * stat)
{
	LOG_DEBUG << "Calling: DETBaseExImpl::GetStatusEx(" << stat << ")";
	if (m_pDETAction != NULL)
	{
		return m_pDETAction->GetStatus(stat);
	}
	else
		return Av::DETEx::keNoTransaction;
}

Av::DETEx::eError DETBaseExImpl::PauseEx()
{
	LOG_DEBUG << "Calling: DETBaseExImpl::PauseEx()";
	if (m_pDETAction != NULL)
	{
		return m_pDETAction->Pause();
	}
	else
		return Av::DETEx::keNoTransaction;
}

Av::DETEx::eError DETBaseExImpl::ResumeEx()
{
	LOG_DEBUG << "Calling: DETBaseExImpl::ResumeEx()";
	if (m_pDETAction != NULL)
	{
		return m_pDETAction->Resume();
	}
	else
		return Av::DETEx::keNoTransaction;
}

Av::DETEx::eError DETBaseExImpl::CancelEx()
{
	LOG_DEBUG << "Calling: DETBaseExImpl::CancelEx()";
	if (m_pDETAction != NULL)
	{
		return m_pDETAction->Cancel();
	}
	else
		return Av::DETEx::keNoTransaction;
}
