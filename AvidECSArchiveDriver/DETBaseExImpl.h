#pragma once

#include <Av/DETBaseEx.h>

#include "DETAction.h"

class DETBaseExImpl : public Av::DETEx::DETBaseEx
{
public:
	DETBaseExImpl();
	virtual ~DETBaseExImpl();
	virtual Av::DETEx::eError VersionEx(long * pwVersion) override;
	virtual Av::DETEx::eError OpenEx(Av::DETEx::ActionType type) override;
	virtual Av::DETEx::eError ActionEx(const char * lpXML) override;
	virtual Av::DETEx::eError CloseEx() override;
	virtual Av::DETEx::eError GetResultEx(char * lpBuffer, unsigned long * nSize) override;
	virtual Av::DETEx::eError GetLastErrorEx(char * lpBuffer, unsigned long * nSize) override;
	virtual Av::DETEx::eError GetStatusEx(Av::DETEx::Status * stat) override;
	virtual Av::DETEx::eError PauseEx() override;
	virtual Av::DETEx::eError ResumeEx() override;
	virtual Av::DETEx::eError CancelEx() override;

private:
	DETAction* m_pDETAction;
};

