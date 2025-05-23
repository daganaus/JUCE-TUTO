/*
  ==============================================================================

   This file is part of the JUCE framework.
   Copyright (c) Raw Material Software Limited

   JUCE is an open source framework subject to commercial or open source
   licensing.

   By downloading, installing, or using the JUCE framework, or combining the
   JUCE framework with any other source code, object code, content or any other
   copyrightable work, you agree to the terms of the JUCE End User Licence
   Agreement, and all incorporated terms including the JUCE Privacy Policy and
   the JUCE Website Terms of Service, as applicable, which will bind you. If you
   do not agree to the terms of these agreements, we will not license the JUCE
   framework to you, and you must discontinue the installation or download
   process and cease use of the JUCE framework.

   JUCE End User Licence Agreement: https://juce.com/legal/juce-8-licence/
   JUCE Privacy Policy: https://juce.com/juce-privacy-policy
   JUCE Website Terms of Service: https://juce.com/juce-website-terms-of-service/

   Or:

   You may also use this code under the terms of the AGPLv3:
   https://www.gnu.org/licenses/agpl-3.0.en.html

   THE JUCE FRAMEWORK IS PROVIDED "AS IS" WITHOUT ANY WARRANTY, AND ALL
   WARRANTIES, WHETHER EXPRESSED OR IMPLIED, INCLUDING WARRANTY OF
   MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE, ARE DISCLAIMED.

  ==============================================================================
*/

namespace juce
{

CallOutBox::CallOutBox (Component& c, Rectangle<int> area, Component* const parent)
    : content (c)
{
    addAndMakeVisible (content);

    if (parent != nullptr)
    {
        parent->addChildComponent (this);
        updatePosition (area, parent->getLocalBounds());
        setVisible (true);
    }
    else
    {
        setAlwaysOnTop (WindowUtils::areThereAnyAlwaysOnTopWindows());
        updatePosition (area, Desktop::getInstance().getDisplays().getDisplayForRect (area)->userArea);
        addToDesktop (ComponentPeer::windowIsTemporary);

        startTimer (100);
    }

    creationTime = Time::getCurrentTime();
}

//==============================================================================
class CallOutBoxCallback final : public ModalComponentManager::Callback,
                                 private Timer
{
public:
    CallOutBoxCallback (std::unique_ptr<Component> c, const Rectangle<int>& area, Component* parent)
        : content (std::move (c)),
          callout (*content, area, parent)
    {
        callout.setVisible (true);
        callout.enterModalState (true, this);
        startTimer (200);
    }

    void modalStateFinished (int) override {}

    void timerCallback() override
    {
        if (! detail::WindowingHelpers::isForegroundOrEmbeddedProcess (&callout))
            callout.dismiss();
    }

    std::unique_ptr<Component> content;
    CallOutBox callout;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CallOutBoxCallback)
};

CallOutBox& CallOutBox::launchAsynchronously (std::unique_ptr<Component> content, Rectangle<int> area, Component* parent)
{
    jassert (content != nullptr); // must be a valid content component!

    return (new CallOutBoxCallback (std::move (content), area, parent))->callout;
}

//==============================================================================
void CallOutBox::setArrowSize (const float newSize)
{
    arrowSize = newSize;
    refreshPath();
}

int CallOutBox::getBorderSize() const noexcept
{
    return jmax (getLookAndFeel().getCallOutBoxBorderSize (*this), (int) arrowSize);
}

void CallOutBox::lookAndFeelChanged()
{
    resized();
}

void CallOutBox::paint (Graphics& g)
{
    getLookAndFeel().drawCallOutBoxBackground (*this, g, outline, background);
}

void CallOutBox::resized()
{
    auto borderSpace = getBorderSize();
    content.setTopLeftPosition (borderSpace, borderSpace);
    refreshPath();
}

void CallOutBox::moved()
{
    refreshPath();
}

void CallOutBox::childBoundsChanged (Component*)
{
    updatePosition (targetArea, availableArea);
}

bool CallOutBox::hitTest (int x, int y)
{
    return outline.contains ((float) x, (float) y);
}

void CallOutBox::inputAttemptWhenModal()
{
    if (dismissalMouseClicksAreAlwaysConsumed
         || targetArea.contains (getMouseXYRelative() + getBounds().getPosition()))
    {
        // if you click on the area that originally popped-up the callout, you expect it
        // to get rid of the box, but deleting the box here allows the click to pass through and
        // probably re-trigger it, so we need to dismiss the box asynchronously to consume the click..

        // For touchscreens, we make sure not to dismiss the CallOutBox immediately,
        // as Windows still sends touch events before the CallOutBox had a chance
        // to really open.

        auto elapsed = Time::getCurrentTime() - creationTime;

        if (elapsed.inMilliseconds() > 200)
            dismiss();
    }
    else
    {
        exitModalState (0);
        setVisible (false);
    }
}

void CallOutBox::setDismissalMouseClicksAreAlwaysConsumed (bool b) noexcept
{
    dismissalMouseClicksAreAlwaysConsumed = b;
}

static constexpr int callOutBoxDismissCommandId = 0x4f83a04b;

void CallOutBox::handleCommandMessage (int commandId)
{
    Component::handleCommandMessage (commandId);

    if (commandId == callOutBoxDismissCommandId)
    {
        exitModalState (0);
        setVisible (false);
    }
}

void CallOutBox::dismiss()
{
    postCommandMessage (callOutBoxDismissCommandId);
}

bool CallOutBox::keyPressed (const KeyPress& key)
{
    if (key.isKeyCode (KeyPress::escapeKey))
    {
        inputAttemptWhenModal();
        return true;
    }

    return false;
}

void CallOutBox::updatePosition (const Rectangle<int>& newAreaToPointTo, const Rectangle<int>& newAreaToFitIn)
{
    targetArea = newAreaToPointTo;
    availableArea = newAreaToFitIn;

    auto borderSpace = getBorderSize();
    auto newBounds = getLocalArea (&content, Rectangle<int> (content.getWidth()  + borderSpace * 2,
                                                             content.getHeight() + borderSpace * 2));

    auto hw = newBounds.getWidth() / 2;
    auto hh = newBounds.getHeight() / 2;
    auto hwReduced = (float) (hw - borderSpace * 2);
    auto hhReduced = (float) (hh - borderSpace * 2);
    auto arrowIndent = (float) borderSpace - arrowSize;

    Point<float> targets[4] = { { (float) targetArea.getCentreX(), (float) targetArea.getBottom() },
                                { (float) targetArea.getRight(),   (float) targetArea.getCentreY() },
                                { (float) targetArea.getX(),       (float) targetArea.getCentreY() },
                                { (float) targetArea.getCentreX(), (float) targetArea.getY() } };

    Line<float> lines[4] = { { targets[0].translated (-hwReduced, hh - arrowIndent),    targets[0].translated (hwReduced, hh - arrowIndent) },
                             { targets[1].translated (hw - arrowIndent, -hhReduced),    targets[1].translated (hw - arrowIndent, hhReduced) },
                             { targets[2].translated (-(hw - arrowIndent), -hhReduced), targets[2].translated (-(hw - arrowIndent), hhReduced) },
                             { targets[3].translated (-hwReduced, -(hh - arrowIndent)), targets[3].translated (hwReduced, -(hh - arrowIndent)) } };

    auto centrePointArea = newAreaToFitIn.reduced (hw, hh).toFloat();
    auto targetCentre = targetArea.getCentre().toFloat();

    float nearest = 1.0e9f;

    for (int i = 0; i < 4; ++i)
    {
        Line<float> constrainedLine (centrePointArea.getConstrainedPoint (lines[i].getStart()),
                                     centrePointArea.getConstrainedPoint (lines[i].getEnd()));

        auto centre = constrainedLine.findNearestPointTo (targetCentre);
        auto distanceFromCentre = centre.getDistanceFrom (targets[i]);

        if (! centrePointArea.intersects (lines[i]))
            distanceFromCentre += 1000.0f;

        if (distanceFromCentre < nearest)
        {
            nearest = distanceFromCentre;
            targetPoint = targets[i];

            newBounds.setPosition ((int) (centre.x - (float) hw),
                                   (int) (centre.y - (float) hh));
        }
    }

    setBounds (newBounds);
}

void CallOutBox::refreshPath()
{
    repaint();
    background = {};
    outline.clear();

    const float gap = 4.5f;

    outline.addBubble (getLocalArea (&content, content.getLocalBounds().toFloat()).expanded (gap, gap),
                       getLocalBounds().toFloat(),
                       targetPoint - getPosition().toFloat(),
                       getLookAndFeel().getCallOutBoxCornerSize (*this), arrowSize * 0.7f);
}

void CallOutBox::timerCallback()
{
    toFront (true);
    stopTimer();
}

//==============================================================================
std::unique_ptr<AccessibilityHandler> CallOutBox::createAccessibilityHandler()
{
    return std::make_unique<AccessibilityHandler> (*this, AccessibilityRole::dialogWindow);
}

} // namespace juce
