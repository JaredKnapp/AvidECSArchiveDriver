#pragma once

#include <stack>
#include <Av\DETEx.h>
#include "CommonDef.h"

class DETState
{
public:
	DETState() { m_StateStack.push(vrsNone); m_StateStack.push(vrsNone); }
	DETState(intRunState State) { m_StateStack.push(vrsNone); m_StateStack.push(State); }
	virtual ~DETState();

	bool IsTransitionValid(intRunState TransState);
	bool IsTransitionValid(intRunState CurState, intRunState TransState);
	Av::DETEx::eRunState GetState();
	bool SetState(intRunState State);
	void RollBackState();
	intRunState GetInternalState();
	CString GetStateString(intRunState State);
	bool IsValid(intRunState State);

private:

	//data members
	typedef std::stack<intRunState> RUNSTATE_STACK;
	RUNSTATE_STACK m_StateStack;

};