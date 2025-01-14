#include "THeartbeatThread.h"

#include "Client.h"
#include "Http.h"
//#include "SocketIO.h"
#include <rapidjson/document.h>
#include <rapidjson/rapidjson.h>
#include <sstream>

namespace json = rapidjson;

void THeartbeatThread::operator()() {
    RegisterThread("Heartbeat");
    std::string Body;
    std::string T;

    // these are "hot-change" related variables
    static std::string Last;

    static std::chrono::high_resolution_clock::time_point LastNormalUpdateTime = std::chrono::high_resolution_clock::now();
    bool isAuth = false;
    while (!mShutdown) {
        Body = GenerateCall();
        // a hot-change occurs when a setting has changed, to update the backend of that change.
        auto Now = std::chrono::high_resolution_clock::now();
        bool Unchanged = Last == Body;
        auto TimePassed = (Now - LastNormalUpdateTime);
        auto Threshold = Unchanged ? 30 : 5;
        if (TimePassed < std::chrono::seconds(Threshold)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        debug("heartbeat (after " + std::to_string(std::chrono::duration_cast<std::chrono::seconds>(TimePassed).count()) + "s)");

        Last = Body;
        LastNormalUpdateTime = Now;
        if (!Application::Settings.CustomIP.empty())
            Body += "&ip=" + Application::Settings.CustomIP;

        Body += "&pps=" + Application::PPS();

        auto SentryReportError = [&](const std::string& transaction, int status) {
            auto Lock = Sentry.CreateExclusiveContext();
            Sentry.SetContext("heartbeat",
                { { "response-body", T },
                    { "request-body", Body } });
            Sentry.SetTransaction(transaction);
            Sentry.Log(SentryLevel::Error, "default", Http::Status::ToString(status) + " (" + std::to_string(status) + ")");
        };

        auto Target = "/heartbeat";
        int ResponseCode = -1;
        const std::vector<std::string> Urls = {
            Application::GetBackendHostname(),
            Application::GetBackup1Hostname(),
            Application::GetBackup2Hostname(),
        };

        json::Document Doc;
        bool Ok = false;
        for (const auto& Url : Urls) {
            T = Http::POST(Url, Target, { { "api-v", "2" } }, Body, false, &ResponseCode);
            trace(T);
            Doc.Parse(T.data(), T.size());
            if (Doc.HasParseError() || !Doc.IsObject()) {
                error("Backend response failed to parse as valid json");
                debug("Response was: `" + T + "`");
                Sentry.SetContext("JSON Response", { { "reponse", T } });
                SentryReportError(Url + Target, ResponseCode);
            } else if (ResponseCode != 200) {
                SentryReportError(Url + Target, ResponseCode);
            } else {
                // all ok
                Ok = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        std::string Status {};
        std::string Code {};
        std::string Message {};
        const auto StatusKey = "status";
        const auto CodeKey = "code";
        const auto MessageKey = "msg";

        if (Ok) {
            if (Doc.HasMember(StatusKey) && Doc[StatusKey].IsString()) {
                Status = Doc[StatusKey].GetString();
            } else {
                Sentry.SetContext("JSON Response", { { StatusKey, "invalid string / missing" } });
                Ok = false;
            }
            if (Doc.HasMember(CodeKey) && Doc[CodeKey].IsString()) {
                Code = Doc[CodeKey].GetString();
            } else {
                Sentry.SetContext("JSON Response", { { CodeKey, "invalid string / missing" } });
                Ok = false;
            }
            if (Doc.HasMember(MessageKey) && Doc[MessageKey].IsString()) {
                Message = Doc[MessageKey].GetString();
            } else {
                Sentry.SetContext("JSON Response", { { MessageKey, "invalid string / missing" } });
                Ok = false;
            }
            if (!Ok) {
                error("Missing/invalid json members in backend response");
                Sentry.LogError("Missing/invalid json members in backend response", __FILE__, std::to_string(__LINE__));
            }
        }

        if (Ok && !isAuth) {
            if (Status == "2000") {
                info(("Authenticated!"));
                isAuth = true;
            } else if (Status == "200") {
                info(("Resumed authenticated session!"));
                isAuth = true;
            } else {
                if (Message.empty()) {
                    Message = "Backend didn't provide a reason";
                }
                error("Backend REFUSED the auth key. " + Message);
            }
        }
    }
}

std::string THeartbeatThread::GenerateCall() {
    std::stringstream Ret;

    Ret << "uuid=" << Application::Settings.Key
        << "&players=" << mServer.ClientCount()
        << "&maxplayers=" << Application::Settings.MaxPlayers
        << "&port=" << Application::Settings.Port
        << "&map=" << Application::Settings.MapName
        << "&private=" << (Application::Settings.Private ? "true" : "false")
        << "&version=" << Application::ServerVersion()
        << "&clientversion=" << Application::ClientVersion()
        << "&name=" << Application::Settings.ServerName
        << "&modlist=" << mResourceManager.TrimmedList()
        << "&modstotalsize=" << mResourceManager.MaxModSize()
        << "&modstotal=" << mResourceManager.ModsLoaded()
        << "&playerslist=" << GetPlayers()
        << "&desc=" << Application::Settings.ServerDesc;
    return Ret.str();
}
THeartbeatThread::THeartbeatThread(TResourceManager& ResourceManager, TServer& Server)
    : mResourceManager(ResourceManager)
    , mServer(Server) {
    Application::RegisterShutdownHandler([&] {
        if (mThread.joinable()) {
            mShutdown = true;
            mThread.join();
        }
    });
    Start();
}
std::string THeartbeatThread::GetPlayers() {
    std::string Return;
    mServer.ForEachClient([&](const std::weak_ptr<TClient>& ClientPtr) -> bool {
        ReadLock Lock(mServer.GetClientMutex());
        if (!ClientPtr.expired()) {
            Return += ClientPtr.lock()->GetName() + ";";
        }
        return true;
    });
    return Return;
}
/*THeartbeatThread::~THeartbeatThread() {
}*/
