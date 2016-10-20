/**
 * @file megachatapi_impl.cpp
 * @brief Private implementation of the intermediate layer for the MEGA C++ SDK.
 *
 * (c) 2013-2016 by Mega Limited, Auckland, New Zealand
 *
 * This file is part of the MEGA SDK - Client Access Engine.
 *
 * Applications using the MEGA API must present a valid application key
 * and comply with the the rules set forth in the Terms of Service.
 *
 * The MEGA SDK is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 * @copyright Simplified (2-clause) BSD License.
 *
 * You should have received a copy of the license along with this
 * program.
 */

#define _POSIX_SOURCE
#define _LARGE_FILES

#define _GNU_SOURCE 1
#define _FILE_OFFSET_BITS 64

#define __DARWIN_C_LEVEL 199506L

#define USE_VARARGS
#define PREFER_STDARG

#include <megaapi_impl.h>

#include "megachatapi_impl.h"
#include <base/cservices.h>
#include <base/logger.h>
#include <IGui.h>
#include <chatClient.h>

#ifndef _WIN32
#include <signal.h>
#endif

using namespace std;
using namespace megachat;
using namespace mega;
using namespace karere;
using namespace chatd;

vector<MegaChatApiImpl *> MegaChatApiImpl::megaChatApiRefs;
LoggerHandler *MegaChatApiImpl::loggerHandler = NULL;
MegaMutex MegaChatApiImpl::sdkMutex(true);
MegaMutex MegaChatApiImpl::refsMutex(true);

MegaChatApiImpl::MegaChatApiImpl(MegaChatApi *chatApi, MegaApi *megaApi)
: localVideoReceiver(nullptr)
{
    refsMutex.lock();
    megaChatApiRefs.push_back(this);
    if (megaChatApiRefs.size() == 1)
    {
        // karere initialization (do NOT use globaInit() since it forces to log to file)
        services_init(MegaChatApiImpl::megaApiPostMessage, SVC_STROPHE_LOG);
    }
    refsMutex.unlock();

    init(chatApi, megaApi);
}

//MegaChatApiImpl::MegaChatApiImpl(MegaChatApi *chatApi, const char *appKey, const char *appDir)
//{
//    init(chatApi, (MegaApi*) new MyMegaApi(appKey, appDir));
//}

MegaChatApiImpl::~MegaChatApiImpl()
{
    MegaChatRequestPrivate *request = new MegaChatRequestPrivate(MegaChatRequest::TYPE_DELETE);
    requestQueue.push(request);
    waiter->notify();
    thread.join();
}

void MegaChatApiImpl::init(MegaChatApi *chatApi, MegaApi *megaApi)
{
    this->chatApi = chatApi;
    this->megaApi = megaApi;
    this->megaApi->addRequestListener(this);

    this->waiter = new MegaWaiter();
    this->mClient = NULL;   // created at loop()

    this->resumeSession = false;
    this->initResult = NULL;
    this->initRequest = NULL;

    this->status = MegaChatApi::STATUS_OFFLINE;

    //Start blocking thread
    threadExit = 0;
    thread.start(threadEntryPoint, this);
}

//Entry point for the blocking thread
void *MegaChatApiImpl::threadEntryPoint(void *param)
{
#ifndef _WIN32
    struct sigaction noaction;
    memset(&noaction, 0, sizeof(noaction));
    noaction.sa_handler = SIG_IGN;
    ::sigaction(SIGPIPE, &noaction, 0);
#endif

    MegaChatApiImpl *chatApiImpl = (MegaChatApiImpl *)param;
    chatApiImpl->loop();
    return 0;
}

void MegaChatApiImpl::loop()
{
    mClient = new karere::Client(*megaApi, *this, megaApi->getBasePath(), Presence::kOnline);

    while (true)
    {
        sdkMutex.unlock();

        waiter->init(NEVER);
        waiter->wait();         // waken up directly by Waiter::notify()

        sdkMutex.lock();

        sendPendingEvents();
        sendPendingRequests();

        if (threadExit)
        {
            // remove the MegaChatApiImpl that is being deleted
            refsMutex.lock();
            for (vector<MegaChatApiImpl*>::iterator it = megaChatApiRefs.begin(); it != megaChatApiRefs.end(); it++)
            {
                if (*it == this)
                {
                    megaChatApiRefs.erase(it);
                    break;
                }
            }
            sendPendingEvents();    // process any pending events in the queue
            refsMutex.unlock();
            if (!megaChatApiRefs.size())    // if no remaining instances...
            {
                globalCleanup();
            }
            sdkMutex.unlock();
            break;
        }
    }
}

void MegaChatApiImpl::megaApiPostMessage(void* msg)
{
    // Add the message to the queue of events
    refsMutex.lock();
    if (megaChatApiRefs.size())
    {
        megaChatApiRefs[0]->postMessage(msg);
    }
    else    // no more instances running, only one left --> directly process messages
    {
        megaProcessMessage(msg);
    }
    refsMutex.unlock();
}

void MegaChatApiImpl::postMessage(void *msg)
{
    eventQueue.push(msg);
    waiter->notify();
}

void MegaChatApiImpl::sendPendingRequests()
{
    MegaChatRequestPrivate *request;
    int errorCode = MegaChatError::ERROR_OK;
    int nextTag = 0;

    while((request = requestQueue.pop()))
    {
        nextTag = ++reqtag;
        request->setTag(nextTag);
        requestMap[nextTag]=request;
        errorCode = MegaChatError::ERROR_OK;

        fireOnChatRequestStart(request);

        switch (request->getType())
        {
        case MegaChatRequest::TYPE_INITIALIZE:
        {
            if (initResult)
            {
                if (initResult->getErrorCode() == MegaChatError::ERROR_OK)
                {
                    fireOnChatRequestFinish(request, initResult);
                    API_LOG_INFO("Initialization complete");
                    fireOnChatRoomUpdate(NULL);
                }
                else
                {
                    fireOnChatRequestFinish(request, initResult);
                    API_LOG_INFO("Initialization failed");
                }
                initResult = NULL;
                initRequest = NULL;
            }
            else
            {
                initRequest = request;
            }
            break;
        }
        case MegaChatRequest::TYPE_CONNECT:
        {
            mClient->connect()
            .then([request, this]()
            {
                MegaChatErrorPrivate *megaChatError = new MegaChatErrorPrivate(MegaChatError::ERROR_OK);
                fireOnChatRequestFinish(request, megaChatError);
            })
            .fail([request, this](const promise::Error& e)
            {
                MegaChatErrorPrivate *megaChatError = new MegaChatErrorPrivate(e.msg(), e.code(), e.type());
                fireOnChatRequestFinish(request, megaChatError);
            });

            break;
        }
        case MegaChatRequest::TYPE_LOGOUT:
        {
            bool deleteDb = request->getFlag();
            mClient->terminate(deleteDb)
            .then([request, this]()
            {
                API_LOG_INFO("Chat engine is logged out!");

                delete mClient;
                mClient = new karere::Client(*this->megaApi, *this, this->megaApi->getBasePath(), Presence::kOnline);

                MegaChatErrorPrivate *megaChatError = new MegaChatErrorPrivate(MegaChatError::ERROR_OK);
                fireOnChatRequestFinish(request, megaChatError);
            })
            .fail([request, this](const promise::Error& e)
            {
                MegaChatErrorPrivate *megaChatError = new MegaChatErrorPrivate(e.msg(), e.code(), e.type());
                fireOnChatRequestFinish(request, megaChatError);
            });
            break;
        }
        case MegaChatRequest::TYPE_DELETE:
        {
            mClient->terminate()
            .then([this]()
            {
                API_LOG_INFO("Chat engine closed!");
                threadExit = 1;
            })
            .fail([](const promise::Error& err)
            {
                API_LOG_ERROR("Error closing chat engine: ", err.what());
            });
            break;
        }
        case MegaChatRequest::TYPE_SET_ONLINE_STATUS:
        {
            int status = request->getNumber();
            if (status < MegaChatApi::STATUS_OFFLINE || status > MegaChatApi::STATUS_CHATTY)
            {
                fireOnChatRequestFinish(request, new MegaChatErrorPrivate("Invalid online status", MegaChatError::ERROR_ARGS));
                break;
            }

            mClient->setPresence(request->getNumber(), true)
            .then([request, this]()
            {
                MegaChatErrorPrivate *megaChatError = new MegaChatErrorPrivate(MegaChatError::ERROR_OK);
                fireOnChatRequestFinish(request, megaChatError);
            })
            .fail([request, this](const promise::Error& err)
            {
                API_LOG_ERROR("Error setting online status: ", err.what());

                MegaChatErrorPrivate *megaChatError = new MegaChatErrorPrivate(err.msg(), err.code(), err.type());
                fireOnChatRequestFinish(request, megaChatError);
            });
            break;
        }

        case MegaChatRequest::TYPE_CREATE_CHATROOM:
        {
            MegaChatPeerList *peersList = request->getMegaChatPeerList();
            if (!peersList || !peersList->size())   // refuse to create chats without participants
            {
                errorCode = MegaChatError::ERROR_ARGS;
                break;
            }

            bool group = request->getFlag();
            const userpriv_vector *userpriv = ((MegaChatPeerListPrivate*)peersList)->getList();
            if (!userpriv)
            {
                errorCode = MegaChatError::ERROR_ARGS;
                break;
            }

            if (!group && peersList->size() > 1)
            {
                group = true;
                request->setFlag(group);
                API_LOG_INFO("Forcing group chat due to more than 2 participants");
            }

            if (group)
            {
                vector<std::pair<handle, Priv>> peers;
                for (unsigned int i = 0; i < userpriv->size(); i++)
                {
                    peers.push_back(std::make_pair(userpriv->at(i).first, (Priv) userpriv->at(i).second));
                }

                mClient->createGroupChat(peers)
                .then([request,this](Id chatid)
                {
                    request->setChatHandle(chatid);

                    MegaChatErrorPrivate *megaChatError = new MegaChatErrorPrivate(MegaChatError::ERROR_OK);
                    fireOnChatRequestFinish(request, megaChatError);
                })
                .fail([request,this](const promise::Error& err)
                {
                    API_LOG_ERROR("Error creating group chat: ", err.what());

                    MegaChatErrorPrivate *megaChatError = new MegaChatErrorPrivate(err.msg(), err.code(), err.type());
                    fireOnChatRequestFinish(request, megaChatError);
                });

            }
            else    // 1on1 chat
            {
                ContactList::iterator it = mClient->contactList->find(peersList->getPeerHandle(0));
                it->second->createChatRoom()
                .then([request,this](ChatRoom* room)
                {
                    request->setChatHandle(room->chatid());

                    MegaChatErrorPrivate *megaChatError = new MegaChatErrorPrivate(MegaChatError::ERROR_OK);
                    fireOnChatRequestFinish(request, megaChatError);
                })
                .fail([request,this](const promise::Error& err)
                {
                    API_LOG_ERROR("Error creating 1on1 chat: ", err.what());

                    MegaChatErrorPrivate *megaChatError = new MegaChatErrorPrivate(err.msg(), err.code(), err.type());
                    fireOnChatRequestFinish(request, megaChatError);
                });
            }
            break;
        }
        case MegaChatRequest::TYPE_INVITE_TO_CHATROOM:
        {
            handle chatid = request->getChatHandle();
            handle uh = request->getUserHandle();
            Priv privilege = (Priv) request->getPrivilege();

            if (chatid == MEGACHAT_INVALID_HANDLE || uh == MEGACHAT_INVALID_HANDLE)
            {
                errorCode = MegaChatError::ERROR_ARGS;
                break;
            }

            ChatRoom *chatroom = findChatRoom(chatid);
            if (!chatroom)
            {
                errorCode = MegaChatError::ERROR_NOENT;
                break;
            }
            if (!chatroom->isGroup())   // invite only for group chats
            {
                errorCode = MegaChatError::ERROR_ARGS;
                break;
            }
            if (chatroom->ownPriv() != (Priv) MegaChatPeerList::PRIV_MODERATOR)
            {
                errorCode = MegaChatError::ERROR_ACCESS;
                break;
            }

            ((GroupChatRoom *)chatroom)->invite(uh, privilege)
            .then([request, this]()
            {
                MegaChatErrorPrivate *megaChatError = new MegaChatErrorPrivate(MegaChatError::ERROR_OK);
                fireOnChatRequestFinish(request, megaChatError);
            })
            .fail([request, this](const promise::Error& err)
            {
                API_LOG_ERROR("Error adding user to group chat: ", err.what());

                MegaChatErrorPrivate *megaChatError = new MegaChatErrorPrivate(err.msg(), err.code(), err.type());
                fireOnChatRequestFinish(request, megaChatError);
            });
            break;
        }
        case MegaChatRequest::TYPE_UPDATE_PEER_PERMISSIONS:
        {
            handle chatid = request->getChatHandle();
            handle uh = request->getUserHandle();
            int privilege = request->getPrivilege();

            if (chatid == MEGACHAT_INVALID_HANDLE || uh == MEGACHAT_INVALID_HANDLE)
            {
                errorCode = MegaChatError::ERROR_ARGS;
                break;
            }

            ChatRoom *chatroom = findChatRoom(chatid);
            if (!chatroom)
            {
                errorCode = MegaChatError::ERROR_NOENT;
                break;
            }
            if (chatroom->ownPriv() != (Priv) MegaChatPeerList::PRIV_MODERATOR)
            {
                errorCode = MegaChatError::ERROR_ACCESS;
                break;
            }

            mClient->api.call(&MegaApi::updateChatPermissions, chatid, uh, privilege)
            .then([request, this](ReqResult result)
            {
                MegaChatErrorPrivate *megaChatError = new MegaChatErrorPrivate(MegaChatError::ERROR_OK);
                fireOnChatRequestFinish(request, megaChatError);
            })
            .fail([request, this](const promise::Error& err)
            {
                API_LOG_ERROR("Error updating peer privileges: ", err.what());

                MegaChatErrorPrivate *megaChatError = new MegaChatErrorPrivate(err.msg(), err.code(), err.type());
                fireOnChatRequestFinish(request, megaChatError);
            });
            break;
        }
        case MegaChatRequest::TYPE_REMOVE_FROM_CHATROOM:
        {
            handle chatid = request->getChatHandle();
            handle uh = request->getUserHandle();

            if (chatid == MEGACHAT_INVALID_HANDLE)
            {
                errorCode = MegaChatError::ERROR_ARGS;
                break;
            }

            ChatRoom *chatroom = findChatRoom(chatid);
            if (!chatroom)
            {
                errorCode = MegaChatError::ERROR_NOENT;
                break;
            }
            if (!chatroom->isGroup())   // only for group chats can be left
            {
                errorCode = MegaChatError::ERROR_ARGS;
                break;
            }

            if (chatroom->ownPriv() != (Priv) MegaChatPeerList::PRIV_MODERATOR)
            {
                if (uh != MEGACHAT_INVALID_HANDLE)
                {
                    errorCode = MegaChatError::ERROR_ACCESS;
                    break;
                }
                else    // uh is optional. If not provided, own user wants to leave the chat
                {
                    uh = mClient->myHandle();
                }
            }

            ((GroupChatRoom *)chatroom)->excludeMember(uh)
            .then([request, this](ReqResult result)
            {
                MegaChatErrorPrivate *megaChatError = new MegaChatErrorPrivate(MegaChatError::ERROR_OK);
                fireOnChatRequestFinish(request, megaChatError);
            })
            .fail([request, this](const promise::Error& err)
            {
                API_LOG_ERROR("Error removing peer from chat: ", err.what());

                MegaChatErrorPrivate *megaChatError = new MegaChatErrorPrivate(err.msg(), err.code(), err.type());
                fireOnChatRequestFinish(request, megaChatError);
            });
            break;
        }
        case MegaChatRequest::TYPE_TRUNCATE_HISTORY:
        {
            handle chatid = request->getChatHandle();
            handle messageid = request->getUserHandle();
            if (chatid == MEGACHAT_INVALID_HANDLE || messageid == MEGACHAT_INVALID_HANDLE)
            {
                errorCode = MegaChatError::ERROR_ARGS;
                break;
            }

            ChatRoom *chatroom = findChatRoom(chatid);
            if (!chatroom)
            {
                errorCode = MegaChatError::ERROR_NOENT;
                break;
            }
            if (chatroom->ownPriv() != (Priv) MegaChatPeerList::PRIV_MODERATOR)
            {
                errorCode = MegaChatError::ERROR_ACCESS;
                break;
            }

            mClient->api.call(&MegaApi::truncateChat, chatid, messageid)
            .then([request, this](ReqResult result)
            {
                MegaChatErrorPrivate *megaChatError = new MegaChatErrorPrivate(MegaChatError::ERROR_OK);
                fireOnChatRequestFinish(request, megaChatError);
            })
            .fail([request, this](const promise::Error& err)
            {
                API_LOG_ERROR("Error truncating chat history: ", err.what());

                MegaChatErrorPrivate *megaChatError = new MegaChatErrorPrivate(err.msg(), err.code(), err.type());
                fireOnChatRequestFinish(request, megaChatError);
            });
            break;
        }
        case MegaChatRequest::TYPE_EDIT_CHATROOM_NAME:
        {
            handle chatid = request->getChatHandle();
            const char *title = request->getText();
            if (chatid == MEGACHAT_INVALID_HANDLE || title == NULL)
            {
                errorCode = MegaChatError::ERROR_ARGS;
                break;
            }

            ChatRoom *chatroom = findChatRoom(chatid);
            if (!chatroom)
            {
                errorCode = MegaChatError::ERROR_NOENT;
                break;
            }
            if (!chatroom->isGroup())   // only for group chats have a title
            {
                errorCode = MegaChatError::ERROR_ARGS;
                break;
            }

            if (chatroom->ownPriv() != (Priv) MegaChatPeerList::PRIV_MODERATOR)
            {
                errorCode = MegaChatError::ERROR_ACCESS;
                break;
            }

            string strTitle(title, 30);
            request->setText(strTitle.c_str()); // update, in case it's been truncated

            ((GroupChatRoom *)chatroom)->setTitle(strTitle)
            .then([request, this]()
            {
                MegaChatErrorPrivate *megaChatError = new MegaChatErrorPrivate(MegaChatError::ERROR_OK);
                fireOnChatRequestFinish(request, megaChatError);
            })
            .fail([request, this](const promise::Error& err)
            {
                API_LOG_ERROR("Error editing chat title: ", err.what());

                MegaChatErrorPrivate *megaChatError = new MegaChatErrorPrivate(err.msg(), err.code(), err.type());
                fireOnChatRequestFinish(request, megaChatError);
            });
            break;
        }
        default:
        {
            errorCode = MegaChatError::ERROR_UNKNOWN;
        }
        }   // end of switch(request->getType())


        if(errorCode)
        {
            MegaChatErrorPrivate *megaChatError = new MegaChatErrorPrivate(errorCode);
            API_LOG_WARNING("Error starting request: %s", megaChatError->getErrorString());
            fireOnChatRequestFinish(request, megaChatError);
        }
    }
}

void MegaChatApiImpl::sendPendingEvents()
{
    void *msg;
    while((msg = eventQueue.pop()))
    {
        megaProcessMessage(msg);
    }
}

void MegaChatApiImpl::setLogLevel(int logLevel)
{
    if(!loggerHandler)
    {
        loggerHandler = new LoggerHandler();
    }
    loggerHandler->setLogLevel(logLevel);
}

void MegaChatApiImpl::setLoggerClass(MegaChatLogger *megaLogger)
{
    if (!megaLogger)   // removing logger
    {
        delete loggerHandler;
        loggerHandler = NULL;
    }
    else
    {
        if(!loggerHandler)
        {
            loggerHandler = new LoggerHandler();
        }
        loggerHandler->setMegaChatLogger(megaLogger);
    }
}

MegaChatRoomHandler *MegaChatApiImpl::getChatRoomHandler(MegaChatHandle chatid)
{
    map<MegaChatHandle, MegaChatRoomHandler*>::iterator it = chatRoomHandler.find(chatid);
    if (it == chatRoomHandler.end())
    {
        chatRoomHandler[chatid] = new MegaChatRoomHandler(this, chatid);
    }

    return chatRoomHandler[chatid];
}

void MegaChatApiImpl::removeChatRoomHandler(MegaChatHandle chatid)
{
    chatRoomHandler.erase(chatid);
}

ChatRoom *MegaChatApiImpl::findChatRoom(MegaChatHandle chatid)
{
    ChatRoom *chatroom = NULL;

    sdkMutex.lock();

    ChatRoomList::iterator it = mClient->chats->find(chatid);
    if (it != mClient->chats->end())
    {
        chatroom = it->second;
    }

    sdkMutex.unlock();

    return chatroom;
}

karere::ChatRoom *MegaChatApiImpl::findChatRoomByUser(MegaChatHandle userhandle)
{
    ChatRoom *chatroom = NULL;

    sdkMutex.lock();

    ContactList::iterator it = mClient->contactList->find(userhandle);
    if (it != mClient->contactList->end())
    {
        chatroom = it->second->chatRoom();
    }

    sdkMutex.unlock();

    return chatroom;
}

chatd::Message *MegaChatApiImpl::findMessage(MegaChatHandle chatid, MegaChatHandle msgid)
{
    Message *msg = NULL;

    sdkMutex.lock();

    ChatRoom *chatroom = findChatRoom(chatid);
    if (chatroom)
    {
        Chat &chat = chatroom->chat();
        Idx index = chat.msgIndexFromId(msgid);
        if (index != CHATD_IDX_INVALID)
        {
            msg = chat.findOrNull(index);
        }
    }

    sdkMutex.unlock();

    return msg;
}

chatd::Message *MegaChatApiImpl::findMessageNotConfirmed(MegaChatHandle chatid, MegaChatHandle msgxid)
{
    Message *msg = NULL;

    sdkMutex.lock();

    ChatRoom *chatroom = findChatRoom(chatid);
    if (chatroom)
    {
        Chat &chat = chatroom->chat();
        msg = chat.getMsgByXid(msgxid);
    }

    sdkMutex.unlock();

    return msg;
}

void MegaChatApiImpl::fireOnChatRequestStart(MegaChatRequestPrivate *request)
{
    API_LOG_INFO("Request (%s) starting", request->getRequestString());

    for (set<MegaChatRequestListener *>::iterator it = requestListeners.begin(); it != requestListeners.end() ; it++)
    {
        (*it)->onRequestStart(chatApi, request);
    }

    MegaChatRequestListener* listener = request->getListener();
    if (listener)
    {
        listener->onRequestStart(chatApi, request);
    }
}

void MegaChatApiImpl::fireOnChatRequestFinish(MegaChatRequestPrivate *request, MegaChatError *e)
{
    if(e->getErrorCode())
    {
        API_LOG_INFO("Request (%s) finished with error: %s", request->getRequestString(), e->getErrorString());
    }
    else
    {
        API_LOG_INFO("Request (%s) finished", request->getRequestString());
    }

    for (set<MegaChatRequestListener *>::iterator it = requestListeners.begin(); it != requestListeners.end() ; it++)
    {
        (*it)->onRequestFinish(chatApi, request, e);
    }

    MegaChatRequestListener* listener = request->getListener();
    if (listener)
    {
        listener->onRequestFinish(chatApi, request, e);
    }

    requestMap.erase(request->getTag());

    delete request;
    delete e;
}

void MegaChatApiImpl::fireOnChatRequestUpdate(MegaChatRequestPrivate *request)
{
    for (set<MegaChatRequestListener *>::iterator it = requestListeners.begin(); it != requestListeners.end() ; it++)
    {
        (*it)->onRequestUpdate(chatApi, request);
    }

    MegaChatRequestListener* listener = request->getListener();
    if (listener)
    {
        listener->onRequestUpdate(chatApi, request);
    }
}

void MegaChatApiImpl::fireOnChatRequestTemporaryError(MegaChatRequestPrivate *request, MegaChatError *e)
{
    request->setNumRetry(request->getNumRetry() + 1);

    for (set<MegaChatRequestListener *>::iterator it = requestListeners.begin(); it != requestListeners.end() ; it++)
    {
        (*it)->onRequestTemporaryError(chatApi, request, e);
    }

    MegaChatRequestListener* listener = request->getListener();
    if (listener)
    {
        listener->onRequestTemporaryError(chatApi, request, e);
    }

    delete e;
}

void MegaChatApiImpl::fireOnChatCallStart(MegaChatCallPrivate *call)
{
    API_LOG_INFO("Starting chat call");

    for(set<MegaChatCallListener *>::iterator it = callListeners.begin(); it != callListeners.end() ; it++)
    {
        (*it)->onChatCallStart(chatApi, call);
    }

    fireOnChatCallStateChange(call);
}

void MegaChatApiImpl::fireOnChatCallStateChange(MegaChatCallPrivate *call)
{
    API_LOG_INFO("Chat call state changed to %s", call->getStatus());

    for(set<MegaChatCallListener *>::iterator it = callListeners.begin(); it != callListeners.end() ; it++)
    {
        (*it)->onChatCallStateChange(chatApi, call);
    }
}

void MegaChatApiImpl::fireOnChatCallTemporaryError(MegaChatCallPrivate *call, MegaChatError *e)
{
    API_LOG_INFO("Chat call temporary error: %s", e->getErrorString());

    for(set<MegaChatCallListener *>::iterator it = callListeners.begin(); it != callListeners.end() ; it++)
    {
        (*it)->onChatCallTemporaryError(chatApi, call, e);
    }
}

void MegaChatApiImpl::fireOnChatCallFinish(MegaChatCallPrivate *call, MegaChatError *e)
{
    if(e->getErrorCode())
    {
        API_LOG_INFO("Chat call finished with error: %s", e->getErrorString());
    }
    else
    {
        API_LOG_INFO("Chat call finished");
    }

    call->setStatus(MegaChatCall::CALL_STATUS_DISCONNECTED);
    fireOnChatCallStateChange(call);

    for (set<MegaChatCallListener *>::iterator it = callListeners.begin(); it != callListeners.end() ; it++)
    {
        (*it)->onChatCallFinish(chatApi, call, e);
    }

    callMap.erase(call->getTag());

    delete call;
    delete e;
}

void MegaChatApiImpl::fireOnChatRemoteVideoData(MegaChatCallPrivate *call, int width, int height, char *buffer)
{
    API_LOG_INFO("Remote video data");

    for(set<MegaChatVideoListener *>::iterator it = remoteVideoListeners.begin(); it != remoteVideoListeners.end() ; it++)
    {
        (*it)->onChatVideoData(chatApi, call, width, height, buffer);
    }
}

void MegaChatApiImpl::fireOnChatLocalVideoData(MegaChatCallPrivate *call, int width, int height, char *buffer)
{
    API_LOG_INFO("Local video data");

    for(set<MegaChatVideoListener *>::iterator it = localVideoListeners.begin(); it != localVideoListeners.end() ; it++)
    {
        (*it)->onChatVideoData(chatApi, call, width, height, buffer);
    }
}

void MegaChatApiImpl::fireOnChatRoomUpdate(MegaChatRoom *chat)
{
    for(set<MegaChatRoomListener *>::iterator it = roomListeners.begin(); it != roomListeners.end() ; it++)
    {
        (*it)->onChatRoomUpdate(chatApi, chat);
    }

    for(set<MegaChatListener *>::iterator it = listeners.begin(); it != listeners.end() ; it++)
    {
        (*it)->onChatRoomUpdate(chatApi, chat);
    }

    delete chat;
}

void MegaChatApiImpl::fireOnMessageLoaded(MegaChatMessage *msg)
{
    for(set<MegaChatRoomListener *>::iterator it = roomListeners.begin(); it != roomListeners.end() ; it++)
    {
        (*it)->onMessageLoaded(chatApi, msg);
    }

    delete msg;
}

void MegaChatApiImpl::fireOnMessageReceived(MegaChatMessage *msg)
{
    for(set<MegaChatRoomListener *>::iterator it = roomListeners.begin(); it != roomListeners.end() ; it++)
    {
        (*it)->onMessageReceived(chatApi, msg);
    }

    delete msg;
}

void MegaChatApiImpl::fireOnMessageUpdate(MegaChatMessage *msg)
{
    for(set<MegaChatRoomListener *>::iterator it = roomListeners.begin(); it != roomListeners.end() ; it++)
    {
        (*it)->onMessageUpdate(chatApi, msg);
    }

    delete msg;
}

void MegaChatApiImpl::fireOnChatListItemUpdate(MegaChatListItem *item)
{
    for(set<MegaChatListener *>::iterator it = listeners.begin(); it != listeners.end() ; it++)
    {
        (*it)->onChatListItemUpdate(chatApi, item);
    }

    delete item;
}

void MegaChatApiImpl::init(MegaChatRequestListener *listener)
{
    MegaChatRequestPrivate *request = new MegaChatRequestPrivate(MegaChatRequest::TYPE_INITIALIZE, listener);
    requestQueue.push(request);
    waiter->notify();
}

void MegaChatApiImpl::connect(MegaChatRequestListener *listener)
{
    MegaChatRequestPrivate *request = new MegaChatRequestPrivate(MegaChatRequest::TYPE_CONNECT, listener);
    requestQueue.push(request);
    waiter->notify();
}

void MegaChatApiImpl::logout(MegaChatRequestListener *listener)
{
    MegaChatRequestPrivate *request = new MegaChatRequestPrivate(MegaChatRequest::TYPE_LOGOUT, listener);
    request->setFlag(true);
    requestQueue.push(request);
    waiter->notify();
}

void MegaChatApiImpl::localLogout(MegaChatRequestListener *listener)
{
    MegaChatRequestPrivate *request = new MegaChatRequestPrivate(MegaChatRequest::TYPE_LOGOUT, listener);
    request->setFlag(false);
    requestQueue.push(request);
    waiter->notify();
}

void MegaChatApiImpl::setOnlineStatus(int status, MegaChatRequestListener *listener)
{
    MegaChatRequestPrivate *request = new MegaChatRequestPrivate(MegaChatRequest::TYPE_SET_ONLINE_STATUS, listener);
    request->setNumber(status);
    requestQueue.push(request);
    waiter->notify();
}

int MegaChatApiImpl::getOnlineStatus()
{
    return status;
}

MegaChatRoomList *MegaChatApiImpl::getChatRooms()
{
    MegaChatRoomListPrivate *chats = new MegaChatRoomListPrivate();

    sdkMutex.lock();

    ChatRoomList::iterator it;
    for (it = mClient->chats->begin(); it != mClient->chats->end(); it++)
    {
        chats->addChatRoom(new MegaChatRoomPrivate(*it->second));
    }

    sdkMutex.unlock();

    return chats;
}

MegaChatRoom *MegaChatApiImpl::getChatRoom(MegaChatHandle chatid)
{
    MegaChatRoomPrivate *chat = NULL;

    sdkMutex.lock();

    ChatRoom *chatRoom = findChatRoom(chatid);
    if (chatRoom)
    {
        chat = new MegaChatRoomPrivate(*chatRoom);
    }

    sdkMutex.unlock();

    return chat;
}

MegaChatRoom *MegaChatApiImpl::getChatRoomByUser(MegaChatHandle userhandle)
{
    MegaChatRoomPrivate *chat = NULL;

    sdkMutex.lock();

    ChatRoom *chatRoom = findChatRoomByUser(userhandle);
    if (chatRoom)
    {
        chat = new MegaChatRoomPrivate(*chatRoom);
    }

    sdkMutex.unlock();

    return chat;
}

void MegaChatApiImpl::createChat(bool group, MegaChatPeerList *peerList, MegaChatRequestListener *listener)
{
    MegaChatRequestPrivate *request = new MegaChatRequestPrivate(MegaChatRequest::TYPE_CREATE_CHATROOM, listener);
    request->setFlag(group);
    request->setMegaChatPeerList(peerList);
    requestQueue.push(request);
    waiter->notify();
}

void MegaChatApiImpl::inviteToChat(MegaChatHandle chatid, MegaChatHandle uh, int privilege, MegaChatRequestListener *listener)
{
    MegaChatRequestPrivate *request = new MegaChatRequestPrivate(MegaChatRequest::TYPE_INVITE_TO_CHATROOM, listener);
    request->setChatHandle(chatid);
    request->setUserHandle(uh);
    request->setPrivilege(privilege);
    requestQueue.push(request);
    waiter->notify();
}

void MegaChatApiImpl::removeFromChat(MegaChatHandle chatid, MegaChatHandle uh, MegaChatRequestListener *listener)
{
    MegaChatRequestPrivate *request = new MegaChatRequestPrivate(MegaChatRequest::TYPE_REMOVE_FROM_CHATROOM, listener);
    request->setChatHandle(chatid);
    request->setUserHandle(uh);
    requestQueue.push(request);
    waiter->notify();
}

void MegaChatApiImpl::updateChatPermissions(MegaChatHandle chatid, MegaChatHandle uh, int privilege, MegaChatRequestListener *listener)
{
    MegaChatRequestPrivate *request = new MegaChatRequestPrivate(MegaChatRequest::TYPE_UPDATE_PEER_PERMISSIONS, listener);
    request->setChatHandle(chatid);
    request->setUserHandle(uh);
    request->setPrivilege(privilege);
    requestQueue.push(request);
    waiter->notify();
}

void MegaChatApiImpl::truncateChat(MegaChatHandle chatid, MegaChatHandle messageid, MegaChatRequestListener *listener)
{
    MegaChatRequestPrivate *request = new MegaChatRequestPrivate(MegaChatRequest::TYPE_TRUNCATE_HISTORY, listener);
    request->setChatHandle(chatid);
    request->setUserHandle(messageid);
    requestQueue.push(request);
    waiter->notify();
}

void MegaChatApiImpl::setChatTitle(MegaChatHandle chatid, const char *title, MegaChatRequestListener *listener)
{
    MegaChatRequestPrivate *request = new MegaChatRequestPrivate(MegaChatRequest::TYPE_EDIT_CHATROOM_NAME, listener);
    request->setChatHandle(chatid);
    request->setText(title);
    requestQueue.push(request);
    waiter->notify();
}

bool MegaChatApiImpl::openChatRoom(MegaChatHandle chatid, MegaChatRoomListener *listener)
{    
    sdkMutex.lock();

    ChatRoom *chatroom = findChatRoom(chatid);
    if (chatroom)
    {
        chatroom->setAppChatHandler(getChatRoomHandler(chatid));
        addChatRoomListener(chatid, listener);
    }

    sdkMutex.unlock();
    return chatroom;
}

void MegaChatApiImpl::closeChatRoom(MegaChatHandle chatid, MegaChatRoomListener *listener)
{
    sdkMutex.lock();

    ChatRoom *chatroom = findChatRoom(chatid);
    if (chatroom)
    {
        chatroom->removeAppChatHandler();
        removeChatRoomHandler(chatid);
        removeChatRoomListener(listener);
    }

    sdkMutex.unlock();
}

int MegaChatApiImpl::loadMessages(MegaChatHandle chatid, int count)
{
    int ret = MegaChatApi::SOURCE_NONE;
    sdkMutex.lock();

    ChatRoom *chatroom = findChatRoom(chatid);
    if (chatroom)
    {
        Chat &chat = chatroom->chat();
        HistSource source = chat.getHistory(count);
        switch (source)
        {
        case kHistSourceNone:   ret = MegaChatApi::SOURCE_NONE; break;
        case kHistSourceRam:
        case kHistSourceDb:     ret = MegaChatApi::SOURCE_LOCAL; break;
        case kHistSourceServer: ret = MegaChatApi::SOURCE_REMOTE; break;
        default:
            API_LOG_ERROR("Unknown source of messages at loadMessages()");
            break;
        }
    }

    sdkMutex.unlock();
    return ret;
}

bool MegaChatApiImpl::isFullHistoryLoaded(MegaChatHandle chatid)
{
    bool ret = false;
    sdkMutex.lock();

    ChatRoom *chatroom = findChatRoom(chatid);
    if (chatroom)
    {
        Chat &chat = chatroom->chat();
        ret = chat.haveAllHistoryNotified();
    }

    sdkMutex.unlock();
    return ret;
}

MegaChatMessage *MegaChatApiImpl::getMessage(MegaChatHandle chatid, MegaChatHandle msgid)
{
    MegaChatMessagePrivate *megaMsg = NULL;
    sdkMutex.lock();

    ChatRoom *chatroom = findChatRoom(chatid);
    if (chatroom)
    {
        Chat &chat = chatroom->chat();
        Idx index = chat.msgIndexFromId(msgid);
        if (index != CHATD_IDX_INVALID)
        {
            Message *msg = chat.findOrNull(index);
            if (msg)    // probably redundant
            {
                megaMsg = new MegaChatMessagePrivate(*msg, chat.getMsgStatus(*msg, index), index);
            }
            else
            {
                API_LOG_ERROR("Failed to find message by index, being index retrieved from message id (index: %d, id: %d)", index, msgid);
            }
        }
    }
    else
    {
        API_LOG_ERROR("Chatroom not found (chatid: %d)", chatid);
    }

    sdkMutex.unlock();
    return megaMsg;
}

MegaChatMessage *MegaChatApiImpl::sendMessage(MegaChatHandle chatid, const char *msg)
{
    if (!msg)
    {
        return NULL;
    }

    MegaChatMessagePrivate *megaMsg = NULL;
    sdkMutex.lock();

    ChatRoom *chatroom = findChatRoom(chatid);
    if (chatroom)
    {
        Chat &chat = chatroom->chat();
        Message::Type t = (Message::Type) MegaChatMessage::TYPE_NORMAL;
        Message *m = chat.msgSubmit(msg, strlen(msg), t, NULL);

        megaMsg = new MegaChatMessagePrivate(*m, Message::Status::kSending, CHATD_IDX_INVALID);
    }

    sdkMutex.unlock();
    return megaMsg;    
}

MegaChatMessage *MegaChatApiImpl::editMessage(MegaChatHandle chatid, MegaChatHandle msgid, const char *msg)
{
    MegaChatMessagePrivate *megaMsg = NULL;
    sdkMutex.lock();

    ChatRoom *chatroom = findChatRoom(chatid);
    if (chatroom)
    {
        Chat &chat = chatroom->chat();
        Message *originalMsg = findMessage(chatid, msgid);
        Idx index;
        if (originalMsg)
        {
            index = chat.msgIndexFromId(msgid);
        }
        else   // message may not have an index yet (not confirmed)
        {
            index = MEGACHAT_INVALID_INDEX;
            originalMsg = findMessageNotConfirmed(chatid, msgid);   // find by transactional id
        }

        if (originalMsg)
        {
            const Message *editedMsg = chat.msgModify(*originalMsg, msg, msg ? strlen(msg) : 0, NULL);
            if (editedMsg)
            {
                megaMsg = new MegaChatMessagePrivate(*editedMsg, Message::Status::kSending, index);
            }
        }
    }

    sdkMutex.unlock();
    return megaMsg;
}

bool MegaChatApiImpl::setMessageSeen(MegaChatHandle chatid, MegaChatHandle msgid)
{
    bool ret = false;

    sdkMutex.lock();

    ChatRoom *chatroom = findChatRoom(chatid);
    if (chatroom)
    {
        ret = chatroom->chat().setMessageSeen(msgid);
    }    

    sdkMutex.unlock();

    return ret;
}

MegaChatMessage *MegaChatApiImpl::getLastMessageSeen(MegaChatHandle chatid)
{
    MegaChatMessagePrivate *megaMsg = NULL;

    sdkMutex.lock();

    ChatRoom *chatroom = findChatRoom(chatid);
    if (chatroom)
    {
        Chat &chat = chatroom->chat();
        Idx index = chat.lastSeenIdx();
        if (index != CHATD_IDX_INVALID)
        {
            const Message *msg = chat.findOrNull(index);
            if (msg)
            {
                Message::Status status = chat.getMsgStatus(*msg, index);
                megaMsg = new MegaChatMessagePrivate(*msg, status, index);
            }
        }
    }

    sdkMutex.unlock();

    return megaMsg;
}

MegaStringList *MegaChatApiImpl::getChatAudioInDevices()
{
    return NULL;
}

MegaStringList *MegaChatApiImpl::getChatVideoInDevices()
{
    return NULL;
}

bool MegaChatApiImpl::setChatAudioInDevice(const char *device)
{
    return true;
}

bool MegaChatApiImpl::setChatVideoInDevice(const char *device)
{
    return true;
}

void MegaChatApiImpl::startChatCall(MegaUser *peer, bool enableVideo, MegaChatRequestListener *listener)
{

}

void MegaChatApiImpl::answerChatCall(MegaChatCall *call, bool accept, MegaChatRequestListener *listener)
{

}

void MegaChatApiImpl::hangAllChatCalls()
{

}

void MegaChatApiImpl::addChatCallListener(MegaChatCallListener *listener)
{
    if (!listener)
    {
        return;
    }

    sdkMutex.lock();
    callListeners.insert(listener);
    sdkMutex.unlock();

}

void MegaChatApiImpl::addChatRequestListener(MegaChatRequestListener *listener)
{
    if (!listener)
    {
        return;
    }

    sdkMutex.lock();
    requestListeners.insert(listener);
    sdkMutex.unlock();
}

void MegaChatApiImpl::addChatLocalVideoListener(MegaChatVideoListener *listener)
{
    if (!listener)
    {
        return;
    }

    sdkMutex.lock();
    localVideoListeners.insert(listener);
    sdkMutex.unlock();
}

void MegaChatApiImpl::addChatRemoteVideoListener(MegaChatVideoListener *listener)
{
    if (!listener)
    {
        return;
    }

    sdkMutex.lock();
    remoteVideoListeners.insert(listener);
    sdkMutex.unlock();
}

void MegaChatApiImpl::addChatListener(MegaChatListener *listener)
{
    if (!listener)
    {
        return;
    }

    sdkMutex.lock();
    listeners.insert(listener);
    sdkMutex.unlock();
}

void MegaChatApiImpl::addChatRoomListener(MegaChatHandle chatid, MegaChatRoomListener *listener)
{
    if (!listener || chatid == MEGACHAT_INVALID_HANDLE)
    {
        return;
    }

    sdkMutex.lock();
    roomListeners.insert(listener);
    sdkMutex.unlock();
}

void MegaChatApiImpl::removeChatCallListener(MegaChatCallListener *listener)
{
    if (!listener)
    {
        return;
    }

    sdkMutex.lock();
    callListeners.erase(listener);
    sdkMutex.unlock();
}

void MegaChatApiImpl::removeChatRequestListener(MegaChatRequestListener *listener)
{
    if (!listener)
    {
        return;
    }

    sdkMutex.lock();
    requestListeners.erase(listener);

    map<int,MegaChatRequestPrivate*>::iterator it = requestMap.begin();
    while (it != requestMap.end())
    {
        MegaChatRequestPrivate* request = it->second;
        if(request->getListener() == listener)
        {
            request->setListener(NULL);
        }

        it++;
    }

    requestQueue.removeListener(listener);
    sdkMutex.unlock();
}

void MegaChatApiImpl::removeChatLocalVideoListener(MegaChatVideoListener *listener)
{
    if (!listener)
    {
        return;
    }

    sdkMutex.lock();
    localVideoListeners.erase(listener);
    sdkMutex.unlock();
}

void MegaChatApiImpl::removeChatRemoteVideoListener(MegaChatVideoListener *listener)
{
    if (!listener)
    {
        return;
    }

    sdkMutex.lock();
    remoteVideoListeners.erase(listener);
    sdkMutex.unlock();
}

void MegaChatApiImpl::removeChatListener(MegaChatListener *listener)
{
    if (!listener)
    {
        return;
    }

    sdkMutex.lock();
    listeners.erase(listener);
    sdkMutex.unlock();
}

void MegaChatApiImpl::removeChatRoomListener(MegaChatRoomListener *listener)
{
    if (!listener)
    {
        return;
    }

    sdkMutex.lock();
    roomListeners.erase(listener);
    sdkMutex.unlock();
}

IApp::IChatHandler *MegaChatApiImpl::createChatHandler(ChatRoom &room)
{
    return getChatRoomHandler(room.chatid());
}

IApp::IContactListHandler *MegaChatApiImpl::contactListHandler()
{
    return nullptr;
}

IApp::IChatListHandler *MegaChatApiImpl::chatListHandler()
{
    return this;
}

void MegaChatApiImpl::onIncomingContactRequest(const MegaContactRequest &req)
{
    // it is notified to the app by the existing MegaApi
}

rtcModule::IEventHandler *MegaChatApiImpl::onIncomingCall(const shared_ptr<rtcModule::ICallAnswer> &ans)
{
    // TODO: create the call object implementing IEventHandler and return it
    return new MegaChatCallPrivate(ans);
}

void MegaChatApiImpl::notifyInvited(const ChatRoom &room)
{
    MegaChatRoomPrivate *chat = new MegaChatRoomPrivate(room);

    fireOnChatRoomUpdate(chat);
}

void MegaChatApiImpl::onTerminate()
{
    API_LOG_DEBUG("Karere is about to terminate (call onTerminate())");
}

IApp::IGroupChatListItem *MegaChatApiImpl::addGroupChatItem(GroupChatRoom &room)
{
    MegaChatGroupListItemHandler *itemHandler = new MegaChatGroupListItemHandler(*this, room.chatid());
    chatGroupListItemHandler.insert(itemHandler);

    return itemHandler;
}

IApp::IPeerChatListItem *MegaChatApiImpl::addPeerChatItem(PeerChatRoom &room)
{
    MegaChatPeerListItemHandler *itemHandler = new MegaChatPeerListItemHandler(*this, room.chatid());
    chatPeerListItemHandler.insert(itemHandler);

    return itemHandler;
}

void MegaChatApiImpl::removeGroupChatItem(IGroupChatListItem &item)
{
    chatGroupListItemHandler.erase(&item);
}

void MegaChatApiImpl::removePeerChatItem(IPeerChatListItem &item)
{
    chatPeerListItemHandler.erase(&item);
}

void MegaChatApiImpl::onRequestFinish(MegaApi *api, MegaRequest *request, MegaError *e)
{
    if (e->getErrorCode() != MegaError::API_OK)
    {
        return;
    }

    switch (request->getType())
    {
        case MegaRequest::TYPE_LOGIN:
            resumeSession = request->getSessionKey();
            break;

        case MegaRequest::TYPE_FETCH_NODES:
            api->pauseActionPackets();
            marshallCall([this, api]()
            {
                mClient->init(resumeSession)
                .then([this, api]()
                {
                    initResult = new MegaChatErrorPrivate(MegaChatError::ERROR_OK);
                    if (initRequest)
                    {
                        fireOnChatRequestFinish(initRequest, initResult);
                        API_LOG_INFO("Initialization complete");
                        fireOnChatRoomUpdate(NULL);
                        initRequest = NULL;
                        initResult = NULL;
                    }
                    api->resumeActionPackets();
                })
                .fail([this, api](const promise::Error& e)
                {
                    initResult = new MegaChatErrorPrivate(e.msg(), e.code(), e.type());
                    if (initRequest)
                    {
                        fireOnChatRequestFinish(initRequest, initResult);
                        API_LOG_INFO("Initialization failed");
                        initRequest = NULL;
                        initResult = NULL;
                    }
                    api->resumeActionPackets();
                });
            });
            break;
    }
}

void MegaChatApiImpl::onOwnPresence(Presence pres)
{
    if (pres.status() == this->status)
    {
        API_LOG_DEBUG("onOwnPresence() notifies the same status: %s (flags: %d)", pres.toString(), pres.flags());
        return;
    }

    this->status = pres.status();

    MegaChatListItemPrivate *item = new MegaChatListItemPrivate(MEGACHAT_INVALID_HANDLE);
    item->setOnlineStatus(status);

    API_LOG_INFO("My own presence has changed to %s (flags: %d)", pres.toString(), pres.flags());

    fireOnChatListItemUpdate(item);
}

ChatRequestQueue::ChatRequestQueue()
{
    mutex.init(false);
}

void ChatRequestQueue::push(MegaChatRequestPrivate *request)
{
    mutex.lock();
    requests.push_back(request);
    mutex.unlock();
}

void ChatRequestQueue::push_front(MegaChatRequestPrivate *request)
{
    mutex.lock();
    requests.push_front(request);
    mutex.unlock();
}

MegaChatRequestPrivate *ChatRequestQueue::pop()
{
    mutex.lock();
    if(requests.empty())
    {
        mutex.unlock();
        return NULL;
    }
    MegaChatRequestPrivate *request = requests.front();
    requests.pop_front();
    mutex.unlock();
    return request;
}

void ChatRequestQueue::removeListener(MegaChatRequestListener *listener)
{
    mutex.lock();

    deque<MegaChatRequestPrivate *>::iterator it = requests.begin();
    while(it != requests.end())
    {
        MegaChatRequestPrivate *request = (*it);
        if(request->getListener()==listener)
            request->setListener(NULL);
        it++;
    }

    mutex.unlock();
}

EventQueue::EventQueue()
{
    mutex.init(false);
}

void EventQueue::push(void *transfer)
{
    mutex.lock();
    events.push_back(transfer);
    mutex.unlock();
}

void EventQueue::push_front(void *event)
{
    mutex.lock();
    events.push_front(event);
    mutex.unlock();
}

void* EventQueue::pop()
{
    mutex.lock();
    if(events.empty())
    {
        mutex.unlock();
        return NULL;
    }
    void* event = events.front();
    events.pop_front();
    mutex.unlock();
    return event;
}

MegaChatRequestPrivate::MegaChatRequestPrivate(int type, MegaChatRequestListener *listener)
{
    this->type = type;
    this->tag = 0;
    this->listener = listener;

    this->number = 0;
    this->retry = 0;
    this->flag = false;
    this->peerList = NULL;
    this->chatid = MEGACHAT_INVALID_HANDLE;
    this->userHandle = MEGACHAT_INVALID_HANDLE;
    this->privilege = MegaChatPeerList::PRIV_UNKNOWN;
    this->text = NULL;
}

MegaChatRequestPrivate::MegaChatRequestPrivate(MegaChatRequestPrivate &request)
{
    this->text = NULL;
    this->peerList = NULL;

    this->type = request.getType();
    this->listener = request.getListener();
    this->setTag(request.getTag());
    this->setNumber(request.getNumber());
    this->setNumRetry(request.getNumRetry());
    this->setFlag(request.getFlag());
    this->setMegaChatPeerList(request.getMegaChatPeerList());
    this->setChatHandle(request.getChatHandle());
    this->setUserHandle(request.getUserHandle());
    this->setPrivilege(request.getPrivilege());
    this->setText(request.getText());
}

MegaChatRequestPrivate::~MegaChatRequestPrivate()
{
    delete peerList;
    delete [] text;
}

MegaChatRequest *MegaChatRequestPrivate::copy()
{
    return new MegaChatRequestPrivate(*this);
}

const char *MegaChatRequestPrivate::getRequestString() const
{
    switch(type)
    {
        case TYPE_DELETE: return "DELETE";
        case TYPE_LOGOUT: return "LOGOUT";
        case TYPE_CONNECT: return "CONNECT";
        case TYPE_INITIALIZE: return "INITIALIZE";
        case TYPE_SET_ONLINE_STATUS: return "SET_CHAT_STATUS";
        case TYPE_CREATE_CHATROOM: return "CREATE CHATROOM";
        case TYPE_INVITE_TO_CHATROOM: return "INVITE_TO_CHATROOM";
        case TYPE_REMOVE_FROM_CHATROOM: return "REMOVE_FROM_CHATROOM";
        case TYPE_UPDATE_PEER_PERMISSIONS: return "UPDATE_PEER_PERMISSIONS";
        case TYPE_TRUNCATE_HISTORY: return "TRUNCATE_HISTORY";
        case TYPE_EDIT_CHATROOM_NAME: return "EDIT_CHATROOM_NAME";

        case TYPE_START_CHAT_CALL: return "START_CHAT_CALL";
        case TYPE_ANSWER_CHAT_CALL: return "ANSWER_CHAT_CALL";
    }
    return "UNKNOWN";
}

const char *MegaChatRequestPrivate::toString() const
{
    return getRequestString();
}

MegaChatRequestListener *MegaChatRequestPrivate::getListener() const
{
    return listener;
}

int MegaChatRequestPrivate::getType() const
{
    return type;
}

long long MegaChatRequestPrivate::getNumber() const
{
    return number;
}

int MegaChatRequestPrivate::getNumRetry() const
{
    return retry;
}

bool MegaChatRequestPrivate::getFlag() const
{
    return flag;
}

MegaChatPeerList *MegaChatRequestPrivate::getMegaChatPeerList()
{
    return peerList;
}

MegaChatHandle MegaChatRequestPrivate::getChatHandle()
{
    return chatid;
}

MegaChatHandle MegaChatRequestPrivate::getUserHandle()
{
    return userHandle;
}

int MegaChatRequestPrivate::getPrivilege()
{
    return privilege;
}

const char *MegaChatRequestPrivate::getText() const
{
    return text;
}

int MegaChatRequestPrivate::getTag() const
{
    return tag;
}

void MegaChatRequestPrivate::setListener(MegaChatRequestListener *listener)
{
    this->listener = listener;
}

void MegaChatRequestPrivate::setTag(int tag)
{
    this->tag = tag;
}

void MegaChatRequestPrivate::setNumber(long long number)
{
    this->number = number;
}

void MegaChatRequestPrivate::setNumRetry(int retry)
{
    this->retry = retry;
}

void MegaChatRequestPrivate::setFlag(bool flag)
{
    this->flag = flag;
}

void MegaChatRequestPrivate::setMegaChatPeerList(MegaChatPeerList *peerList)
{
    if (this->peerList)
        delete this->peerList;

    this->peerList = peerList ? peerList->copy() : NULL;
}

void MegaChatRequestPrivate::setChatHandle(MegaChatHandle chatid)
{
    this->chatid = chatid;
}

void MegaChatRequestPrivate::setUserHandle(MegaChatHandle userhandle)
{
    this->userHandle = userhandle;
}

void MegaChatRequestPrivate::setPrivilege(int priv)
{
    this->privilege = priv;
}

void MegaChatRequestPrivate::setText(const char *text)
{
    if(this->text)
    {
        delete [] this->text;
    }
    this->text = MegaApi::strdup(text);
}

MegaChatCallPrivate::MegaChatCallPrivate(const shared_ptr<rtcModule::ICallAnswer> &ans)
{
    mAns = ans;
    this->peer = mAns->call()->peerJid().c_str();
    status = 0;
    tag = 0;
    videoReceiver = NULL;
}

MegaChatCallPrivate::MegaChatCallPrivate(const char *peer)
{
    this->peer = MegaApi::strdup(peer);
    status = 0;
    tag = 0;
    videoReceiver = NULL;
    mAns = NULL;
}

MegaChatCallPrivate::MegaChatCallPrivate(const MegaChatCallPrivate &call)
{
    this->peer = MegaApi::strdup(call.getPeer());
    this->status = call.getStatus();
    this->tag = call.getTag();
    this->videoReceiver = NULL;
    mAns = NULL;
}

MegaChatCallPrivate::~MegaChatCallPrivate()
{
    delete [] peer;
//    delete mAns;  it's a shared pointer, no need to delete
    // videoReceiver is deleted on onVideoDetach, because it's called after the call is finished
}

MegaChatCall *MegaChatCallPrivate::copy()
{
    return new MegaChatCallPrivate(*this);
}

int MegaChatCallPrivate::getStatus() const
{
    return status;
}

int MegaChatCallPrivate::getTag() const
{
    return tag;
}

MegaChatHandle MegaChatCallPrivate::getContactHandle() const
{
    if(!peer)
    {
        return MEGACHAT_INVALID_HANDLE;
    }

    MegaChatHandle userHandle = MEGACHAT_INVALID_HANDLE;
    string tmp = peer;
    tmp.resize(13);
    Base32::atob(tmp.data(), (byte *)&userHandle, sizeof(userHandle));
    return userHandle;
}

//shared_ptr<rtcModule::ICallAnswer> MegaChatCallPrivate::getAnswerObject()
//{
//    return mAns;
//}

const char *MegaChatCallPrivate::getPeer() const
{
    return peer;
}

void MegaChatCallPrivate::setStatus(int status)
{
    this->status = status;
}

void MegaChatCallPrivate::setTag(int tag)
{
    this->tag = tag;
}

void MegaChatCallPrivate::setVideoReceiver(MegaChatVideoReceiver *videoReceiver)
{
    delete this->videoReceiver;
    this->videoReceiver = videoReceiver;
}

shared_ptr<rtcModule::ICall> MegaChatCallPrivate::call() const
{
    return nullptr;
}

bool MegaChatCallPrivate::reqStillValid() const
{
    return false;
}

set<string> *MegaChatCallPrivate::files() const
{
    return NULL;
}

AvFlags MegaChatCallPrivate::peerMedia() const
{
    AvFlags ret;
    return ret;
}

bool MegaChatCallPrivate::answer(bool accept, AvFlags ownMedia)
{
    return false;
}

//void MegaChatCallPrivate::setAnswerObject(rtcModule::ICallAnswer *answerObject)
//{
//    this->mAns = answerObject;
//}

MegaChatVideoReceiver::MegaChatVideoReceiver(MegaChatApiImpl *chatApi, MegaChatCallPrivate *call, bool local)
{
    this->chatApi = chatApi;
    this->call = call;
    this->local = local;
}

MegaChatVideoReceiver::~MegaChatVideoReceiver()
{
}

unsigned char *MegaChatVideoReceiver::getImageBuffer(unsigned short width, unsigned short height, void **userData)
{
    MegaChatVideoFrame *frame = new MegaChatVideoFrame;
    frame->width = width;
    frame->height = height;
    frame->buffer = new byte[width * height * 4];  // in format ARGB: 4 bytes per pixel
    *userData = frame;
    return frame->buffer;
}

void MegaChatVideoReceiver::frameComplete(void *userData)
{
    MegaChatVideoFrame *frame = (MegaChatVideoFrame *)userData;
    if(local)
    {
        chatApi->fireOnChatLocalVideoData(call, frame->width, frame->height, (char *)frame->buffer);
    }
    else
    {
        chatApi->fireOnChatRemoteVideoData(call, frame->width, frame->height, (char *)frame->buffer);
    }
    delete frame->buffer;
    delete frame;
}

void MegaChatVideoReceiver::onVideoAttach()
{

}

void MegaChatVideoReceiver::onVideoDetach()
{
    delete this;
}

void MegaChatVideoReceiver::clearViewport()
{
}

void MegaChatVideoReceiver::released()
{

}


MegaChatRoomHandler::MegaChatRoomHandler(MegaChatApiImpl *chatApi, MegaChatHandle chatid)
{
    this->chatApi = chatApi;
    this->chatid = chatid;

    this->mRoom = NULL;
    this->mChat = NULL;
}

IApp::ICallHandler *MegaChatRoomHandler::callHandler()
{
    // TODO: create a MegaChatCallPrivate() with the peer information and return it
    return NULL;
}

void MegaChatRoomHandler::onTitleChanged(const string &title)
{
    MegaChatRoomPrivate *chat = (MegaChatRoomPrivate *) chatApi->getChatRoom(chatid);
    chat->setTitle(title.c_str());

    chatApi->fireOnChatRoomUpdate(chat);
}

void MegaChatRoomHandler::onUnreadCountChanged(int count)
{
    MegaChatRoomPrivate *chat = (MegaChatRoomPrivate *) chatApi->getChatRoom(chatid);
    chat->setUnreadCount(count);

    chatApi->fireOnChatRoomUpdate(chat);
}

void MegaChatRoomHandler::onPresenceChanged(Presence state)
{
    MegaChatRoomPrivate *chat = (MegaChatRoomPrivate *) chatApi->getChatRoom(chatid);
    chat->setOnlineStatus(state.status());

    chatApi->fireOnChatRoomUpdate(chat);
}

void MegaChatRoomHandler::init(Chat &chat, DbInterface *&)
{
    mChat = &chat;
    mRoom = chatApi->findChatRoom(chatid);

    mChat->resetListenerState();
}

void MegaChatRoomHandler::onDestroy()
{
    mChat = NULL;
    mRoom = NULL;
}

void MegaChatRoomHandler::onRecvNewMessage(Idx idx, Message &msg, Message::Status status)
{
    // forward the event to the chatroom, so chatlist items also receive the notification
    if (mRoom)
    {
        mRoom->onRecvNewMessage(idx, msg, status);
    }

    MegaChatMessagePrivate *message = new MegaChatMessagePrivate(msg, status, idx);
    chatApi->fireOnMessageReceived(message);
}

void MegaChatRoomHandler::onRecvHistoryMessage(Idx idx, Message &msg, Message::Status status, bool isLocal)
{
    MegaChatMessagePrivate *message = new MegaChatMessagePrivate(msg, status, idx);
    chatApi->fireOnMessageLoaded(message);
}

void MegaChatRoomHandler::onHistoryDone(chatd::HistSource /*source*/)
{
    chatApi->fireOnMessageLoaded(NULL);
}

void MegaChatRoomHandler::onUnsentMsgLoaded(chatd::Message &msg)
{
    Message::Status status = (Message::Status) MegaChatMessage::STATUS_SENDING;
    MegaChatMessagePrivate *message = new MegaChatMessagePrivate(msg, status, MEGACHAT_INVALID_INDEX);
    chatApi->fireOnMessageLoaded(message);
}

void MegaChatRoomHandler::onUnsentEditLoaded(chatd::Message &msg, bool oriMsgIsSending)
{
    Message::Status status = (Message::Status) MegaChatMessage::STATUS_SENDING;
    Idx index = MEGACHAT_INVALID_INDEX;
    if (!oriMsgIsSending)   // original message was already sent
    {
        index = mChat->msgIndexFromId(msg.id());
    }
    MegaChatMessagePrivate *message = new MegaChatMessagePrivate(msg, status, index);
    message->setContentChanged();
    chatApi->fireOnMessageLoaded(message);
}

void MegaChatRoomHandler::onMessageConfirmed(Id msgxid, const Message &msg, Idx idx)
{
    Message::Status status = mChat->getMsgStatus(msg, idx);
    MegaChatMessagePrivate *message = new MegaChatMessagePrivate(msg, status, idx);
    message->setStatus(status);
    message->setTempId(msgxid);     // to allow the app to find the "temporal" message
    chatApi->fireOnMessageUpdate(message);
}

void MegaChatRoomHandler::onMessageRejected(const Message &msg)
{
    Message::Status status = (Message::Status) MegaChatMessage::STATUS_SERVER_REJECTED;
    MegaChatMessagePrivate *message = new MegaChatMessagePrivate(msg, status, MEGACHAT_INVALID_INDEX);
    message->setStatus(status);
    chatApi->fireOnMessageUpdate(message);
}

void MegaChatRoomHandler::onMessageStatusChange(Idx idx, Message::Status newStatus, const Message &msg)
{
    MegaChatMessagePrivate *message = new MegaChatMessagePrivate(msg, newStatus, idx);
    message->setStatus(newStatus);
    chatApi->fireOnMessageUpdate(message);
}

void MegaChatRoomHandler::onMessageEdited(const Message &msg, chatd::Idx idx)
{
    Message::Status status = mChat->getMsgStatus(msg, idx);
    MegaChatMessagePrivate *message = new MegaChatMessagePrivate(msg, status, idx);
    message->setContentChanged();
    chatApi->fireOnMessageUpdate(message);
}

void MegaChatRoomHandler::onEditRejected(const Message &msg, bool oriIsConfirmed)
{
    Idx index;
    Message::Status status;

    if (oriIsConfirmed)    // message is confirmed, but edit has been rejected
    {
        index = mChat->msgIndexFromId(msg.id());
        status = mChat->getMsgStatus(msg, index);
    }
    else // both, original message and edit, have been rejected
    {
        index = MEGACHAT_INVALID_INDEX;
        status = Message::kSending;
    }

    MegaChatMessagePrivate *message = new MegaChatMessagePrivate(msg, status, index);
    message->setStatus(status);
    chatApi->fireOnMessageUpdate(message);
}

void MegaChatRoomHandler::onOnlineStateChange(ChatState state)
{
    if (mRoom)
    {
        // forward the event to the chatroom, so chatlist items also receive the notification
        mRoom->onOnlineStateChange(state);

        MegaChatRoomPrivate *chatroom = new MegaChatRoomPrivate(*mRoom);
        chatroom->setOnlineState(state);
        chatApi->fireOnChatRoomUpdate(chatroom);
    }
}

void MegaChatRoomHandler::onUserJoin(Id userid, Priv privilege)
{
    if (mRoom)
    {
        // forward the event to the chatroom, so chatlist items also receive the notification
        mRoom->onUserJoin(userid, privilege);

        MegaChatRoomPrivate *chatroom = new MegaChatRoomPrivate(*mRoom);
        chatroom->setMembersUpdated();
        chatApi->fireOnChatRoomUpdate(chatroom);
    }
}

void MegaChatRoomHandler::onUserLeave(Id userid)
{
    if (mRoom)
    {
        // forward the event to the chatroom, so chatlist items also receive the notification
        mRoom->onUserLeave(userid);

        MegaChatRoomPrivate *chatroom = new MegaChatRoomPrivate(*mRoom);
        chatroom->setMembersUpdated();
        chatApi->fireOnChatRoomUpdate(chatroom);
    }
}

void MegaChatRoomHandler::onUnreadChanged()
{
    if (mRoom)
    {
        // forward the event to the chatroom, so chatlist items also receive the notification
        mRoom->onUnreadChanged();

        if (mChat)
        {
            MegaChatRoomPrivate *chatroom = new MegaChatRoomPrivate(*mRoom);
            chatroom->setUnreadCount(mChat->unreadMsgCount());
            chatApi->fireOnChatRoomUpdate(chatroom);
        }
    }
}

void MegaChatRoomHandler::onManualSendRequired(chatd::Message *msg, uint64_t /*id*/, chatd::ManualSendReason /*reason*/)
{
    Idx index = mChat->msgIndexFromId(msg->id());
    Message::Status status = (index != MEGACHAT_INVALID_INDEX) ? mChat->getMsgStatus(*msg, index) : Message::kSending;
    MegaChatMessagePrivate *message = new MegaChatMessagePrivate(*msg, status, index);
    delete msg; // we take ownership of the Message

    message->setStatus(status);
    chatApi->fireOnMessageUpdate(message);
}


MegaChatErrorPrivate::MegaChatErrorPrivate(const string &msg, int code, int type)
    : promise::Error(msg, code, type)
{

}

MegaChatErrorPrivate::MegaChatErrorPrivate(int code, int type)
    : promise::Error(MegaChatErrorPrivate::getGenericErrorString(code), code, type)
{
}

const char* MegaChatErrorPrivate::getGenericErrorString(int errorCode)
{
    switch(errorCode)
    {
    case ERROR_OK:
        return "No error";
    case ERROR_ARGS:
        return "Invalid argument";
    case ERROR_UNKNOWN:
    default:
        return "Unknown error";
    }
}


MegaChatErrorPrivate::MegaChatErrorPrivate(const MegaChatErrorPrivate *error)
    : promise::Error(error->getErrorString(), error->getErrorCode(), error->getErrorType())
{

}

int MegaChatErrorPrivate::getErrorCode() const
{
    return code();
}

int MegaChatErrorPrivate::getErrorType() const
{
    return type();
}

const char *MegaChatErrorPrivate::getErrorString() const
{
    return what();
}

const char *MegaChatErrorPrivate::toString() const
{
    char *errorString = new char[msg().size()+1];
    strcpy(errorString, what());
    return errorString;
}

MegaChatError *MegaChatErrorPrivate::copy()
{
    return new MegaChatErrorPrivate(this);
}


MegaChatRoomListPrivate::MegaChatRoomListPrivate()
{

}

MegaChatRoomListPrivate::MegaChatRoomListPrivate(const MegaChatRoomListPrivate *list)
{
    MegaChatRoomPrivate *chat;

    for (unsigned int i = 0; i < list->size(); i++)
    {
        chat = new MegaChatRoomPrivate(list->get(i));
        this->list.push_back(chat);
    }
}

MegaChatRoomList *MegaChatRoomListPrivate::copy() const
{
    return new MegaChatRoomListPrivate(this);
}

const MegaChatRoom *MegaChatRoomListPrivate::get(unsigned int i) const
{
    if (i >= size())
    {
        return NULL;
    }
    else
    {
        return list.at(i);
    }
}

unsigned int MegaChatRoomListPrivate::size() const
{
    return list.size();
}

void MegaChatRoomListPrivate::addChatRoom(MegaChatRoom *chat)
{
    list.push_back(chat);
}


MegaChatRoomPrivate::MegaChatRoomPrivate(const MegaChatRoom *chat)
{
    this->chatid = chat->getChatId();
    this->priv = chat->getOwnPrivilege();
    for (unsigned int i = 0; i < chat->getPeerCount(); i++)
    {
        peers.push_back(userpriv_pair(chat->getPeerHandle(i), (privilege_t) chat->getPeerPrivilege(i)));
    }
    this->group = chat->isGroup();
    this->title = chat->getTitle();
    this->chatState = chat->getOnlineState();
    this->unreadCount = chat->getUnreadCount();

    this->changed = chat->getChanges();
}

MegaChatRoomPrivate::MegaChatRoomPrivate(const karere::ChatRoom &chat)
{
    this->changed = 0;
    this->chatid = chat.chatid();
    this->priv = chat.ownPriv();
    this->group = chat.isGroup();
    this->title = chat.titleString().c_str();
    this->chatState = chat.chatdOnlineState();
    this->unreadCount = chat.chat().unreadMsgCount();

    if (group)
    {
        GroupChatRoom &groupchat = (GroupChatRoom&) chat;
        GroupChatRoom::MemberMap peers = groupchat.peers();

        GroupChatRoom::MemberMap::iterator it;
        for (it = peers.begin(); it != peers.end(); it++)
        {
            this->peers.push_back(userpriv_pair(it->first,
                                          (privilege_t) it->second->priv()));
        }
        this->status = chat.chatdOnlineState();
    }
    else
    {
        PeerChatRoom &peerchat = (PeerChatRoom&) chat;
        privilege_t priv = (privilege_t) peerchat.peerPrivilege();
        handle uh = peerchat.peer();

        this->peers.push_back(userpriv_pair(uh, priv));
        this->status = chat.presence().status();
    }
}

MegaChatRoom *MegaChatRoomPrivate::copy() const
{
    return new MegaChatRoomPrivate(this);
}

MegaChatHandle MegaChatRoomPrivate::getChatId() const
{
    return chatid;
}

int MegaChatRoomPrivate::getOwnPrivilege() const
{
    return priv;
}

int MegaChatRoomPrivate::getPeerPrivilegeByHandle(MegaChatHandle userhandle) const
{
    for (unsigned int i = 0; i < peers.size(); i++)
    {
        if (peers.at(i).first == userhandle)
        {
            return peers.at(i).second;
        }
    }

    return PRIV_UNKNOWN;
}

int MegaChatRoomPrivate::getPeerPrivilege(unsigned int i) const
{
    return peers.at(i).second;
}

unsigned int MegaChatRoomPrivate::getPeerCount() const
{
    return peers.size();
}

MegaChatHandle MegaChatRoomPrivate::getPeerHandle(unsigned int i) const
{
    return peers.at(i).first;
}

bool MegaChatRoomPrivate::isGroup() const
{
    return group;
}

const char *MegaChatRoomPrivate::getTitle() const
{
    return title;
}

int MegaChatRoomPrivate::getOnlineState() const
{
    return chatState;
}

int MegaChatRoomPrivate::getChanges() const
{
    return changed;
}

bool MegaChatRoomPrivate::hasChanged(int changeType) const
{
    return (changed & changeType);
}

int MegaChatRoomPrivate::getUnreadCount() const
{
    return unreadCount;
}

int MegaChatRoomPrivate::getOnlineStatus() const
{
    return status;
}

void MegaChatRoomPrivate::setTitle(const char *title)
{
    if(this->title)
    {
        delete [] this->title;
    }
    this->title = MegaApi::strdup(title);
    this->changed |= MegaChatRoom::CHANGE_TYPE_TITLE;
}

void MegaChatRoomPrivate::setUnreadCount(int count)
{
    this->unreadCount = count;
    this->changed |= MegaChatRoom::CHANGE_TYPE_UNREAD_COUNT;
}

void MegaChatRoomPrivate::setOnlineStatus(int status)
{
    this->status = status;
    this->changed |= MegaChatRoom::CHANGE_TYPE_STATUS;
}

void MegaChatRoomPrivate::setMembersUpdated()
{
    this->changed |= MegaChatRoom::CHANGE_TYPE_PARTICIPANTS;
}

void MegaChatRoomPrivate::setOnlineState(int state)
{
    this->chatState = state;
    this->changed |= MegaChatRoom::CHANGE_TYPE_CHAT_STATE;
}

void MegaChatListItemHandler::onVisibilityChanged(int newVisibility)
{
    MegaChatListItemPrivate *item = new MegaChatListItemPrivate(this->chatid);
    item->setVisibility((visibility_t) newVisibility);

    chatApi.fireOnChatListItemUpdate(item);
}

void MegaChatListItemHandler::onTitleChanged(const string &title)
{
    MegaChatListItemPrivate *item = new MegaChatListItemPrivate(this->chatid);
    item->setTitle(title.c_str());

    chatApi.fireOnChatListItemUpdate(item);
}

void MegaChatListItemHandler::onUnreadCountChanged(int count)
{
    MegaChatListItemPrivate *item = new MegaChatListItemPrivate(this->chatid);
    item->setUnreadCount(count);

    chatApi.fireOnChatListItemUpdate(item);
}

void MegaChatListItemHandler::onPresenceChanged(Presence state)
{
    MegaChatListItemPrivate *item = new MegaChatListItemPrivate(this->chatid);
    item->setOnlineStatus(state.status());

    chatApi.fireOnChatListItemUpdate(item);
}

MegaChatPeerListPrivate::MegaChatPeerListPrivate()
{
}

MegaChatPeerListPrivate::~MegaChatPeerListPrivate()
{

}

MegaChatPeerList *MegaChatPeerListPrivate::copy() const
{
    MegaChatPeerListPrivate *ret = new MegaChatPeerListPrivate;

    for (int i = 0; i < size(); i++)
    {
        ret->addPeer(list.at(i).first, list.at(i).second);
    }

    return ret;
}

void MegaChatPeerListPrivate::addPeer(MegaChatHandle h, int priv)
{
    list.push_back(userpriv_pair(h, (privilege_t) priv));
}

MegaChatHandle MegaChatPeerListPrivate::getPeerHandle(int i) const
{
    if (i > size())
    {
        return MEGACHAT_INVALID_HANDLE;
    }
    else
    {
        return list.at(i).first;
    }
}

int MegaChatPeerListPrivate::getPeerPrivilege(int i) const
{
    if (i > size())
    {
        return PRIV_UNKNOWN;
    }
    else
    {
        return list.at(i).second;
    }
}

int MegaChatPeerListPrivate::size() const
{
    return list.size();
}

const userpriv_vector *MegaChatPeerListPrivate::getList() const
{
    return &list;
}

MegaChatPeerListPrivate::MegaChatPeerListPrivate(userpriv_vector *userpriv)
{
    handle uh;
    privilege_t priv;

    for (unsigned i = 0; i < userpriv->size(); i++)
    {
        uh = userpriv->at(i).first;
        priv = userpriv->at(i).second;

        this->addPeer(uh, priv);
    }
}


MegaChatListItemHandler::MegaChatListItemHandler(MegaChatApiImpl &chatApi, MegaChatHandle chatid)
    :chatApi(chatApi), chatid(chatid)
{
}

MegaChatListItemPrivate::MegaChatListItemPrivate(MegaChatHandle chatid)
    : MegaChatListItem()
{
    this->chatid = chatid;
    this->title = NULL;
    this->visibility = VISIBILITY_UNKNOWN;
    this->unreadCount = 0;
    this->status = MegaChatApi::STATUS_OFFLINE;
    this->changed = 0;
}

MegaChatListItemPrivate::~MegaChatListItemPrivate()
{
    delete [] title;
}

MegaChatListItem *MegaChatListItemPrivate::copy() const
{
    return new MegaChatListItemPrivate(chatid);
}

int MegaChatListItemPrivate::getChanges() const
{
    return changed;
}

bool MegaChatListItemPrivate::hasChanged(int changeType) const
{
    return (changed & changeType);
}

MegaChatHandle MegaChatListItemPrivate::getChatId() const
{
    return chatid;
}

const char *MegaChatListItemPrivate::getTitle() const
{
    return title;
}

int MegaChatListItemPrivate::getVisibility() const
{
    return visibility;
}

int MegaChatListItemPrivate::getUnreadCount() const
{
    return unreadCount;
}

int MegaChatListItemPrivate::getOnlineStatus() const
{
    return status;
}

void MegaChatListItemPrivate::setVisibility(visibility_t visibility)
{
    this->visibility = visibility;
    this->changed |= MegaChatListItem::CHANGE_TYPE_VISIBILITY;
}

void MegaChatListItemPrivate::setTitle(const char *title)
{
    if(this->title)
    {
        delete [] this->title;
    }
    this->title = MegaApi::strdup(title);
    this->changed |= MegaChatListItem::CHANGE_TYPE_TITLE;
}

void MegaChatListItemPrivate::setUnreadCount(int count)
{
    this->unreadCount = count;
    this->changed |= MegaChatListItem::CHANGE_TYPE_UNREAD_COUNT;
}

void MegaChatListItemPrivate::setOnlineStatus(int status)
{
    this->status = status;
    this->changed |= MegaChatListItem::CHANGE_TYPE_STATUS;
}

void MegaChatListItemPrivate::setMembersUpdated()
{
    this->changed |= MegaChatListItem::CHANGE_TYPE_PARTICIPANTS;
}


MegaChatGroupListItemHandler::MegaChatGroupListItemHandler(MegaChatApiImpl &chatApi, MegaChatHandle chatid)
    : MegaChatListItemHandler(chatApi, chatid)
{

}

void MegaChatGroupListItemHandler::onUserJoin(uint64_t, Priv)
{
    MegaChatListItemPrivate *item = new MegaChatListItemPrivate(this->chatid);
    item->setMembersUpdated();

    chatApi.fireOnChatListItemUpdate(item);
}

void MegaChatGroupListItemHandler::onUserLeave(uint64_t )
{
    MegaChatListItemPrivate *item = new MegaChatListItemPrivate(this->chatid);
    item->setMembersUpdated();

    chatApi.fireOnChatListItemUpdate(item);
}


MegaChatPeerListItemHandler::MegaChatPeerListItemHandler(MegaChatApiImpl &chatApi, MegaChatHandle chatid)
    : MegaChatListItemHandler(chatApi, chatid)
{

}


MegaChatMessagePrivate::MegaChatMessagePrivate(const MegaChatMessage *msg)
{
    this->msg = MegaApi::strdup(msg->getContent());
    this->uh = msg->getUserHandle();
    this->msgId = msg->getMsgId();
    this->tempId = msg->getTempId();
    this->index = msg->getMsgIndex();
    this->status = msg->getStatus();
    this->ts = msg->getTimestamp();
    this->type = msg->getType();
    this->changed = msg->getChanges();
    this->edited = msg->isEdited();
    this->deleted = msg->isDeleted();
}

MegaChatMessagePrivate::MegaChatMessagePrivate(const Message &msg, Message::Status status, Idx index)
{
    string tmp(msg.buf(), msg.size());
    this->msg = msg.size() ? MegaApi::strdup(tmp.c_str()) : NULL;
    this->uh = msg.userid;
    this->msgId = msg.isSending() ? MEGACHAT_INVALID_HANDLE : (MegaChatHandle) msg.id();
    this->tempId = msg.isSending() ? (MegaChatHandle) msg.id() : MEGACHAT_INVALID_HANDLE;
    this->type = msg.type;
    this->ts = msg.ts;
    this->status = status;
    this->index = index;
    this->changed = 0;
    this->edited = msg.updated && msg.size();
    this->deleted = msg.updated && !msg.size();
}

MegaChatMessagePrivate::~MegaChatMessagePrivate()
{
    delete [] msg;
}

MegaChatMessage *MegaChatMessagePrivate::copy() const
{
    return new MegaChatMessagePrivate(this);
}

int MegaChatMessagePrivate::getStatus() const
{
    return status;
}

MegaChatHandle MegaChatMessagePrivate::getMsgId() const
{
    return msgId;
}

MegaChatHandle MegaChatMessagePrivate::getTempId() const
{
    return tempId;
}

int MegaChatMessagePrivate::getMsgIndex() const
{
    return index;
}

MegaChatHandle MegaChatMessagePrivate::getUserHandle() const
{
    return uh;
}

int MegaChatMessagePrivate::getType() const
{
    return type;
}

int64_t MegaChatMessagePrivate::getTimestamp() const
{
    return ts;
}

const char *MegaChatMessagePrivate::getContent() const
{
    return msg;
}

bool MegaChatMessagePrivate::isEdited() const
{
    return edited;
}

bool MegaChatMessagePrivate::isDeleted() const
{
    return deleted;
}

int MegaChatMessagePrivate::getChanges() const
{
    return changed;
}

bool MegaChatMessagePrivate::hasChanged(int changeType) const
{
    return (changed & changeType);
}

void MegaChatMessagePrivate::setStatus(int status)
{
    this->status = status;
    this->changed |= MegaChatMessage::CHANGE_TYPE_STATUS;
}

void MegaChatMessagePrivate::setTempId(MegaChatHandle tempId)
{
    this->tempId = tempId;
}

void MegaChatMessagePrivate::setContentChanged()
{
    this->changed |= MegaChatMessage::CHANGE_TYPE_CONTENT;
}

LoggerHandler::LoggerHandler()
    : ILoggerBackend(MegaChatApi::LOG_LEVEL_INFO)
{
    mutex.init(true);
    this->megaLogger = NULL;

    gLogger.addUserLogger("MegaChatApi", this);
}

LoggerHandler::~LoggerHandler()
{
    gLogger.removeUserLogger("MegaChatApi");
}

void LoggerHandler::setMegaChatLogger(MegaChatLogger *logger)
{
    this->megaLogger = logger;
}

void LoggerHandler::setLogLevel(int logLevel)
{
    this->maxLogLevel = logLevel;
}

void LoggerHandler::log(krLogLevel level, const char *msg, size_t len, unsigned flags)
{
    mutex.lock();
    if (megaLogger)
    {
        megaLogger->log(level, msg);
    }
    mutex.unlock();
}
