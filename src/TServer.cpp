#include "TServer.h"
#include "Client.h"
#include "Common.h"
#include "TNetwork.h"
#include "TPPSMonitor.h"
#include <TLuaFile.h>
#include <any>
#include <sstream>

#undef GetObject // Fixes Windows

#include "Json.h"

namespace json = rapidjson;

TServer::TServer(int argc, char** argv) {
    info("BeamMP Server v" + Application::ServerVersion());
    if (argc > 1) {
        Application::Settings.CustomIP = argv[1];
        size_t n = std::count(Application::Settings.CustomIP.begin(), Application::Settings.CustomIP.end(), '.');
        auto p = Application::Settings.CustomIP.find_first_not_of(".0123456789");
        if (p != std::string::npos || n != 3 || Application::Settings.CustomIP.substr(0, 3) == "127") {
            Application::Settings.CustomIP.clear();
            warn("IP Specified is invalid! Ignoring");
        } else {
            info("server started with custom IP");
        }
    }
}

void TServer::RemoveClient(const std::weak_ptr<TClient>& WeakClientPtr) {
    if (!WeakClientPtr.expired()) {
        TClient& Client = *WeakClientPtr.lock();
        debug("removing client " + Client.GetName() + " (" + std::to_string(ClientCount()) + ")");
        Client.ClearCars();
        WriteLock Lock(mClientsMutex);
        mClients.erase(WeakClientPtr.lock());
    }
}

std::weak_ptr<TClient> TServer::InsertNewClient() {
    debug("inserting new client (" + std::to_string(ClientCount()) + ")");
    WriteLock Lock(mClientsMutex);
    auto [Iter, Replaced] = mClients.insert(std::make_shared<TClient>(*this));
    return *Iter;
}

void TServer::ForEachClient(const std::function<bool(std::weak_ptr<TClient>)>& Fn) {
    decltype(mClients) Clients;
    {
        ReadLock lock(mClientsMutex);
        Clients = mClients;
    }
    for (auto& Client : Clients) {
        if (!Fn(Client)) {
            break;
        }
    }
}

size_t TServer::ClientCount() const {
    ReadLock Lock(mClientsMutex);
    return mClients.size();
}

void TServer::GlobalParser(const std::weak_ptr<TClient>& Client, std::string Packet, TPPSMonitor& PPSMonitor, TNetwork& Network) {
    if (Packet.find("Zp") != std::string::npos && Packet.size() > 500) {
        //abort();
    }
    if (Packet.substr(0, 4) == "ABG:") {
        Packet = DeComp(Packet.substr(4));
    }
    if (Packet.empty()) {
        return;
    }

    if (Client.expired()) {
        return;
    }
    auto LockedClient = Client.lock();

    std::any Res;
    char Code = Packet.at(0);

    //V to Z
    if (Code <= 90 && Code >= 86) {
        PPSMonitor.IncrementInternalPPS();
        Network.SendToAll(LockedClient.get(), Packet, false, false);
        return;
    }
    switch (Code) {
    case 'H': // initial connection
        trace(std::string("got 'H' packet: '") + Packet + "' (" + std::to_string(Packet.size()) + ")");
        if (!Network.SyncClient(Client)) {
            // TODO handle
        }
        return;
    case 'p':
        if (!Network.Respond(*LockedClient, ("p"), false)) {
            // failed to send
            if (LockedClient->GetStatus() > -1) {
                LockedClient->SetStatus(-1);
            }
        } else {
            Network.UpdatePlayer(*LockedClient);
        }
        return;
    case 'O':
        if (Packet.length() > 1000) {
            debug(("Received data from: ") + LockedClient->GetName() + (" Size: ") + std::to_string(Packet.length()));
        }
        ParseVehicle(*LockedClient, Packet, Network);
        return;
    case 'J':
        trace(std::string(("got 'J' packet: '")) + Packet + ("' (") + std::to_string(Packet.size()) + (")"));
        Network.SendToAll(LockedClient.get(), Packet, false, true);
        return;
    case 'C':
        trace(std::string(("got 'C' packet: '")) + Packet + ("' (") + std::to_string(Packet.size()) + (")"));
        if (Packet.length() < 4 || Packet.find(':', 3) == std::string::npos)
            break;
        Res = TriggerLuaEvent("onChatMessage", false, nullptr, std::make_unique<TLuaArg>(TLuaArg { { LockedClient->GetID(), LockedClient->GetName(), Packet.substr(Packet.find(':', 3) + 1) } }), true);
        LogChatMessage(LockedClient->GetName(), LockedClient->GetID(), Packet.substr(Packet.find(':', 3) + 1)); // FIXME: this needs to be adjusted once lua is merged
        if (std::any_cast<int>(Res))
            break;
        Network.SendToAll(nullptr, Packet, true, true);
        return;
    case 'E':
        trace(std::string(("got 'E' packet: '")) + Packet + ("' (") + std::to_string(Packet.size()) + (")"));
        HandleEvent(*LockedClient, Packet);
        return;
    case 'N':
        trace("got 'N' packet (" + std::to_string(Packet.size()) + ")");
        Network.SendToAll(LockedClient.get(), Packet, false, true);
        return;
    default:
        return;
    }
}

void TServer::HandleEvent(TClient& c, const std::string& Data) {
    std::stringstream ss(Data);
    std::string t, Name;
    int a = 0;
    while (std::getline(ss, t, ':')) {
        switch (a) {
        case 1:
            Name = t;
            break;
        case 2:
            TriggerLuaEvent(Name, false, nullptr, std::make_unique<TLuaArg>(TLuaArg { { c.GetID(), t } }), false);
            break;
        default:
            break;
        }
        if (a == 2)
            break;
        a++;
    }
}
bool TServer::IsUnicycle(TClient& c, const std::string& CarJson) {
    rapidjson::Document Car;
    Car.Parse(CarJson.c_str(), CarJson.size());
    if (Car.HasParseError()) {
        error("Failed to parse vehicle data -> " + CarJson);
    } else if (Car["jbm"].IsString() && std::string(Car["jbm"].GetString()) == "unicycle") {
        return true;
    }
    return false;
}
bool TServer::ShouldSpawn(TClient& c, const std::string& CarJson, int ID) {

    if (c.GetUnicycleID() > -1 && (c.GetCarCount() - 1) < Application::Settings.MaxCars) {
        return true;
    }

    if (IsUnicycle(c, CarJson)) {
        c.SetUnicycleID(ID);
        return true;
    }

    return Application::Settings.MaxCars > c.GetCarCount();
}

void TServer::ParseVehicle(TClient& c, const std::string& Pckt, TNetwork& Network) {
    if (Pckt.length() < 4)
        return;
    std::string Packet = Pckt;
    char Code = Packet.at(1);
    int PID = -1;
    int VID = -1, Pos;
    std::string Data = Packet.substr(3), pid, vid;
    switch (Code) { //Spawned Destroyed Switched/Moved NotFound Reset
    case 's':
        trace(std::string(("got 'Os' packet: '")) + Packet + ("' (") + std::to_string(Packet.size()) + (")"));
        if (Data.at(0) == '0') {
            int CarID = c.GetOpenCarID();
            debug(c.GetName() + (" created a car with ID ") + std::to_string(CarID));

            std::string CarJson = Packet.substr(5);
            Packet = "Os:" + c.GetRoles() + ":" + c.GetName() + ":" + std::to_string(c.GetID()) + "-" + std::to_string(CarID) + ":" + CarJson;
            auto Res = TriggerLuaEvent(("onVehicleSpawn"), false, nullptr, std::make_unique<TLuaArg>(TLuaArg { { c.GetID(), CarID, Packet.substr(3) } }), true);

            if (ShouldSpawn(c, CarJson, CarID) && std::any_cast<int>(Res) == 0) {
                c.AddNewCar(CarID, Packet);
                Network.SendToAll(nullptr, Packet, true, true);
            } else {
                if (!Network.Respond(c, Packet, true)) {
                    // TODO: handle
                }
                std::string Destroy = "Od:" + std::to_string(c.GetID()) + "-" + std::to_string(CarID);
                if (!Network.Respond(c, Destroy, true)) {
                    // TODO: handle
                }
                debug(c.GetName() + (" (force : car limit/lua) removed ID ") + std::to_string(CarID));
            }
        }
        return;
    case 'c':
        trace(std::string(("got 'Oc' packet: '")) + Packet + ("' (") + std::to_string(Packet.size()) + (")"));
        pid = Data.substr(0, Data.find('-'));
        vid = Data.substr(Data.find('-') + 1, Data.find(':', 1) - Data.find('-') - 1);
        if (pid.find_first_not_of("0123456789") == std::string::npos && vid.find_first_not_of("0123456789") == std::string::npos) {
            PID = stoi(pid);
            VID = stoi(vid);
        }
        if (PID != -1 && VID != -1 && PID == c.GetID()) {
            auto Res = TriggerLuaEvent(("onVehicleEdited"), false, nullptr,
                std::make_unique<TLuaArg>(TLuaArg { { c.GetID(), VID, Packet.substr(3) } }),
                true);

            auto FoundPos = Packet.find('{');
            FoundPos = FoundPos == std::string::npos ? 0 : FoundPos; // attempt at sanitizing this
            if ((c.GetUnicycleID() != VID || IsUnicycle(c, Packet.substr(FoundPos)))
                && std::any_cast<int>(Res) == 0) {
                Network.SendToAll(&c, Packet, false, true);
                Apply(c, VID, Packet);
            } else {
                if (c.GetUnicycleID() == VID) {
                    c.SetUnicycleID(-1);
                }
                std::string Destroy = "Od:" + std::to_string(c.GetID()) + "-" + std::to_string(VID);
                if (!Network.Respond(c, Destroy, true)) {
                    // TODO: handle
                }
                c.DeleteCar(VID);
            }
        }
        return;
    case 'd':
        trace(std::string(("got 'Od' packet: '")) + Packet + ("' (") + std::to_string(Packet.size()) + (")"));
        pid = Data.substr(0, Data.find('-'));
        vid = Data.substr(Data.find('-') + 1);
        if (pid.find_first_not_of("0123456789") == std::string::npos && vid.find_first_not_of("0123456789") == std::string::npos) {
            PID = stoi(pid);
            VID = stoi(vid);
        }
        if (PID != -1 && VID != -1 && PID == c.GetID()) {
            if (c.GetUnicycleID() == VID) {
                c.SetUnicycleID(-1);
            }
            Network.SendToAll(nullptr, Packet, true, true);
            TriggerLuaEvent(("onVehicleDeleted"), false, nullptr,
                std::make_unique<TLuaArg>(TLuaArg { { c.GetID(), VID } }), false);
            c.DeleteCar(VID);
            debug(c.GetName() + (" deleted car with ID ") + std::to_string(VID));
        }
        return;
    case 'r':
        trace(std::string(("got 'Or' packet: '")) + Packet + ("' (") + std::to_string(Packet.size()) + (")"));
        Pos = int(Data.find('-'));
        pid = Data.substr(0, Pos++);
        vid = Data.substr(Pos, Data.find(':') - Pos);

        if (pid.find_first_not_of("0123456789") == std::string::npos && vid.find_first_not_of("0123456789") == std::string::npos) {
            PID = stoi(pid);
            VID = stoi(vid);
        }

        if (PID != -1 && VID != -1 && PID == c.GetID()) {
            Data = Data.substr(Data.find('{'));
            TriggerLuaEvent("onVehicleReset", false, nullptr,
                std::make_unique<TLuaArg>(TLuaArg { { c.GetID(), VID, Data } }),
                false);
            Network.SendToAll(&c, Packet, false, true);
        }
        return;
    case 't':
        trace(std::string(("got 'Ot' packet: '")) + Packet + ("' (") + std::to_string(Packet.size()) + (")"));
        Network.SendToAll(&c, Packet, false, true);
        return;
    default:
        trace(std::string(("possibly not implemented: '") + Packet + ("' (") + std::to_string(Packet.size()) + (")")));
        return;
    }
}

void TServer::Apply(TClient& c, int VID, const std::string& pckt) {
    auto FoundPos = pckt.find('{');
    if (FoundPos == std::string::npos) {
        error("Malformed packet received, no '{' found");
        return;
    }
    std::string Packet = pckt.substr(FoundPos);
    std::string VD = c.GetCarData(VID);
    if (VD.empty()) {
        error("Tried to apply change to vehicle that does not exist");
        auto Lock = Sentry.CreateExclusiveContext();
        Sentry.SetContext("vehicle-change",
            { { "packet", Packet },
                { "vehicle-id", std::to_string(VID) },
                { "client-car-count", std::to_string(c.GetCarCount()) } });
        Sentry.LogError("attempt to apply change to nonexistent vehicle", _file_basename, _line);
        return;
    }
    std::string Header = VD.substr(0, VD.find('{'));

    FoundPos = VD.find('{');
    if (FoundPos == std::string::npos) {
        auto Lock = Sentry.CreateExclusiveContext();
        Sentry.SetContext("vehicle-change-packet",
            { { "packet", VD } });
        Sentry.LogError("malformed packet", _file_basename, _line);
        error("Malformed packet received, no '{' found");
        return;
    }
    VD = VD.substr(FoundPos);
    rapidjson::Document Veh, Pack;
    Veh.Parse(VD.c_str());
    if (Veh.HasParseError()) {
        error("Could not get vehicle config!");
        return;
    }
    Pack.Parse(Packet.c_str());
    if (Pack.HasParseError() || Pack.IsNull()) {
        error("Could not get active vehicle config!");
        return;
    }

    for (auto& M : Pack.GetObject()) {
        if (Veh[M.name].IsNull()) {
            Veh.AddMember(M.name, M.value, Veh.GetAllocator());
        } else {
            Veh[M.name] = Pack[M.name];
        }
    }
    rapidjson::StringBuffer Buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(Buffer);
    Veh.Accept(writer);
    c.SetCarData(VID, Header + Buffer.GetString());
}

void TServer::InsertClient(const std::shared_ptr<TClient>& NewClient) {
    debug("inserting client (" + std::to_string(ClientCount()) + ")");
    WriteLock Lock(mClientsMutex); //TODO why is there 30+ threads locked here
    (void)mClients.insert(NewClient);
}
