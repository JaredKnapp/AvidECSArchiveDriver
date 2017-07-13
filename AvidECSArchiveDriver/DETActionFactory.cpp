#include "stdafx.h"

#include "DETActionPush.h"
#include "DETActionPull.h"
#include "DETActionRemove.h"

#include "DETActionFactory.h"


DETAction * DETActionFactory::create(Av::DETEx::ActionType action)
{
	DETAction* pAction = NULL;

	switch (action)
	{
	case Av::DETEx::Push: pAction = new DETActionPush(); break;
	case Av::DETEx::Pull: pAction = new DETActionPull(); break;
	case Av::DETEx::Remove: pAction = new DETActionRemove(); break;
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
