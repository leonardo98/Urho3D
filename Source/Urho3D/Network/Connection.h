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

#pragma once

#include "../Container/HashSet.h"
#include "../Core/Object.h"
#include "../Core/Timer.h"
#include "../Input/Controls.h"
#include "../IO/VectorBuffer.h"
#include "../Scene/ReplicationState.h"

#include <kNet/kNetFwd.h>
#include <kNet/SharedPtr.h>

#ifdef SendMessage
#undef SendMessage
#endif

namespace Urho3D
{

class File;
class MemoryBuffer;
class Node;
class Scene;
class Serializable;
class PackageFile;

/// Queued remote event.
struct RemoteEvent
{
    /// Remote sender node ID (0 if not a remote node event.)
    unsigned senderID_;
    /// Event type.
    StringHash eventType_;
    /// Event data.
    VariantMap eventData_;
    /// In order flag.
    bool inOrder_;
};

/// Send modes for observer position/rotation. Activated by the client setting either position or rotation.
enum ObserverPositionSendMode
{
    OPSM_NONE = 0,
    OPSM_POSITION,
    OPSM_POSITION_ROTATION
};

/// %Connection to a remote network host.
class URHO3D_API Connection : public Object
{
    URHO3D_OBJECT(Connection, Object);

public:
    /// Construct with context
    Connection(Context* context, bool isClient);
    /// Destruct.
    ~Connection();

    /// Send a message.
    void SendMessage(int msgID, bool reliable, bool inOrder, const VectorBuffer& msg, unsigned contentID = 0);
    /// Send a remote event.
    void SendRemoteEvent(StringHash eventType, bool inOrder, const VariantMap& eventData = Variant::emptyVariantMap);
    /// Send a remote event with the specified node as sender.
    void SendRemoteEvent(Node* node, StringHash eventType, bool inOrder, const VariantMap& eventData = Variant::emptyVariantMap);
    /// Assign scene. On the server, this will cause the client to load it.
    void SetScene(Scene* newScene);
    /// Assign identity. Called by Network.
    void SetIdentity(const VariantMap& identity);
    /// Set new controls.
    void SetControls(const Controls& newControls);
    /// Set the observer position for interest management, to be sent to the server.
    void SetPosition(const Vector3& position);
    /// Set the observer rotation for interest management, to be sent to the server. Note: not used by the NetworkPriority component.
    void SetRotation(const Quaternion& rotation);
    /// Set the connection pending status. Called by Network.
    void SetConnectPending(bool connectPending);
    /// Set whether to log data in/out statistics.
    void SetLogStatistics(bool enable);
    /// Send scene update messages. Called by Network.
    void SendServerUpdate();
    /// Send latest controls from the client. Called by Network.
    void SendClientUpdate();
    /// Send queued remote events. Called by Network.
    void SendRemoteEvents();
    /// Process pending latest data for nodes and components.
    void ProcessPendingLatestData();
    /// Process a message from the server or client. Called by Network.
    bool ProcessMessage(int msgID, MemoryBuffer& msg);

    /// Return client identity.
    VariantMap& GetIdentity() { return identity_; }

    /// Return the scene used by this connection.
    Scene* GetScene() const;

    /// Return the client controls of this connection.
    const Controls& GetControls() const { return controls_; }

    /// Return the controls timestamp, sent from client to server along each control update.
    unsigned char GetTimeStamp() const { return timeStamp_; }

    /// Return the observer position sent by the client for interest management.
    const Vector3& GetPosition() const { return position_; }

    /// Return the observer rotation sent by the client for interest management.
    const Quaternion& GetRotation() const { return rotation_; }

    /// Return whether is a client connection.
    bool IsClient() const { return isClient_; }

    /// Return whether connection is pending.
    bool IsConnectPending() const { return connectPending_; }

    /// Return whether the scene is loaded and ready to receive server updates.
    bool IsSceneLoaded() const { return sceneLoaded_; }

    /// Return whether to log data in/out statistics.
    bool GetLogStatistics() const { return logStatistics_; }

    /// Current controls.
    Controls controls_;
    /// Controls timestamp. Incremented after each sent update.
    unsigned char timeStamp_;
    /// Identity map.
    VariantMap identity_;

private:

    virtual bool Send(int msgID, bool reliable, bool inOrder, const VectorBuffer& msg, unsigned contentID = 0) = 0;
    
private:
    /// Process a LoadScene message from the server. Called by Network.
    void ProcessLoadScene(int msgID, MemoryBuffer& msg);
    /// Process a scene update message from the server. Called by Network.
    void ProcessSceneUpdate(int msgID, MemoryBuffer& msg);
    /// Process an Identity message from the client. Called by Network.
    void ProcessIdentity(int msgID, MemoryBuffer& msg);
    /// Process a Controls message from the client. Called by Network.
    void ProcessControls(int msgID, MemoryBuffer& msg);
    /// Process a remote event message from the client or server. Called by Network.
    void ProcessRemoteEvent(int msgID, MemoryBuffer& msg);
    /// Process a node for sending a network update. Recurses to process depended on node(s) first.
    void ProcessNode(unsigned nodeID);
    /// Process a node that the client has not yet received.
    void ProcessNewNode(Node* node);
    /// Process a node that the client has already received.
    void ProcessExistingNode(Node* node, NodeReplicationState& nodeState);
    /// Return whether nodes creation is available
    bool IsNodeCreationAvailable() const;

    /// Scene.
    WeakPtr<Scene> scene_;
    /// Network replication state of the scene.
    SceneReplicationState sceneState_;
    /// Pending latest data for not yet received nodes.
    HashMap<unsigned, PODVector<unsigned char> > nodeLatestData_;
    /// Pending latest data for not yet received components.
    HashMap<unsigned, PODVector<unsigned char> > componentLatestData_;
    /// Node ID's to process during a replication update.
    HashSet<unsigned> nodesToProcess_;
    /// Reusable message buffer.
    VectorBuffer msg_;
    /// Queued remote events.
    Vector<RemoteEvent> remoteEvents_;
    /// Statistics timer.
    Timer statsTimer_;
    /// Observer position for interest management.
    Vector3 position_;
    /// Observer rotation for interest management.
    Quaternion rotation_;
    /// Send mode for the observer position & rotation.
    ObserverPositionSendMode sendMode_;
    /// Client connection flag.
    bool isClient_;
    /// Connection pending flag.
    bool connectPending_;
    /// Scene loaded flag.
    bool sceneLoaded_;
    /// Show statistics flag.
    bool logStatistics_;
    /// the number of the nodes was created (tick)
    unsigned numberNodeCreated_;
};


class URHO3D_API KNetConnection : public Connection
{
    URHO3D_OBJECT(KNetConnection, Connection);

public:
    /// Construct with context and kNet message connection pointers.
    KNetConnection(Context* context, bool isClient, 
        kNet::SharedPtr<kNet::MessageConnection> connection);

    kNet::MessageConnection* GetMessageConnection() const;

    /// Disconnect. If wait time is non-zero, will block while waiting for disconnect to finish.
    void Disconnect(int waitMSec = 0);

    /// Set network simulation parameters. Called by Network.
    void ConfigureNetworkSimulator(int latencyMs, float packetLoss);
    /// Return an address:port string.
    String ToString() const;

private:

    bool Send(int msgID, bool reliable, bool inOrder, const VectorBuffer& msg, unsigned contentID = 0) override;
    
private:

    /// kNet message connection.
    kNet::SharedPtr<kNet::MessageConnection> connection_;
    /// Remote endpoint address.
    String address_;
    /// Remote endpoint port.
    unsigned short port_;
};

}
