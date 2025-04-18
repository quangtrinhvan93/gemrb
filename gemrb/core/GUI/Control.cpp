/* GemRB - Infinity Engine Emulator
 * Copyright (C) 2003 The GemRB Project
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.

 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.

 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 *
 */

#include "GUI/Control.h"

#include "ie_cursors.h"

#include "Debug.h"
#include "Interface.h"
#include "Sprite2D.h"
#include "Timer.h"

#include "GUI/GUIScriptInterface.h"
#include "GUI/Window.h"
#include "Logging/Logging.h"

#include <cstdio>

namespace GemRB {

tick_t Control::ActionRepeatDelay = 250;

const Control::ValueRange Control::MaxValueRange = std::make_pair(0, std::numeric_limits<value_t>::max());

Control::Control(const Region& frame)
	: View(frame) // dont pass superview to View constructor
{
	SetValueRange(MaxValueRange);
}

Control::~Control()
{
	ClearActionTimer();
}

void Control::SetAction(ControlEventHandler handler, Control::Action type, EventButton button,
			Event::EventMods mod, short count)
{
	ControlActionKey key(type, mod, button, count);
	return SetAction(std::move(handler), key);
}

void Control::SetAction(Responder handler, const ActionKey& key)
{
	if (handler) {
		actions[key] = std::move(handler);
	} else {
		// delete the entry if there is one instead of setting it to NULL
		const auto& it = actions.find(key);
		if (it != actions.end()) {
			actions.erase(it);
		}
	}
}

void Control::SetActionInterval(tick_t interval)
{
	repeatDelay = interval;
	if (actionTimer) {
		actionTimer->SetInterval(repeatDelay);
	}
}

bool Control::SupportsAction(const ActionKey& key)
{
	return actions.count(key);
}

bool Control::PerformAction()
{
	return PerformAction(ACTION_DEFAULT);
}

bool Control::PerformAction(const ActionKey& key)
{
	if (IsDisabled()) {
		return false;
	}

	const auto& it = actions.find(key);
	if (it != actions.end()) {
		if (!window) {
			Log(WARNING, "Control", "Executing event handler for a control with no window. This most likely indicates a programming or scripting error.");
		}

		(it->second)(this);
		return true;
	}
	return false;
}

void Control::FlagsChanged(unsigned int /*oldflags*/)
{
	if (actionTimer && (flags & Disabled)) {
		ClearActionTimer();
	}
}

void Control::UpdateState(const varname_t& varname, value_t val)
{
	if (VarName == varname) {
		UpdateState(val);
	}
}

void Control::SetFocus()
{
	window->SetFocused(this);
	MarkDirty();
}

bool Control::IsFocused() const
{
	return window->FocusedView() == this;
}

Control::value_t Control::SetValue(value_t val)
{
	value_t oldVal = Value;
	Value = Clamp(val, range.first, range.second);

	if (oldVal != Value) {
		if (IsDictBound()) {
			UpdateDictValue();
		}
		PerformAction(ValueChange);
		MarkDirty();
	}
	return Value;
}

Control::value_t Control::SetValueRange(ValueRange r)
{
	range = std::move(r);
	if (Value != INVALID_VALUE) {
		return SetValue(Value); // update the value if it falls outside the range
	}
	return INVALID_VALUE;
}

Control::value_t Control::SetValueRange(value_t min, value_t max)
{
	return SetValueRange(ValueRange(min, max));
}

void Control::UpdateDictValue() noexcept
{
	if (!IsDictBound()) {
		return;
	}

	// set this even when the value doesn't change
	// if a radio is clicked, then one of its siblings, the siblings value won't change
	// but we expect the dictionary to reflect the selected value
	auto& vars = core->GetDictionary();
	BitOp op = GetDictOp();
	value_t curVal = op == BitOp::SET ? INVALID_VALUE : 0;
	std::string key { VarName.c_str() };

	auto lookup = vars.Get(key);
	if (lookup != nullptr) {
		curVal = *lookup;
	}

	value_t newVal = curVal;
	SetBits(newVal, Value, op);
	vars.Set(key, newVal);

	const Window* win = GetWindow();
	if (win) {
		win->RedrawControls(VarName);
	} else {
		UpdateState(VarName, newVal);
	}
}

void Control::BindDictVariable(const varname_t& var, value_t val, ValueRange valRange) noexcept
{
	// blank out any old varname so we can set the control value without setting the old variable
	VarName.Reset();
	if (valRange.first != Control::INVALID_VALUE) {
		SetValueRange(valRange);
	}
	SetValue(val);
	// now that the value range is setup, we can change the dictionary variable
	VarName = var;

	if (GetDictOp() == BitOp::SET) {
		// SET implies the dictionary value should always mirror Value
		UpdateDictValue();
	} else {
		auto& vars = core->GetDictionary();
		auto lookup = vars.Get(VarName);
		if (lookup != nullptr) {
			UpdateState(VarName, *lookup);
		}
	}
}

bool Control::IsDictBound() const noexcept
{
	return !VarName.IsEmpty();
}

void Control::ClearActionTimer()
{
	if (actionTimer) {
		actionTimer->Invalidate();
		actionTimer = NULL;
	}
}

void Control::StartActionTimer(const ControlEventHandler& action, tick_t delay)
{
	EventHandler h = [this, action]() {
		// update the timer to use the actual repeatDelay
		SetActionInterval(repeatDelay);

		if (IsDictBound()) {
			SetValue(GetValue());
		}

		return action(this);
	};
	// always start the timer with ActionRepeatDelay
	// this way we have consistent behavior for the initial delay prior to switching to a faster delay
	actionTimer = &core->SetTimer(h, delay ? delay : ActionRepeatDelay);
}

View::UniqueDragOp Control::DragOperation()
{
	if (actionTimer) {
		return nullptr;
	}

	ActionKey key(Action::DragDropCreate);

	if (SupportsAction(key)) {
		// we have to use a timer so that the dragop is set before the callback is called
		EventHandler h = [this, key]() {
			return actions[key](this);
		};

		actionTimer = &core->SetTimer(h, 0, 0);
	}
	return std::make_unique<ControlDragOp>(this);
}

bool Control::AcceptsDragOperation(const DragOp& dop) const
{
	const ControlDragOp* cdop = dynamic_cast<const ControlDragOp*>(&dop);
	if (cdop) {
		assert(cdop->dragView != this);
		// if 2 controls share the same VarName we assume they are swappable...
		return (VarName == cdop->Source()->VarName);
	}

	return View::AcceptsDragOperation(dop);
}

Holder<Sprite2D> Control::DragCursor() const
{
	if (InDebugMode(DebugMode::VIEWS)) {
		return core->Cursors[IE_CURSOR_SWAP];
	}
	return nullptr;
}

bool Control::OnMouseUp(const MouseEvent& me, unsigned short mod)
{
	ControlActionKey key(Click, mod, me.button, me.repeats);
	if (SupportsAction(key)) {
		PerformAction(key);
		ClearActionTimer();
	} else if (me.repeats > 1) {
		// also try a single-click in case there is no doubleclick handler
		// and there is never a triple+ click handler
		MouseEvent me2(me);
		me2.repeats = 1;
		OnMouseUp(me2, mod);
	}
	return true; // always handled
}

bool Control::OnMouseDown(const MouseEvent& me, unsigned short mod)
{
	ControlActionKey key(Click, mod, me.button, me.repeats);
	if (repeatDelay && SupportsAction(key)) {
		StartActionTimer(actions[key]);
	}
	return true; // always handled
}

void Control::OnMouseEnter(const MouseEvent& /*me*/, const DragOp*)
{
	PerformAction(HoverBegin);
}

void Control::OnMouseLeave(const MouseEvent& /*me*/, const DragOp*)
{
	PerformAction(HoverEnd);
}

bool Control::OnTouchDown(const TouchEvent& /*te*/, unsigned short /*mod*/)
{
	ControlEventHandler cb = &Control::HandleTouchActionTimer;
	StartActionTimer(cb, 500); // TODO: this time value should be configurable
	return true; // always handled
}

bool Control::OnTouchUp(const TouchEvent& te, unsigned short mod)
{
	if (actionTimer) {
		// touch up before timer triggered
		// send the touch down+up events
		ClearActionTimer();
		View::OnTouchDown(te, mod);
		View::OnTouchUp(te, mod);
		return true;
	}
	return false; // touch was already handled as a long press
}

bool Control::OnKeyPress(const KeyboardEvent& key, unsigned short mod)
{
	if (key.keycode == GEM_RETURN) {
		return PerformAction();
	}

	return View::OnKeyPress(key, mod);
}

void Control::HandleTouchActionTimer()
{
	assert(actionTimer);

	ClearActionTimer();

	// long press action (GEM_MB_MENU)
	// NOTE: we could save the mod value from OnTouchDown to support modifiers to the touch, but we don't have a use ATM
	ControlActionKey key(Click, 0, GEM_MB_MENU, 1);
	PerformAction(key);
}

ViewScriptingRef* Control::CreateScriptingRef(ScriptingId id, ScriptingGroup_t group)
{
	return new ControlScriptingRef(this, id, group);
}

}
