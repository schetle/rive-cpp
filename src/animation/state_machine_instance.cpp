#include "rive/animation/animation_state_instance.hpp"
#include "rive/animation/animation_state.hpp"
#include "rive/animation/any_state.hpp"
#include "rive/animation/cubic_interpolator.hpp"
#include "rive/animation/entry_state.hpp"
#include "rive/animation/layer_state_flags.hpp"
#include "rive/animation/nested_state_machine.hpp"
#include "rive/animation/state_instance.hpp"
#include "rive/animation/state_machine_bool.hpp"
#include "rive/animation/state_machine_input_instance.hpp"
#include "rive/animation/state_machine_input.hpp"
#include "rive/animation/state_machine_instance.hpp"
#include "rive/animation/state_machine_layer.hpp"
#include "rive/animation/state_machine_listener.hpp"
#include "rive/animation/state_machine_number.hpp"
#include "rive/animation/state_machine_trigger.hpp"
#include "rive/animation/state_machine.hpp"
#include "rive/animation/state_transition.hpp"
#include "rive/animation/transition_condition.hpp"
#include "rive/animation/state_machine_fire_event.hpp"
#include "rive/hit_result.hpp"
#include "rive/math/aabb.hpp"
#include "rive/math/hit_test.hpp"
#include "rive/nested_animation.hpp"
#include "rive/nested_artboard.hpp"
#include "rive/shapes/shape.hpp"
#include "rive/math/math_types.hpp"
#include "rive/audio_event.hpp"
#include <unordered_map>
#include <chrono>

using namespace rive;
namespace rive
{
class StateMachineLayerInstance
{
public:
    ~StateMachineLayerInstance()
    {
        delete m_anyStateInstance;
        delete m_currentState;
        delete m_stateFrom;
    }

    void init(StateMachineInstance* stateMachineInstance,
              const StateMachineLayer* layer,
              ArtboardInstance* instance)
    {
        m_stateMachineInstance = stateMachineInstance;
        m_artboardInstance = instance;
        assert(m_layer == nullptr);
        m_anyStateInstance = layer->anyState()->makeInstance(instance).release();
        m_layer = layer;
        changeState(m_layer->entryState());
        auto now = std::chrono::high_resolution_clock::now();
        auto nanos =
            std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
        srand(nanos);
    }

    void updateMix(float seconds)
    {
        if (m_transition != nullptr && m_stateFrom != nullptr && m_transition->duration() != 0)
        {
            m_mix = std::min(
                1.0f,
                std::max(0.0f, (m_mix + seconds / m_transition->mixTime(m_stateFrom->state()))));
            if (m_mix == 1.0f && !m_transitionCompleted)
            {
                m_transitionCompleted = true;
                fireEvents(StateMachineFireOccurance::atEnd, m_transition->events());
            }
        }
        else
        {
            m_mix = 1.0f;
        }
    }

    bool advance(float seconds)
    {
        m_stateMachineChangedOnAdvance = false;
        m_currentState->advance(seconds, m_stateMachineInstance);
        updateMix(seconds);

        if (m_stateFrom != nullptr && m_mix < 1.0f && !m_holdAnimationFrom)
        {
            // This didn't advance during our updateState, but it should now
            // that we realize we need to mix it in.
            m_stateFrom->advance(seconds, m_stateMachineInstance);
        }

        apply();

        for (int i = 0; updateState(i != 0); i++)
        {
            apply();

            if (i == maxIterations)
            {
                fprintf(stderr, "StateMachine exceeded max iterations.\n");
                return false;
            }
        }

        m_currentState->clearSpilledTime();

        return m_mix != 1.0f || m_waitingForExit ||
               (m_currentState != nullptr && m_currentState->keepGoing());
    }

    bool isTransitioning()
    {
        return m_transition != nullptr && m_stateFrom != nullptr && m_transition->duration() != 0 &&
               m_mix < 1.0f;
    }

    bool updateState(bool ignoreTriggers)
    {
        // Don't allow changing state while a transition is taking place
        // (we're mixing one state onto another) if enableEarlyExit is not true.
        if (isTransitioning() && !m_transition->enableEarlyExit())
        {
            return false;
        }

        m_waitingForExit = false;

        if (tryChangeState(m_anyStateInstance, ignoreTriggers))
        {
            return true;
        }

        return tryChangeState(m_currentState, ignoreTriggers);
    }

    void fireEvents(StateMachineFireOccurance occurs,
                    const std::vector<StateMachineFireEvent*>& fireEvents)
    {
        for (auto event : fireEvents)
        {
            if (event->occurs() == occurs)
            {
                event->perform(m_stateMachineInstance);
            }
        }
    }

    bool canChangeState(const LayerState* stateTo)
    {
        return !((m_currentState == nullptr ? nullptr : m_currentState->state()) == stateTo);
    }

    double randomValue() { return ((double)rand() / (RAND_MAX)); }

    bool changeState(const LayerState* stateTo)
    {
        if ((m_currentState == nullptr ? nullptr : m_currentState->state()) == stateTo)
        {
            return false;
        }

        // Fire end events for the state we're changing from.
        if (m_currentState != nullptr)
        {
            fireEvents(StateMachineFireOccurance::atEnd, m_currentState->state()->events());
        }

        m_currentState =
            stateTo == nullptr ? nullptr : stateTo->makeInstance(m_artboardInstance).release();

        // Fire start events for the state we're changing to.
        if (m_currentState != nullptr)
        {
            fireEvents(StateMachineFireOccurance::atStart, m_currentState->state()->events());
        }
        return true;
    }

    StateTransition* findRandomTransition(StateInstance* stateFromInstance, bool ignoreTriggers)
    {
        uint32_t totalWeight = 0;
        auto stateFrom = stateFromInstance->state();
        // printf("stateFrom->transitionCount(): %zu\n", stateFrom->transitionCount());
        for (size_t i = 0, length = stateFrom->transitionCount(); i < length; i++)
        {
            auto transition = stateFrom->transition(i);
            auto allowed =
                transition->allowed(stateFromInstance, m_stateMachineInstance, ignoreTriggers);
            if (allowed == AllowTransition::yes && canChangeState(transition->stateTo()))
            {
                transition->evaluatedRandomWeight(transition->randomWeight());
                totalWeight += transition->randomWeight();
            }
            else
            {
                transition->evaluatedRandomWeight(0);
                if (allowed == AllowTransition::waitingForExit)
                {
                    m_waitingForExit = true;
                }
            }
        }
        if (totalWeight > 0)
        {
            double randomWeight = randomValue() * totalWeight * 1.0;
            float currentWeight = 0;
            size_t index = 0;
            StateTransition* transition;
            while (index < stateFrom->transitionCount())
            {
                transition = stateFrom->transition(index);
                auto transitionWeight = transition->evaluatedRandomWeight();
                if (currentWeight + transitionWeight > randomWeight)
                {
                    return transition;
                }
                currentWeight += transitionWeight;
                index++;
            }
        }
        return nullptr;
    }

    StateTransition* findAllowedTransition(StateInstance* stateFromInstance, bool ignoreTriggers)
    {
        auto stateFrom = stateFromInstance->state();
        // If it should randomize
        if ((static_cast<LayerStateFlags>(stateFrom->flags()) & LayerStateFlags::Random) ==
            LayerStateFlags::Random)
        {
            return findRandomTransition(stateFromInstance, ignoreTriggers);
        }
        // Else search the first valid transition
        for (size_t i = 0, length = stateFrom->transitionCount(); i < length; i++)
        {
            auto transition = stateFrom->transition(i);
            auto allowed =
                transition->allowed(stateFromInstance, m_stateMachineInstance, ignoreTriggers);
            if (allowed == AllowTransition::yes && canChangeState(transition->stateTo()))
            {
                transition->evaluatedRandomWeight(transition->randomWeight());
                return transition;
            }
            else
            {
                transition->evaluatedRandomWeight(0);
                if (allowed == AllowTransition::waitingForExit)
                {
                    m_waitingForExit = true;
                }
            }
        }
        return nullptr;
    }

    bool tryChangeState(StateInstance* stateFromInstance, bool ignoreTriggers)
    {
        if (stateFromInstance == nullptr)
        {
            return false;
        }
        auto outState = m_currentState;
        auto transition = findAllowedTransition(stateFromInstance, ignoreTriggers);
        if (transition != nullptr)
        {
            changeState(transition->stateTo());
            m_stateMachineChangedOnAdvance = true;
            // state actually has changed
            m_transition = transition;
            fireEvents(StateMachineFireOccurance::atStart, transition->events());
            if (transition->duration() == 0)
            {
                m_transitionCompleted = true;
                fireEvents(StateMachineFireOccurance::atEnd, transition->events());
            }
            else
            {
                m_transitionCompleted = false;
            }

            if (m_stateFrom != m_anyStateInstance)
            {
                // Old state from is done.
                delete m_stateFrom;
            }
            m_stateFrom = outState;

            // If we had an exit time and wanted to pause on exit, make
            // sure to hold the exit time. Delegate this to the
            // transition by telling it that it was completed.
            if (outState != nullptr && transition->applyExitCondition(outState))
            {
                // Make sure we apply this state. This only returns true
                // when it's an animation state instance.
                auto instance =
                    static_cast<AnimationStateInstance*>(m_stateFrom)->animationInstance();

                m_holdAnimation = instance->animation();
                m_holdTime = instance->time();
            }
            m_mixFrom = m_mix;

            // Keep mixing last animation that was mixed in.
            if (m_mix != 0.0f)
            {
                m_holdAnimationFrom = transition->pauseOnExit();
            }
            if (m_stateFrom != nullptr && m_stateFrom->state()->is<AnimationState>() &&
                m_currentState != nullptr)
            {
                auto instance =
                    static_cast<AnimationStateInstance*>(m_stateFrom)->animationInstance();

                auto spilledTime = instance->spilledTime();
                m_currentState->advance(spilledTime, m_stateMachineInstance);
            }
            m_mix = 0.0f;
            updateMix(0.0f);
            m_waitingForExit = false;
            return true;
        }
        return false;
    }

    void apply(/*Artboard* artboard*/)
    {
        if (m_holdAnimation != nullptr)
        {
            m_holdAnimation->apply(m_artboardInstance, m_holdTime, m_mixFrom);
            m_holdAnimation = nullptr;
        }

        CubicInterpolator* cubic = nullptr;
        if (m_transition != nullptr && m_transition->interpolator() != nullptr)
        {
            cubic = m_transition->interpolator();
        }

        if (m_stateFrom != nullptr && m_mix < 1.0f)
        {
            auto fromMix = cubic != nullptr ? cubic->transform(m_mixFrom) : m_mixFrom;
            m_stateFrom->apply(fromMix);
        }
        if (m_currentState != nullptr)
        {
            auto mix = cubic != nullptr ? cubic->transform(m_mix) : m_mix;
            m_currentState->apply(mix);
        }
    }

    bool stateChangedOnAdvance() const { return m_stateMachineChangedOnAdvance; }

    const LayerState* currentState()
    {
        return m_currentState == nullptr ? nullptr : m_currentState->state();
    }

    const LinearAnimationInstance* currentAnimation() const
    {
        if (m_currentState == nullptr || !m_currentState->state()->is<AnimationState>())
        {
            return nullptr;
        }
        return static_cast<AnimationStateInstance*>(m_currentState)->animationInstance();
    }

private:
    static const int maxIterations = 100;
    StateMachineInstance* m_stateMachineInstance = nullptr;
    const StateMachineLayer* m_layer = nullptr;
    ArtboardInstance* m_artboardInstance = nullptr;

    StateInstance* m_anyStateInstance = nullptr;
    StateInstance* m_currentState = nullptr;
    StateInstance* m_stateFrom = nullptr;

    const StateTransition* m_transition = nullptr;
    bool m_transitionCompleted = false;

    bool m_holdAnimationFrom = false;

    float m_mix = 1.0f;
    float m_mixFrom = 1.0f;
    bool m_stateMachineChangedOnAdvance = false;

    bool m_waitingForExit = false;
    /// Used to ensure a specific animation is applied on the next apply.
    const LinearAnimation* m_holdAnimation = nullptr;
    float m_holdTime = 0.0f;
};

class HitComponent
{
public:
    Component* component() const { return m_component; }
    HitComponent(Component* component, StateMachineInstance* stateMachineInstance) :
        m_component(component), m_stateMachineInstance(stateMachineInstance)
    {}
    virtual ~HitComponent() {}
    virtual HitResult processEvent(Vec2D position, ListenerType hitType, bool canHit) = 0;

protected:
    Component* m_component;
    StateMachineInstance* m_stateMachineInstance;
};

/// Representation of a Shape from the Artboard Instance and all the listeners it
/// triggers. Allows tracking hover and performing hit detection only once on
/// shapes that trigger multiple listeners.
class HitShape : public HitComponent
{
public:
    HitShape(Component* shape, StateMachineInstance* stateMachineInstance) :
        HitComponent(shape, stateMachineInstance)
    {}
    bool isHovered = false;
    float hitRadius = 2;
    Vec2D previousPosition;
    std::vector<const StateMachineListener*> listeners;

    bool hitTest(Vec2D position) const
    {

        auto shape = m_component->as<Shape>();
        auto worldBounds = shape->worldBounds();
        if (!worldBounds.contains(position))
        {
            return false;
        }
        auto hitArea = AABB(position.x - hitRadius,
                            position.y - hitRadius,
                            position.x + hitRadius,
                            position.y + hitRadius)
                           .round();
        return shape->hitTest(hitArea);
    }

    HitResult processEvent(Vec2D position, ListenerType hitType, bool canHit) override
    {
        auto shape = m_component->as<Shape>();
        bool isOver = canHit ? hitTest(position) : false;
        bool hoverChange = isHovered != isOver;
        isHovered = isOver;
        if (hoverChange && isHovered)
        {
            previousPosition.x = position.x;
            previousPosition.y = position.y;
        }

        // // iterate all listeners associated with this hit shape
        for (auto listener : listeners)
        {
            // Always update hover states regardless of which specific listener type
            // we're trying to trigger.
            if (hoverChange)
            {
                if (isOver && listener->listenerType() == ListenerType::enter)
                {
                    listener->performChanges(m_stateMachineInstance, position, previousPosition);
                    m_stateMachineInstance->markNeedsAdvance();
                }
                else if (!isOver && listener->listenerType() == ListenerType::exit)
                {
                    listener->performChanges(m_stateMachineInstance, position, previousPosition);
                    m_stateMachineInstance->markNeedsAdvance();
                }
            }
            if (isOver && hitType == listener->listenerType())
            {
                listener->performChanges(m_stateMachineInstance, position, previousPosition);
                m_stateMachineInstance->markNeedsAdvance();
            }
        }
        previousPosition.x = position.x;
        previousPosition.y = position.y;
        return isOver ? shape->isTargetOpaque() ? HitResult::hitOpaque : HitResult::hit
                      : HitResult::none;
    }
};
class HitNestedArtboard : public HitComponent
{
public:
    HitNestedArtboard(Component* nestedArtboard, StateMachineInstance* stateMachineInstance) :
        HitComponent(nestedArtboard, stateMachineInstance)
    {}
    ~HitNestedArtboard() override {}
    HitResult processEvent(Vec2D position, ListenerType hitType, bool canHit) override
    {
        auto nestedArtboard = m_component->as<NestedArtboard>();
        HitResult hitResult = HitResult::none;
        if (nestedArtboard->isCollapsed())
        {
            return hitResult;
        }
        Vec2D nestedPosition;
        if (!nestedArtboard->worldToLocal(position, &nestedPosition))
        {
            // Mounted artboard isn't ready or has a 0 scale transform.
            return hitResult;
        }

        for (auto nestedAnimation : nestedArtboard->nestedAnimations())
        {
            if (nestedAnimation->is<NestedStateMachine>())
            {
                auto nestedStateMachine = nestedAnimation->as<NestedStateMachine>();
                if (canHit)
                {
                    switch (hitType)
                    {
                        case ListenerType::down:
                            hitResult = nestedStateMachine->pointerDown(nestedPosition);
                            break;
                        case ListenerType::up:
                            hitResult = nestedStateMachine->pointerUp(nestedPosition);
                            break;
                        case ListenerType::move:
                            hitResult = nestedStateMachine->pointerMove(nestedPosition);
                            break;
                        case ListenerType::enter:
                        case ListenerType::exit:
                        case ListenerType::event:
                            break;
                    }
                }
                else
                {
                    switch (hitType)
                    {
                        case ListenerType::down:
                        case ListenerType::up:
                        case ListenerType::move:
                            nestedStateMachine->pointerExit(nestedPosition);
                            break;
                        case ListenerType::enter:
                        case ListenerType::exit:
                        case ListenerType::event:
                            break;
                    }
                }
            }
        }
        return hitResult;
    }
};
} // namespace rive

HitResult StateMachineInstance::updateListeners(Vec2D position, ListenerType hitType)
{
    if (m_artboardInstance->frameOrigin())
    {
        position -= Vec2D(m_artboardInstance->originX() * m_artboardInstance->width(),
                          m_artboardInstance->originY() * m_artboardInstance->height());
    }

    bool hitSomething = false;
    bool hitOpaque = false;
    for (const auto& hitShape : m_hitComponents)
    {

        // TODO: quick reject.

        HitResult hitResult = hitShape->processEvent(position, hitType, !hitOpaque);
        if (hitResult != HitResult::none)
        {
            hitSomething = true;
            if (hitResult == HitResult::hitOpaque)
            {
                hitOpaque = true;
            }
        }
    }
    return hitSomething ? hitOpaque ? HitResult::hitOpaque : HitResult::hit : HitResult::none;
}

HitResult StateMachineInstance::pointerMove(Vec2D position)
{
    return updateListeners(position, ListenerType::move);
}
HitResult StateMachineInstance::pointerDown(Vec2D position)
{
    return updateListeners(position, ListenerType::down);
}
HitResult StateMachineInstance::pointerUp(Vec2D position)
{
    return updateListeners(position, ListenerType::up);
}
HitResult StateMachineInstance::pointerExit(Vec2D position)
{
    return updateListeners(position, ListenerType::exit);
}

StateMachineInstance::StateMachineInstance(const StateMachine* machine,
                                           ArtboardInstance* instance) :
    Scene(instance), m_machine(machine)
{
    const auto count = machine->inputCount();
    m_inputInstances.resize(count);
    for (size_t i = 0; i < count; i++)
    {
        auto input = machine->input(i);
        if (input == nullptr)
        {
            continue;
        }
        switch (input->coreType())
        {
            case StateMachineBool::typeKey:
                m_inputInstances[i] = new SMIBool(input->as<StateMachineBool>(), this);
                break;
            case StateMachineNumber::typeKey:
                m_inputInstances[i] = new SMINumber(input->as<StateMachineNumber>(), this);
                break;
            case StateMachineTrigger::typeKey:
                m_inputInstances[i] = new SMITrigger(input->as<StateMachineTrigger>(), this);
                break;
            default:
                // Sanity check.
                break;
        }
    }

    m_layerCount = machine->layerCount();
    m_layers = new StateMachineLayerInstance[m_layerCount];
    for (size_t i = 0; i < m_layerCount; i++)
    {
        m_layers[i].init(this, machine->layer(i), m_artboardInstance);
    }

    // Initialize listeners. Store a lookup table of shape id to hit shape
    // representation (an object that stores all the listeners triggered by the
    // shape producing a listener).
    std::unordered_map<uint32_t, HitShape*> hitShapeLookup;
    for (std::size_t i = 0; i < machine->listenerCount(); i++)
    {
        auto listener = machine->listener(i);

        // Iterate actual leaf hittable shapes tied to this listener and resolve
        // corresponding ones in the artboard instance.
        for (auto id : listener->hitShapeIds())
        {
            HitShape* hitShape;
            auto itr = hitShapeLookup.find(id);
            if (itr == hitShapeLookup.end())
            {
                auto shape = m_artboardInstance->resolve(id);
                if (shape != nullptr && shape->is<Shape>())
                {
                    auto hs = rivestd::make_unique<HitShape>(shape->as<Component>(), this);
                    hitShapeLookup[id] = hitShape = hs.get();
                    m_hitComponents.push_back(std::move(hs));
                }
                else
                {
                    // No object or not a shape...
                    continue;
                }
            }
            else
            {
                hitShape = itr->second;
            }
            hitShape->listeners.push_back(listener);
        }
    }

    for (auto nestedArtboard : instance->nestedArtboards())
    {
        if (nestedArtboard->hasNestedStateMachines())
        {

            auto hn =
                rivestd::make_unique<HitNestedArtboard>(nestedArtboard->as<Component>(), this);
            m_hitComponents.push_back(std::move(hn));

            for (auto animation : nestedArtboard->nestedAnimations())
            {
                if (animation->is<NestedStateMachine>())
                {
                    animation->as<NestedStateMachine>()
                        ->stateMachineInstance()
                        ->setParentNestedArtboard(nestedArtboard);
                    animation->as<NestedStateMachine>()
                        ->stateMachineInstance()
                        ->setParentStateMachineInstance(this);
                }
            }
        }
    }
    sortHitComponents();
}

StateMachineInstance::~StateMachineInstance()
{
    for (auto inst : m_inputInstances)
    {
        delete inst;
    }
    delete[] m_layers;
}

void StateMachineInstance::sortHitComponents()
{
    Drawable* last = m_artboardInstance->firstDrawable();
    if (last)
    {
        // walk to the end, so we can visit in reverse-order
        while (last->prev)
        {
            last = last->prev;
        }
    }
    auto hitShapesCount = m_hitComponents.size();
    auto currentSortedIndex = 0;
    for (auto drawable = last; drawable; drawable = drawable->next)
    {
        for (size_t i = currentSortedIndex; i < hitShapesCount; i++)
        {
            if (m_hitComponents[i]->component() == drawable)
            {
                if (currentSortedIndex != i)
                {
                    std::iter_swap(m_hitComponents.begin() + currentSortedIndex,
                                   m_hitComponents.begin() + i);
                }
                currentSortedIndex++;
                break;
            }
        }
        if (currentSortedIndex == hitShapesCount)
        {
            break;
        }
    }
}

bool StateMachineInstance::advance(float seconds)
{
    if (m_artboardInstance->hasChangedDrawOrderInLastUpdate())
    {
        sortHitComponents();
    }
    this->notifyEventListeners(m_reportedEvents, nullptr);
    m_reportedEvents.clear();
    m_needsAdvance = false;
    for (size_t i = 0; i < m_layerCount; i++)
    {
        if (m_layers[i].advance(seconds))
        {
            m_needsAdvance = true;
        }
    }

    for (auto inst : m_inputInstances)
    {
        inst->advanced();
    }

    return m_needsAdvance;
}

bool StateMachineInstance::advanceAndApply(float seconds)
{
    bool keepGoing = this->advance(seconds);
    keepGoing = m_artboardInstance->advance(seconds) || keepGoing;
    return keepGoing;
}

void StateMachineInstance::markNeedsAdvance() { m_needsAdvance = true; }
bool StateMachineInstance::needsAdvance() const { return m_needsAdvance; }

std::string StateMachineInstance::name() const { return m_machine->name(); }

SMIInput* StateMachineInstance::input(size_t index) const
{
    if (index < m_inputInstances.size())
    {
        return m_inputInstances[index];
    }
    return nullptr;
}

template <typename SMType, typename InstType>
InstType* StateMachineInstance::getNamedInput(const std::string& name) const
{
    for (const auto inst : m_inputInstances)
    {
        auto input = inst->input();
        if (input->is<SMType>() && input->name() == name)
        {
            return static_cast<InstType*>(inst);
        }
    }
    return nullptr;
}

SMIBool* StateMachineInstance::getBool(const std::string& name) const
{
    return getNamedInput<StateMachineBool, SMIBool>(name);
}
SMINumber* StateMachineInstance::getNumber(const std::string& name) const
{
    return getNamedInput<StateMachineNumber, SMINumber>(name);
}
SMITrigger* StateMachineInstance::getTrigger(const std::string& name) const
{
    return getNamedInput<StateMachineTrigger, SMITrigger>(name);
}

size_t StateMachineInstance::stateChangedCount() const
{
    size_t count = 0;
    for (size_t i = 0; i < m_layerCount; i++)
    {
        if (m_layers[i].stateChangedOnAdvance())
        {
            count++;
        }
    }
    return count;
}

const LayerState* StateMachineInstance::stateChangedByIndex(size_t index) const
{
    size_t count = 0;
    for (size_t i = 0; i < m_layerCount; i++)
    {
        if (m_layers[i].stateChangedOnAdvance())
        {
            if (count == index)
            {
                return m_layers[i].currentState();
            }
            count++;
        }
    }
    return nullptr;
}

size_t StateMachineInstance::currentAnimationCount() const
{
    size_t count = 0;
    for (size_t i = 0; i < m_layerCount; i++)
    {
        if (m_layers[i].currentAnimation() != nullptr)
        {
            count++;
        }
    }
    return count;
}

const LinearAnimationInstance* StateMachineInstance::currentAnimationByIndex(size_t index) const
{
    size_t count = 0;
    for (size_t i = 0; i < m_layerCount; i++)
    {
        if (m_layers[i].currentAnimation() != nullptr)
        {
            if (count == index)
            {
                return m_layers[i].currentAnimation();
            }
            count++;
        }
    }
    return nullptr;
}

void StateMachineInstance::reportEvent(Event* event, float delaySeconds)
{
    m_reportedEvents.push_back(EventReport(event, delaySeconds));
}

std::size_t StateMachineInstance::reportedEventCount() const { return m_reportedEvents.size(); }

const EventReport StateMachineInstance::reportedEventAt(std::size_t index) const
{
    if (index >= m_reportedEvents.size())
    {
        return EventReport(nullptr, 0.0f);
    }
    return m_reportedEvents[index];
}

void StateMachineInstance::notifyEventListeners(const std::vector<EventReport>& events,
                                                NestedArtboard* source)
{
    if (events.size() > 0)
    {
        // We trigger the listeners in order
        for (size_t i = 0; i < m_machine->listenerCount(); i++)
        {
            auto listener = m_machine->listener(i);
            auto target = artboard()->resolve(listener->targetId());
            if (listener != nullptr && listener->listenerType() == ListenerType::event &&
                (source == nullptr || source == target))
            {
                for (const auto event : events)
                {
                    auto sourceArtboard = source == nullptr ? artboard() : source->artboard();

                    // listener->eventId() can point to an id from an event in the context of this
                    // artboard or the context of a nested artboard. Because those ids belong to
                    // different contexts, they can have the same value. So when the eventId is
                    // resolved within one context, but actually pointing to the other, it can
                    // return the wrong event object. If, by chance, that event exists in the other
                    // context, and is being reported, it will trigger the wrong set of actions.
                    // This validation makes sure that a listener must be targetting the current
                    // artboard to disambiguate between external and internal events.
                    if (source == nullptr &&
                        sourceArtboard->resolve(listener->targetId()) != artboard())
                    {
                        continue;
                    }
                    auto listenerEvent = sourceArtboard->resolve(listener->eventId());
                    if (listenerEvent == event.event())
                    {
                        listener->performChanges(this, Vec2D(), Vec2D());
                        break;
                    }
                }
            }
        }
        // Bubble the event up to parent artboard state machines immediately
        if (m_parentStateMachineInstance != nullptr)
        {
            m_parentStateMachineInstance->notifyEventListeners(events, m_parentNestedArtboard);
        }

        for (auto report : events)
        {
            auto event = report.event();
            if (event->is<AudioEvent>())
            {
                event->as<AudioEvent>()->play();
            }
        }
    }
}