#pragma once

#include <Av/DETBaseEx.h>
#include "DETAction.h"

class DETActionFactory
{
public: 
	static DETAction* create(Av::DETEx::ActionType action);

private:
	DETActionFactory();
	~DETActionFactory();
};

