
#import "megachatapi.h"
#import "MEGAChatSdk.h"

class DelegateMEGAChatListener : public megachat::MegaChatListener {
    
public:
    
    DelegateMEGAChatListener(MEGAChatSdk *megaChatSDK, id<MEGAChatDelegate>listener, bool singleListener = true);
    id<MEGAChatDelegate>getUserListener();
    
    void onChatListItemUpdate(megachat::MegaChatApi *api, megachat::MegaChatListItem *item);
    void onChatInitStateUpdate(megachat::MegaChatApi *api, int newState);
    void onChatOnlineStatusUpdate(megachat::MegaChatApi *api, megachat::MegaChatHandle userHandle, int status, bool inProgress);
    void onChatPresenceConfigUpdate(megachat::MegaChatApi *api, megachat::MegaChatPresenceConfig *config);
    void onChatConnectionStateUpdate(megachat::MegaChatApi *api, megachat::MegaChatHandle chatId, int newState);
    void onChatPresenceLastGreen(megachat::MegaChatApi* api, megachat::MegaChatHandle userHandle, int lastGreen);
    
private:
    __weak MEGAChatSdk *megaChatSDK;
    id<MEGAChatDelegate>listener;
    bool singleListener;
};
