#include "CommandHandler.h"
#include "UDPPulseReceiver.h"
#include "startAirspyProcess.h"
#include "TunnelProtocol.h"

#include <chrono>
#include <cstdint>
#include <mavsdk/mavsdk.h>
#include <mavsdk/plugins/action/action.h>
#include <mavsdk/plugins/telemetry/telemetry.h>
#include <iostream>
#include <future>
#include <memory>
#include <thread>

using namespace mavsdk;

int main(int argc, char** argv)
{
    // Check that TunnelProtocol hasn't exceed limits
    static_assert(TunnelProtocolValidateSizes, "TunnelProtocolValidateSizes failed");

    Mavsdk mavsdk;
    mavsdk.set_configuration(Mavsdk::Configuration(1, MAV_COMP_ID_ONBOARD_COMPUTER, true));

    if (argc != 2) {
        std::cout << "Connection url must be specified on command line" << std::endl;
        return 1;
    }

    ConnectionResult connection_result;
    connection_result = mavsdk.add_any_connection(argv[1]);
    if (connection_result != ConnectionResult::Success) {
        std::cout << "Connection failed for " << argv[1] << ": " << connection_result << std::endl;
        return 1;
    }

    std::cout << "Waiting to discover Autopilot\n";
    auto autopilotPromise   = std::promise<std::shared_ptr<System>>{};
    auto autopilotFuture    = autopilotPromise.get_future();
    mavsdk.subscribe_on_new_system([&mavsdk, &autopilotPromise]() {
        std::vector< std::shared_ptr< System > > systems = mavsdk.systems();
        for (size_t i=0; i<systems.size(); i++) {
            std::shared_ptr< System > system = systems[i];
            std::vector< uint8_t > compIds = system->component_ids();
            if (std::find(compIds.begin(), compIds.end(), MAV_COMP_ID_AUTOPILOT1) != compIds.end()) {
                std::cout << "Discovered Autopilot" << std::endl;
                autopilotPromise.set_value(system);
                mavsdk.subscribe_on_new_system(nullptr);
                break;         
            }
        }
    });
    autopilotFuture.wait();

    std::cout << "Waiting to discover QGC\n";
    auto qgcPromise = std::promise<std::shared_ptr<System>>{};
    auto qgcFuture  = qgcPromise.get_future();
    mavsdk.subscribe_on_new_system([&mavsdk, &qgcPromise]() {
        auto system = mavsdk.systems().back();
        std::vector< uint8_t > compIds = system->component_ids();
        if (std::find(compIds.begin(), compIds.end(), MAV_COMP_ID_MISSIONPLANNER) != compIds.end()) {
            std::cout << "Discovered QGC" << std::endl;
            qgcPromise.set_value(system);
            mavsdk.subscribe_on_new_system(nullptr);            
        }
    });
    qgcFuture.wait();

    // We have both systems ready for use now
    auto autopilotSystem    = autopilotFuture.get();
    auto qgcSystem          = qgcFuture.get();

    auto mavlinkPassthrough = MavlinkPassthrough{ qgcSystem };
    auto udpPulseReceiver   = UDPPulseReceiver{ "127.0.0.1", 30000, mavlinkPassthrough };
    
    CommandHandler{ *qgcSystem, mavlinkPassthrough };

    udpPulseReceiver.start();

    std::cout << "Ready" << std::endl;

    while (true) {
        udpPulseReceiver.receive();
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
}

    return 0;
}
