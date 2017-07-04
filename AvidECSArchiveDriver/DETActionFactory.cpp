#include "stdafx.h"
#include "DETActionPush.h"
#include "DETActionFactory.h"


DETAction * DETActionFactory::create(Av::DETEx::ActionType action)
{
	DETAction* pAction = NULL;

	switch (action)
	{
	case Av::DETEx::Push: pAction = new DETActionPush(); break;
	case Av::DETEx::Pull: break;
	case Av::DETEx::Remove: break;
	case Av::DETEx::Iterate: break;
	default:
		pAction = NULL;
	}
	return pAction;
}

DETActionFactory::DETActionFactory()
{
}


DETActionFactory::~DETActionFactory()
{
}
