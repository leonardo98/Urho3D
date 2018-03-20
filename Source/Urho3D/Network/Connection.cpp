//
// Copyright (c) 2008-2017 the Urho3D project.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
//

#include "../Precompiled.h"

#include "../Core/Profiler.h"
#include "../IO/File.h"
#include "../IO/FileSystem.h"
#include "../IO/Log.h"
#include "../IO/MemoryBuffer.h"
#include "../Network/Connection.h"
#include "../Network/Network.h"
#include "../Network/NetworkEvents.h"
#include "../Network/NetworkPriority.h"
#include "../Network/Protocol.h"
#include "../Resource/ResourceCache.h"
#include "../Scene/Scene.h"
#include "../Scene/SceneEvents.h"
#include "../Scene/SmoothedTransform.h"

#include <kNet/kNet.h>

#include "../DebugNew.h"

namespace Urho3D
{

static const int STATS_INTERVAL_MSEC = 2000;
static const int NUMBER_NODES_TO_PROCESS = 16;

Connection::Connection(Context* context, bool isClient) :
    Object(context),
    timeStamp_(0),
    sendMode_(OPSM_NONE),
    isClient_(isClient),
    connectPending_(false),
    sceneLoaded_(false),
    logStatistics_(false)
{
    sceneState_.connection_ = this;
}

Connection::~Connection()
{
    // Reset scene (remove possible owner references), as this connection is about to be destroyed
    SetScene(0);
}

void Connection::SendMessage(int msgID, bool reliable, bool inOrder, const VectorBuffer& msg, unsigned contentID)
{
    Send(msgID, reliable, inOrder, msg, contentID);
}

void Connection::SendRemoteEvent(StringHash eventType, bool inOrder, const VariantMap& eventData)
{
    RemoteEvent queuedEvent;
    queuedEvent.senderID_ = 0;
    queuedEvent.eventType_ = eventType;
    queuedEvent.eventData_ = eventData;
    queuedEvent.inOrder_ = inOrder;
    remoteEvents_.Push(queuedEvent);
}

void Connection::SendRemoteEvent(Node* node, StringHash eventType, bool inOrder, const VariantMap& eventData)
{
    if (!node)
    {
        URHO3D_LOGERROR("Null sender node for remote node event");
        return;
    }
    if (node->GetScene() != scene_)
    {
        URHO3D_LOGERROR("Sender node is not in the connection's scene, can not send remote node event");
        return;
    }
    if (node->GetID() >= FIRST_LOCAL_ID)
    {
        URHO3D_LOGERROR("Sender node has a local ID, can not send remote node event");
        return;
    }

    RemoteEvent queuedEvent;
    queuedEvent.senderID_ = node->GetID();
    queuedEvent.eventType_ = eventType;
    queuedEvent.eventData_ = eventData;
    queuedEvent.inOrder_ = inOrder;
    remoteEvents_.Push(queuedEvent);
}

void Connection::SetScene(Scene* newScene)
{
    if (scene_)
    {
        // Remove replication states and owner references from the previous scene
        scene_->CleanupConnection(this);
    }

    scene_ = newScene;

    if (!scene_)
        return;

    sceneLoaded_ = true;

    if (isClient_)
    {
        sceneState_.Clear();
        msg_.Clear();
        SendMessage(MSG_LOADSCENE, true, true, msg_);
    }
}

void Connection::SetIdentity(const VariantMap& identity)
{
    identity_ = identity;
}

void Connection::SetControls(const Controls& newControls)
{
    controls_ = newControls;
}

void Connection::SetPosition(const Vector3& position)
{
    position_ = position;
    if (sendMode_ == OPSM_NONE)
        sendMode_ = OPSM_POSITION;
}

void Connection::SetRotation(const Quaternion& rotation)
{
    rotation_ = rotation;
    if (sendMode_ != OPSM_POSITION_ROTATION)
        sendMode_ = OPSM_POSITION_ROTATION;
}

void Connection::SetConnectPending(bool connectPending)
{
    connectPending_ = connectPending;
}

void Connection::SetLogStatistics(bool enable)
{
    logStatistics_ = enable;
}

void Connection::SendServerUpdate()
{
    if (!scene_ || !sceneLoaded_)
        return;

    // Always check the root node (scene) first so that the scene-wide components get sent first,
    // and all other replicated nodes get added to the dirty set for sending the initial state
    unsigned sceneID = scene_->GetID();
    nodesToProcess_.Insert(sceneID);
    ProcessNode(sceneID);

    // Then go through all dirtied nodes
    nodesToProcess_.Insert(sceneState_.dirtyNodes_);
    nodesToProcess_.Erase(sceneID); // Do not process the root node twice

    int counter = 0;
    while (nodesToProcess_.Size() && counter < NUMBER_NODES_TO_PROCESS)
    {
        unsigned nodeID = nodesToProcess_.Front();
        ProcessNode(nodeID);
        ++counter;
    }
}

void Connection::SendClientUpdate()
{
    if (!scene_ || !sceneLoaded_)
        return;

    msg_.Clear();
    msg_.WriteUInt(controls_.buttons_);
    msg_.WriteFloat(controls_.yaw_);
    msg_.WriteFloat(controls_.pitch_);
    msg_.WriteVariantMap(controls_.extraData_);
    msg_.WriteUByte(timeStamp_);
    if (sendMode_ >= OPSM_POSITION)
        msg_.WriteVector3(position_);
    if (sendMode_ >= OPSM_POSITION_ROTATION)
        msg_.WritePackedQuaternion(rotation_);
    SendMessage(MSG_CONTROLS, false, false, msg_, CONTROLS_CONTENT_ID);
    controls_.Reset();
    ++timeStamp_;
}

void Connection::SendRemoteEvents()
{
    if (remoteEvents_.Empty())
        return;

    URHO3D_PROFILE(SendRemoteEvents);

    for (Vector<RemoteEvent>::ConstIterator i = remoteEvents_.Begin(); i != remoteEvents_.End(); ++i)
    {
        msg_.Clear();
        if (!i->senderID_)
        {
            msg_.WriteStringHash(i->eventType_);
            msg_.WriteVariantMap(i->eventData_);
            SendMessage(MSG_REMOTEEVENT, true, i->inOrder_, msg_);
        }
        else
        {
            msg_.WriteNetID(i->senderID_);
            msg_.WriteStringHash(i->eventType_);
            msg_.WriteVariantMap(i->eventData_);
            SendMessage(MSG_REMOTENODEEVENT, true, i->inOrder_, msg_);
        }
    }

    remoteEvents_.Clear();
}


void Connection::ProcessPendingLatestData()
{
    if (!scene_ || !sceneLoaded_)
        return;

    // Iterate through pending node data and see if we can find the nodes now
    for (HashMap<unsigned, PODVector<unsigned char> >::Iterator i = nodeLatestData_.Begin(); i != nodeLatestData_.End();)
    {
        HashMap<unsigned, PODVector<unsigned char> >::Iterator current = i++;
        Node* node = scene_->GetNode(current->first_);
        if (node)
        {
            MemoryBuffer msg(current->second_);
            msg.ReadNetID(); // Skip the node ID
            node->ReadLatestDataUpdate(msg);
            // ApplyAttributes() is deliberately skipped, as Node has no attributes that require late applying.
            // Furthermore it would propagate to components and child nodes, which is not desired in this case
            nodeLatestData_.Erase(current);
        }
    }

    // Iterate through pending component data and see if we can find the components now
    for (HashMap<unsigned, PODVector<unsigned char> >::Iterator i = componentLatestData_.Begin(); i != componentLatestData_.End();)
    {
        HashMap<unsigned, PODVector<unsigned char> >::Iterator current = i++;
        Component* component = scene_->GetComponent(current->first_);
        if (component)
        {
            MemoryBuffer msg(current->second_);
            msg.ReadNetID(); // Skip the component ID
            if (component->ReadLatestDataUpdate(msg))
                component->ApplyAttributes();
            componentLatestData_.Erase(current);
        }
    }
}

bool Connection::ProcessMessage(int msgID, MemoryBuffer& msg)
{
    bool processed = true;

    switch (msgID)
    {
    case MSG_IDENTITY:
        ProcessIdentity(msgID, msg);
        break;

    case MSG_CONTROLS:
        ProcessControls(msgID, msg);
        break;

    case MSG_LOADSCENE:
        ProcessLoadScene(msgID, msg);
        break;


    case MSG_CREATENODE:
    case MSG_NODEDELTAUPDATE:
    case MSG_NODELATESTDATA:
    case MSG_REMOVENODE:
    case MSG_CREATECOMPONENT:
    case MSG_COMPONENTDELTAUPDATE:
    case MSG_COMPONENTLATESTDATA:
    case MSG_REMOVECOMPONENT:
        ProcessSceneUpdate(msgID, msg);
        break;

    case MSG_REMOTEEVENT:
    case MSG_REMOTENODEEVENT:
        ProcessRemoteEvent(msgID, msg);
        break;

    default:
        processed = false;
        break;
    }

    return processed;
}

void Connection::ProcessLoadScene(int msgID, MemoryBuffer& msg)
{
    if (IsClient())
    {
        URHO3D_LOGWARNING("Received unexpected LoadScene message from client");
        return;
    }

    if (!scene_)
    {
        URHO3D_LOGERROR("Can not handle LoadScene message without an assigned scene");
        return;
    }

    // Clear previous pending latest data and package downloads if any
    nodeLatestData_.Clear();
    componentLatestData_.Clear();

    sceneLoaded_ = true;
}

void Connection::ProcessSceneUpdate(int msgID, MemoryBuffer& msg)
{
    /// \todo On mobile devices processing this message may potentially cause a crash if it attempts to load new GPU resources
    /// while the application is minimized
    if (IsClient())
    {
        URHO3D_LOGWARNING("Received unexpected SceneUpdate message from client ");
        return;
    }

    if (!scene_)
        return;

    switch (msgID)
    {
    case MSG_CREATENODE:
        {
            unsigned nodeID = msg.ReadNetID();
            // In case of the root node (scene), it should already exist. Do not create in that case
            Node* node = scene_->GetNode(nodeID);
            if (!node)
            {
                // Add initially to the root level. May be moved as we receive the parent attribute
                node = scene_->CreateChild(nodeID, REPLICATED);
                // Create smoothed transform component
                node->CreateComponent<SmoothedTransform>(LOCAL);
            }

            // Read initial attributes, then snap the motion smoothing immediately to the end
            node->ReadDeltaUpdate(msg);
            SmoothedTransform* transform = node->GetComponent<SmoothedTransform>();
            if (transform)
                transform->Update(1.0f, 0.0f);

            // Read initial user variables
            unsigned numVars = msg.ReadVLE();
            while (numVars)
            {
                StringHash key = msg.ReadStringHash();
                node->SetVar(key, msg.ReadVariant());
                --numVars;
            }

            // Read components
            unsigned numComponents = msg.ReadVLE();
            while (numComponents)
            {
                --numComponents;

                StringHash type = msg.ReadStringHash();
                unsigned componentID = msg.ReadNetID();

                // Check if the component by this ID and type already exists in this node
                Component* component = scene_->GetComponent(componentID);
                if (!component || component->GetType() != type || component->GetNode() != node)
                {
                    if (component)
                        component->Remove();
                    component = node->CreateComponent(type, REPLICATED, componentID);
                }

                // If was unable to create the component, would desync the message and therefore have to abort
                if (!component)
                {
                    URHO3D_LOGERROR("CreateNode message parsing aborted due to unknown component");
                    return;
                }

                // Read initial attributes and apply
                component->ReadDeltaUpdate(msg);
                component->ApplyAttributes();
            }
        }
        break;

    case MSG_NODEDELTAUPDATE:
        {
            unsigned nodeID = msg.ReadNetID();
            Node* node = scene_->GetNode(nodeID);
            if (node)
            {
                node->ReadDeltaUpdate(msg);
                // ApplyAttributes() is deliberately skipped, as Node has no attributes that require late applying.
                // Furthermore it would propagate to components and child nodes, which is not desired in this case
                unsigned changedVars = msg.ReadVLE();
                while (changedVars)
                {
                    StringHash key = msg.ReadStringHash();
                    node->SetVar(key, msg.ReadVariant());
                    --changedVars;
                }
            }
            else
                URHO3D_LOGWARNING("NodeDeltaUpdate message received for missing node " + String(nodeID));
        }
        break;

    case MSG_NODELATESTDATA:
        {
            unsigned nodeID = msg.ReadNetID();
            Node* node = scene_->GetNode(nodeID);
            if (node)
            {
                node->ReadLatestDataUpdate(msg);
                // ApplyAttributes() is deliberately skipped, as Node has no attributes that require late applying.
                // Furthermore it would propagate to components and child nodes, which is not desired in this case
            }
            else
            {
                // Latest data messages may be received out-of-order relative to node creation, so cache if necessary
                PODVector<unsigned char>& data = nodeLatestData_[nodeID];
                data.Resize(msg.GetSize());
                memcpy(&data[0], msg.GetData(), msg.GetSize());
            }
        }
        break;

    case MSG_REMOVENODE:
        {
            unsigned nodeID = msg.ReadNetID();
            Node* node = scene_->GetNode(nodeID);
            if (node)
                node->Remove();
            nodeLatestData_.Erase(nodeID);
        }
        break;

    case MSG_CREATECOMPONENT:
        {
            unsigned nodeID = msg.ReadNetID();
            Node* node = scene_->GetNode(nodeID);
            if (node)
            {
                StringHash type = msg.ReadStringHash();
                unsigned componentID = msg.ReadNetID();

                // Check if the component by this ID and type already exists in this node
                Component* component = scene_->GetComponent(componentID);
                if (!component || component->GetType() != type || component->GetNode() != node)
                {
                    if (component)
                        component->Remove();
                    component = node->CreateComponent(type, REPLICATED, componentID);
                }

                // If was unable to create the component, would desync the message and therefore have to abort
                if (!component)
                {
                    URHO3D_LOGERROR("CreateComponent message parsing aborted due to unknown component");
                    return;
                }

                // Read initial attributes and apply
                component->ReadDeltaUpdate(msg);
                component->ApplyAttributes();
            }
            else
                URHO3D_LOGWARNING("CreateComponent message received for missing node " + String(nodeID));
        }
        break;

    case MSG_COMPONENTDELTAUPDATE:
        {
            unsigned componentID = msg.ReadNetID();
            Component* component = scene_->GetComponent(componentID);
            if (component)
            {
                component->ReadDeltaUpdate(msg);
                component->ApplyAttributes();
            }
            else
                URHO3D_LOGWARNING("ComponentDeltaUpdate message received for missing component " + String(componentID));
        }
        break;

    case MSG_COMPONENTLATESTDATA:
        {
            unsigned componentID = msg.ReadNetID();
            Component* component = scene_->GetComponent(componentID);
            if (component)
            {
                if (component->ReadLatestDataUpdate(msg))
                    component->ApplyAttributes();
            }
            else
            {
                // Latest data messages may be received out-of-order relative to component creation, so cache if necessary
                PODVector<unsigned char>& data = componentLatestData_[componentID];
                data.Resize(msg.GetSize());
                memcpy(&data[0], msg.GetData(), msg.GetSize());
            }
        }
        break;

    case MSG_REMOVECOMPONENT:
        {
            unsigned componentID = msg.ReadNetID();
            Component* component = scene_->GetComponent(componentID);
            if (component)
                component->Remove();
            componentLatestData_.Erase(componentID);
        }
        break;

    default: break;
    }
}

void Connection::ProcessIdentity(int msgID, MemoryBuffer& msg)
{
    if (!IsClient())
    {
        URHO3D_LOGWARNING("Received unexpected Identity message from server");
        return;
    }

    identity_ = msg.ReadVariantMap();

    using namespace ClientIdentity;

    VariantMap eventData = identity_;
    eventData[P_CONNECTION] = this;
    eventData[P_ALLOW] = true;
    SendEvent(E_CLIENTIDENTITY, eventData);

    sceneLoaded_ = true;
}

void Connection::ProcessControls(int msgID, MemoryBuffer& msg)
{
    if (!IsClient())
    {
        URHO3D_LOGWARNING("Received unexpected Controls message from server");
        return;
    }

    Controls newControls;
    newControls.buttons_ = msg.ReadUInt();
    newControls.yaw_ = msg.ReadFloat();
    newControls.pitch_ = msg.ReadFloat();
    newControls.extraData_ = msg.ReadVariantMap();

    SetControls(newControls);
    timeStamp_ = msg.ReadUByte();

    // Client may or may not send observer position & rotation for interest management
    if (!msg.IsEof())
        position_ = msg.ReadVector3();
    if (!msg.IsEof())
        rotation_ = msg.ReadPackedQuaternion();
}


void Connection::ProcessRemoteEvent(int msgID, MemoryBuffer& msg)
{
    using namespace RemoteEventData;

    if (msgID == MSG_REMOTEEVENT)
    {
        StringHash eventType = msg.ReadStringHash();

        VariantMap eventData = msg.ReadVariantMap();
        eventData[P_CONNECTION] = this;
        SendEvent(eventType, eventData);
    }
    else
    {
        if (!scene_)
        {
            URHO3D_LOGERROR("Can not receive remote node event without an assigned scene");
            return;
        }

        unsigned nodeID = msg.ReadNetID();
        StringHash eventType = msg.ReadStringHash();
       
        VariantMap eventData = msg.ReadVariantMap();
        Node* sender = scene_->GetNode(nodeID);
        if (!sender)
        {
            URHO3D_LOGWARNING("Missing sender for remote node event, discarding");
            return;
        }
        eventData[P_CONNECTION] = this;
        sender->SendEvent(eventType, eventData);
    }
}

Scene* Connection::GetScene() const
{
    return scene_;
}

void Connection::ProcessNode(unsigned nodeID)
{
    // Check that we have not already processed this due to dependency recursion
    if (!nodesToProcess_.Erase(nodeID))
        return;

    // Find replication state for the node
    HashMap<unsigned, NodeReplicationState>::Iterator i = sceneState_.nodeStates_.Find(nodeID);
    if (i != sceneState_.nodeStates_.End())
    {
        // Replication state found: the node is either be existing or removed
        Node* node = i->second_.node_;
        if (!node)
        {
            msg_.Clear();
            msg_.WriteNetID(nodeID);

            // Note: we will send MSG_REMOVENODE redundantly for each node in the hierarchy, even if removing the root node
            // would be enough. However, this may be better due to the client not possibly having updated parenting
            // information at the time of receiving this message
            SendMessage(MSG_REMOVENODE, true, true, msg_);
            sceneState_.nodeStates_.Erase(nodeID);
        }
        else
            ProcessExistingNode(node, i->second_);
    }
    else
    {
        // Replication state not found: this is a new node
        Node* node = scene_->GetNode(nodeID);
        if (node)
            ProcessNewNode(node);
        else
        {
            // Did not find the new node (may have been created, then removed immediately): erase from dirty set.
            sceneState_.dirtyNodes_.Erase(nodeID);
        }
    }
}

void Connection::ProcessNewNode(Node* node)
{
    // Process depended upon nodes first, if they are dirty
    const PODVector<Node*>& dependencyNodes = node->GetDependencyNodes();
    for (PODVector<Node*>::ConstIterator i = dependencyNodes.Begin(); i != dependencyNodes.End(); ++i)
    {
        unsigned nodeID = (*i)->GetID();
        if (sceneState_.dirtyNodes_.Contains(nodeID))
            ProcessNode(nodeID);
    }

    msg_.Clear();
    msg_.WriteNetID(node->GetID());

    NodeReplicationState& nodeState = sceneState_.nodeStates_[node->GetID()];
    nodeState.connection_ = this;
    nodeState.sceneState_ = &sceneState_;
    nodeState.node_ = node;
    node->AddReplicationState(&nodeState);

    // Write node's attributes
    node->WriteInitialDeltaUpdate(msg_, timeStamp_);

    // Write node's user variables
    const VariantMap& vars = node->GetVars();
    msg_.WriteVLE(vars.Size());
    for (VariantMap::ConstIterator i = vars.Begin(); i != vars.End(); ++i)
    {
        msg_.WriteStringHash(i->first_);
        msg_.WriteVariant(i->second_);
    }

    // Write node's components
    msg_.WriteVLE(node->GetNumNetworkComponents());
    const Vector<SharedPtr<Component> >& components = node->GetComponents();
    for (unsigned i = 0; i < components.Size(); ++i)
    {
        Component* component = components[i];
        // Check if component is not to be replicated
        if (component->GetID() >= FIRST_LOCAL_ID)
            continue;

        ComponentReplicationState& componentState = nodeState.componentStates_[component->GetID()];
        componentState.connection_ = this;
        componentState.nodeState_ = &nodeState;
        componentState.component_ = component;
        component->AddReplicationState(&componentState);

        msg_.WriteStringHash(component->GetType());
        msg_.WriteNetID(component->GetID());
        component->WriteInitialDeltaUpdate(msg_, timeStamp_);
    }

    SendMessage(MSG_CREATENODE, true, true, msg_);

    nodeState.markedDirty_ = false;
    sceneState_.dirtyNodes_.Erase(node->GetID());
}

void Connection::ProcessExistingNode(Node* node, NodeReplicationState& nodeState)
{
    // Process depended upon nodes first, if they are dirty
    const PODVector<Node*>& dependencyNodes = node->GetDependencyNodes();
    for (PODVector<Node*>::ConstIterator i = dependencyNodes.Begin(); i != dependencyNodes.End(); ++i)
    {
        unsigned nodeID = (*i)->GetID();
        if (sceneState_.dirtyNodes_.Contains(nodeID))
            ProcessNode(nodeID);
    }

    // Check from the interest management component, if exists, whether should update
    /// \todo Searching for the component is a potential CPU hotspot. It should be cached
    NetworkPriority* priority = node->GetComponent<NetworkPriority>();
    if (priority && (!priority->GetAlwaysUpdateOwner() || node->GetOwner() != this))
    {
        float distance = (node->GetWorldPosition() - position_).Length();
        if (!priority->CheckUpdate(distance, nodeState.priorityAcc_))
            return;
    }

    // Check if attributes have changed
    if (nodeState.dirtyAttributes_.Count() || nodeState.dirtyVars_.Size())
    {
        const Vector<AttributeInfo>* attributes = node->GetNetworkAttributes();
        unsigned numAttributes = attributes->Size();
        bool hasLatestData = false;

        for (unsigned i = 0; i < numAttributes; ++i)
        {
            if (nodeState.dirtyAttributes_.IsSet(i) && (attributes->At(i).mode_ & AM_LATESTDATA))
            {
                hasLatestData = true;
                nodeState.dirtyAttributes_.Clear(i);
            }
        }

        // Send latestdata message if necessary
        if (hasLatestData)
        {
            msg_.Clear();
            msg_.WriteNetID(node->GetID());
            node->WriteLatestDataUpdate(msg_, timeStamp_);

            SendMessage(MSG_NODELATESTDATA, true, false, msg_, node->GetID());
        }

        // Send deltaupdate if remaining dirty bits, or vars have changed
        if (nodeState.dirtyAttributes_.Count() || nodeState.dirtyVars_.Size())
        {
            msg_.Clear();
            msg_.WriteNetID(node->GetID());
            node->WriteDeltaUpdate(msg_, nodeState.dirtyAttributes_, timeStamp_);

            // Write changed variables
            msg_.WriteVLE(nodeState.dirtyVars_.Size());
            const VariantMap& vars = node->GetVars();
            for (HashSet<StringHash>::ConstIterator i = nodeState.dirtyVars_.Begin(); i != nodeState.dirtyVars_.End(); ++i)
            {
                VariantMap::ConstIterator j = vars.Find(*i);
                if (j != vars.End())
                {
                    msg_.WriteStringHash(j->first_);
                    msg_.WriteVariant(j->second_);
                }
                else
                {
                    // Variable has been marked dirty, but is removed (which is unsupported): send a dummy variable in place
                    URHO3D_LOGWARNING("Sending dummy user variable as original value was removed");
                    msg_.WriteStringHash(StringHash());
                    msg_.WriteVariant(Variant::EMPTY);
                }
            }

            SendMessage(MSG_NODEDELTAUPDATE, true, true, msg_);

            nodeState.dirtyAttributes_.ClearAll();
            nodeState.dirtyVars_.Clear();
        }
    }

    // Check for removed or changed components
    for (HashMap<unsigned, ComponentReplicationState>::Iterator i = nodeState.componentStates_.Begin();
         i != nodeState.componentStates_.End();)
    {
        HashMap<unsigned, ComponentReplicationState>::Iterator current = i++;
        ComponentReplicationState& componentState = current->second_;
        Component* component = componentState.component_;
        if (!component)
        {
            // Removed component
            msg_.Clear();
            msg_.WriteNetID(current->first_);

            SendMessage(MSG_REMOVECOMPONENT, true, true, msg_);
            nodeState.componentStates_.Erase(current);
        }
        else
        {
            // Existing component. Check if attributes have changed
            if (componentState.dirtyAttributes_.Count())
            {
                const Vector<AttributeInfo>* attributes = component->GetNetworkAttributes();
                unsigned numAttributes = attributes->Size();
                bool hasLatestData = false;

                for (unsigned i = 0; i < numAttributes; ++i)
                {
                    if (componentState.dirtyAttributes_.IsSet(i) && (attributes->At(i).mode_ & AM_LATESTDATA))
                    {
                        hasLatestData = true;
                        componentState.dirtyAttributes_.Clear(i);
                    }
                }

                // Send latestdata message if necessary
                if (hasLatestData)
                {
                    msg_.Clear();
                    msg_.WriteNetID(component->GetID());
                    component->WriteLatestDataUpdate(msg_, timeStamp_);

                    SendMessage(MSG_COMPONENTLATESTDATA, true, false, msg_, component->GetID());
                }

                // Send deltaupdate if remaining dirty bits
                if (componentState.dirtyAttributes_.Count())
                {
                    msg_.Clear();
                    msg_.WriteNetID(component->GetID());
                    component->WriteDeltaUpdate(msg_, componentState.dirtyAttributes_, timeStamp_);

                    SendMessage(MSG_COMPONENTDELTAUPDATE, true, true, msg_);

                    componentState.dirtyAttributes_.ClearAll();
                }
            }
        }
    }

    // Check for new components
    if (nodeState.componentStates_.Size() != node->GetNumNetworkComponents())
    {
        const Vector<SharedPtr<Component> >& components = node->GetComponents();
        for (unsigned i = 0; i < components.Size(); ++i)
        {
            Component* component = components[i];
            // Check if component is not to be replicated
            if (component->GetID() >= FIRST_LOCAL_ID)
                continue;

            HashMap<unsigned, ComponentReplicationState>::Iterator j = nodeState.componentStates_.Find(component->GetID());
            if (j == nodeState.componentStates_.End())
            {
                // New component
                ComponentReplicationState& componentState = nodeState.componentStates_[component->GetID()];
                componentState.connection_ = this;
                componentState.nodeState_ = &nodeState;
                componentState.component_ = component;
                component->AddReplicationState(&componentState);

                msg_.Clear();
                msg_.WriteNetID(node->GetID());
                msg_.WriteStringHash(component->GetType());
                msg_.WriteNetID(component->GetID());
                component->WriteInitialDeltaUpdate(msg_, timeStamp_);

                SendMessage(MSG_CREATECOMPONENT, true, true, msg_);
            }
        }
    }

    nodeState.markedDirty_ = false;
    sceneState_.dirtyNodes_.Erase(node->GetID());
}


KNetConnection::KNetConnection(Context* context, bool isClient,
    kNet::SharedPtr<kNet::MessageConnection> connection)
    : Connection(context, isClient)
    , connection_(connection)
{
    // Store address and port now for accurate logging (kNet may already have destroyed the socket on disconnection,
    // in which case we would log a zero address:port on disconnect)
    kNet::EndPoint endPoint = connection_->RemoteEndPoint();
    ///\todo Not IPv6-capable.
    address_ = Urho3D::ToString("%d.%d.%d.%d", endPoint.ip[0], endPoint.ip[1], endPoint.ip[2], endPoint.ip[3]);
    port_ = endPoint.port;

}

bool KNetConnection::Send(int msgID, bool reliable, bool inOrder, const VectorBuffer& msg, unsigned contentID)
{
    // Make sure not to use kNet internal message ID's
    if (msgID <= 0x4 || msgID >= 0x3ffffffe)
    {
        URHO3D_LOGERROR("Can not send message with reserved ID");
        return false;
    }

    kNet::NetworkMessage* kNetMsg = connection_->StartNewMessage((unsigned long)msgID, msg.GetSize());
    if (!kNetMsg)
    {
        URHO3D_LOGERROR("Can not start new network message");
        return false;
    }

    kNetMsg->reliable = reliable;
    kNetMsg->inOrder = inOrder;
    kNetMsg->priority = 0;
    kNetMsg->contentID = 0;
    if (msg.GetSize())
        memcpy(kNetMsg->data, msg.GetData(), msg.GetSize());

    connection_->EndAndQueueMessage(kNetMsg);
    return true;
}

void KNetConnection::Disconnect(int waitMSec)
{
    connection_->Disconnect(waitMSec);
}

kNet::MessageConnection* KNetConnection::GetMessageConnection() const
{
    return const_cast<kNet::MessageConnection*>(connection_.ptr());
}

void KNetConnection::ConfigureNetworkSimulator(int latencyMs, float packetLoss)
{
    if (connection_)
    {
        kNet::NetworkSimulator& simulator = connection_->NetworkSendSimulator();
        simulator.enabled = latencyMs > 0 || packetLoss > 0.0f;
        simulator.constantPacketSendDelay = (float)latencyMs;
        simulator.packetLossRate = packetLoss;
    }
}

String KNetConnection::ToString() const
{
    return address_ + ":" + String(port_);
}


}
