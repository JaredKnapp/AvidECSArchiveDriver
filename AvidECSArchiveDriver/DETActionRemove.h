#pragma once

#include "DETAction.h"

class DETActionRemove : public DETAction
{
public:
	virtual Av::DETEx::eError Action(const char* lpXML);
};

