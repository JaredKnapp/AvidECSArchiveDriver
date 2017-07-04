#include "stdafx.h"

#include "DETState.h"

#include "CriticalSection.h"


/////////////////////////////////////////////////////////////////////////////
// DETState

DETState::~DETState()
{
}

/*
****************************************************************
**
** Public memebers
**
****************************************************************
*/
bool DETState::IsTransitionValid(intRunState TransState)
{
	intRunState StateToTransition = vrsNone;

	for (int idx = 0; idx < MaxStateDepth; idx++)
	{
		//check if TransState matches one of the acceptable transition states
		StateToTransition = StateTbl[(int)m_StateStack.top()][idx];

		if (StateToTransition == TransState)
		{
			return true;
		}
	}

	return false;
}

bool DETState::IsTransitionValid(intRunState CurState, intRunState TransState)
{
	intRunState StateToTransition = vrsNone;

	for (int idx = 0; idx < MaxStateDepth; idx++)
	{
		//check if TransState matches one of the acceptable transition states
		StateToTransition = StateTbl[(int)CurState][idx];

		if (StateToTransition == TransState)
		{
			return true;
		}
	}

	return false;
}

bool DETState::IsValid(intRunState State)
{
	//check if CurState matches the state we're actually in 
	intRunState CurState = m_StateStack.top();

	if (CurState == State)
	{
		return true;
	}

	return false;
}

bool DETState::SetState(intRunState State) {
	//first determine if this is an acceptable state to transition to State
	bool ValidTrans = IsTransitionValid(m_StateStack.top(), State);

	if (ValidTrans) {
		//Update internal state, also convert to a valid DET TM state
		m_StateStack.push(State);
	}

	return ValidTrans;
}

void DETState::RollBackState()
{
	if (!m_StateStack.empty())
	{
		m_StateStack.pop();
	}
}

Av::DETEx::eRunState DETState::GetState()
{
	return StateMapTbl[(int)m_StateStack.top()][0];
}

intRunState DETState::GetInternalState()
{
	return m_StateStack.top();
}

CString DETState::GetStateString(intRunState State)
{
	return StateStrings[State];
}