#pragma once

#include "DETAction.h"

class DETActionPull : public DETAction
{
	virtual bool TransferFile(unsigned long index);
};

