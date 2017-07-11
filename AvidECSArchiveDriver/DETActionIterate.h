#pragma once

#include "DETAction.h"

class DETActionIterate : public DETAction
{
public:
	virtual Av::DETEx::eError Action(const char* lpXML);
};

