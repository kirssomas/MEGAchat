#ifndef MEGACHATAPPLICATION_H
#define MEGACHATAPPLICATION_H
#include <fstream>
#include <memory>
#include <QApplication>
#include "LoginDialog.h"
#include "MainWindow.h"
#include "megachatapi.h"
#include "QTMegaListener.h"
#include "QTMegaChatListener.h"
#include "QTMegaChatRequestListener.h"
#include "QTMegaChatRoomListener.h"
#include "QTMegaChatNotificationListener.h"

#define MAX_RETRIES 5
class MegaLoggerApplication;
class LoginDialog;

class MegaChatApplication : public QApplication,
    public ::mega::MegaListener,
    public megachat::MegaChatRequestListener,
    public megachat::MegaChatNotificationListener
{
    Q_OBJECT
    public:
        MegaChatApplication(int &argc, char** argv);
        virtual ~MegaChatApplication();
        void init();
        void login();
        void configureLogs();
        bool initAnonymous(std::string chatlink);
        std::string getText(std::string title);
        const char *readSid();
        const char *sid() const;
        void saveSid(const char *sid);
        void removeSid();
        LoginDialog *loginDialog() const;
        void resetLoginDialog();
        std::string base64ToBinary(const char *base64);
        std::string getLocalUserAlias(megachat::MegaChatHandle uh);

        virtual void onRequestFinish(megachat::MegaChatApi *mMegaChatApi, megachat::MegaChatRequest *request, megachat::MegaChatError *e);
        virtual void onRequestFinish(::mega::MegaApi *api, ::mega::MegaRequest *request, ::mega::MegaError *e);
        virtual void onUsersUpdate(::mega::MegaApi *api, ::mega::MegaUserList *userList);
        virtual void onChatNotification(megachat::MegaChatApi *api, megachat::MegaChatHandle chatid, megachat::MegaChatMessage *msg);

        const char *getFirstname(megachat::MegaChatHandle uh, const char *authorizationToken);

        bool isStagingEnabled();
        void enableStaging(bool enable);

        std::shared_ptr<::mega::MegaPushNotificationSettings> getNotificationSettings() const;
        std::shared_ptr<::mega::MegaTimeZoneDetails> getTimeZoneDetails() const;
        MainWindow *mainWindow() const;
        ::mega::MegaApi *megaApi() const;
        megachat::MegaChatApi *megaChatApi() const;

protected:
        const char *mSid;
        std::string mAppDir;
        MainWindow *mMainWin;
        LoginDialog *mLoginDialog;
        MegaLoggerApplication *mLogger;
        ::mega::MegaApi *mMegaApi;
        megachat::MegaChatApi *mMegaChatApi;
        ::mega::QTMegaListener *megaListenerDelegate;
        megachat::QTMegaChatRequestListener *megaChatRequestListenerDelegate;
        megachat::QTMegaChatNotificationListener *megaChatNotificationListenerDelegate;

    private:
        std::map<megachat::MegaChatHandle, std::string> mFirstnamesMap;
        std::map<megachat::MegaChatHandle, std::string> mAliasesMap;
        std::map<megachat::MegaChatHandle, bool> mFirstnameFetching;
        bool useStaging = false;
        std::shared_ptr<::mega::MegaPushNotificationSettings> mNotificationSettings;
        std::shared_ptr<::mega::MegaTimeZoneDetails> mTimeZoneDetails;

    public slots:
        void onAnonymousLogout();
        void onLoginClicked();
        void onPreviewClicked();

};
#endif // MEGACHATAPPLICATION_H
