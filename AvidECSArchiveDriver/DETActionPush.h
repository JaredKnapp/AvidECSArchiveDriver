#pragma once

#include "DETAction.h"

class DETActionPush : public DETAction
{
	virtual bool TransferFile(unsigned long index);
};

