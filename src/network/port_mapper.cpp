#include "network/port_mapper.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <sstream>
#include <string>

#if defined(DINERO_HAVE_MINIUPNPC)
#include <miniupnpc/miniupnpc.h>
#include <miniupnpc/upnpcommands.h>
#include <miniupnpc/upnperrors.h>
#endif

#if defined(DINERO_HAVE_NATPMP)
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <sys/select.h>
#endif
#include <natpmp.h>
#endif

namespace dinero::network {
namespace {

std::string LowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

[[maybe_unused]] std::string PortToString(uint16_t port) {
    return std::to_string(static_cast<unsigned int>(port));
}

#if defined(DINERO_HAVE_NATPMP)
std::string InAddrToString(const in_addr& addr) {
    char buffer[INET_ADDRSTRLEN] = {0};
    if (!inet_ntop(AF_INET, &addr, buffer, sizeof(buffer))) {
        return {};
    }
    return std::string(buffer);
}

int ReadNatPmpResponse(natpmp_t* natpmp, natpmpresp_t* response) {
    int rc = NATPMP_TRYAGAIN;
    while (rc == NATPMP_TRYAGAIN) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(natpmp->s, &fds);

        timeval timeout;
        getnatpmprequesttimeout(natpmp, &timeout);
        select(natpmp->s + 1, &fds, nullptr, nullptr, &timeout);

        rc = readnatpmpresponseorretry(natpmp, response);
    }
    return rc;
}
#endif

}  // namespace

PortMappingMode ParsePortMappingMode(const std::string& value,
                                     bool upnp_enabled,
                                     bool natpmp_enabled) {
    const std::string mode = LowerAscii(value);
    if (mode == "1" || mode == "true" || mode == "yes" || mode == "on" || mode == "auto") {
        return PortMappingMode::Auto;
    }
    if (mode == "upnp") {
        return PortMappingMode::Upnp;
    }
    if (mode == "natpmp" || mode == "nat-pmp" || mode == "pmp" || mode == "pcp") {
        return PortMappingMode::NatPmp;
    }
    if (upnp_enabled && natpmp_enabled) {
        return PortMappingMode::Auto;
    }
    if (upnp_enabled) {
        return PortMappingMode::Upnp;
    }
    if (natpmp_enabled) {
        return PortMappingMode::NatPmp;
    }
    return PortMappingMode::Disabled;
}

std::string PortMappingModeName(PortMappingMode mode) {
    switch (mode) {
    case PortMappingMode::Disabled:
        return "disabled";
    case PortMappingMode::Upnp:
        return "upnp";
    case PortMappingMode::NatPmp:
        return "natpmp";
    case PortMappingMode::Auto:
        return "auto";
    }
    return "disabled";
}

PortMappingSession::~PortMappingSession() {
    Stop();
}

PortMappingResult PortMappingSession::Start(const PortMappingConfig& config) {
    Stop();

    PortMappingResult result;
    if (!config.enabled()) {
        result.message = "port mapping disabled";
        return result;
    }

    mode_ = config.mode;
    internal_port_ = config.internal_port;
    external_port_ = config.external_port;

    if (config.mode == PortMappingMode::Upnp || config.mode == PortMappingMode::Auto) {
        if (TryUpnp(config, &result)) {
            active_ = true;
            protocol_ = "UPnP";
            return result;
        }
    }

    if (config.mode == PortMappingMode::NatPmp || config.mode == PortMappingMode::Auto) {
        if (TryNatPmp(config, &result)) {
            active_ = true;
            protocol_ = "NAT-PMP";
            return result;
        }
    }

    if (result.message.empty()) {
        result.message = "no supported port mapping protocol is available";
    }
    return result;
}

void PortMappingSession::Stop() {
    if (!active_) {
        return;
    }

    if (protocol_ == "UPnP") {
        StopUpnp();
    } else if (protocol_ == "NAT-PMP") {
        StopNatPmp();
    }

    active_ = false;
    protocol_.clear();
    internal_port_ = 0;
    external_port_ = 0;
    mode_ = PortMappingMode::Disabled;
}

bool PortMappingSession::TryUpnp(const PortMappingConfig& config, PortMappingResult* result) {
#if defined(DINERO_HAVE_MINIUPNPC)
    int discover_error = 0;
#if defined(MINIUPNPC_API_VERSION) && MINIUPNPC_API_VERSION >= 14
    UPNPDev* devices = upnpDiscover(2000, nullptr, nullptr, 0, 0, 2, &discover_error);
#else
    UPNPDev* devices = upnpDiscover(2000, nullptr, nullptr, 0, 0, &discover_error);
#endif
    if (!devices) {
        if (result) {
            result->message = "UPnP discovery failed: " + std::to_string(discover_error);
        }
        return false;
    }

    UPNPUrls urls;
    IGDdatas data;
    char lan_address[64] = {0};
    char wan_address[64] = {0};
    std::memset(&urls, 0, sizeof(urls));
    std::memset(&data, 0, sizeof(data));

#if defined(MINIUPNPC_API_VERSION) && MINIUPNPC_API_VERSION >= 18
    const int valid_igd = UPNP_GetValidIGD(devices,
                                           &urls,
                                           &data,
                                           lan_address,
                                           sizeof(lan_address),
                                           wan_address,
                                           sizeof(wan_address));
#else
    const int valid_igd = UPNP_GetValidIGD(devices, &urls, &data, lan_address, sizeof(lan_address));
#endif
    if (valid_igd <= 0) {
        freeUPNPDevlist(devices);
        if (result) {
            result->message = "UPnP router found no valid IGD";
        }
        return false;
    }

    char external_ip[64] = {0};
    UPNP_GetExternalIPAddress(urls.controlURL, data.first.servicetype, external_ip);

    const std::string external_port = PortToString(config.external_port);
    const std::string internal_port = PortToString(config.internal_port);
    const std::string lease = std::to_string(config.lifetime_seconds);
    const int rc = UPNP_AddPortMapping(urls.controlURL,
                                       data.first.servicetype,
                                       external_port.c_str(),
                                       internal_port.c_str(),
                                       lan_address,
                                       config.description.c_str(),
                                       "TCP",
                                       nullptr,
                                       lease.c_str());

    FreeUPNPUrls(&urls);
    freeUPNPDevlist(devices);

    if (rc != UPNPCOMMAND_SUCCESS) {
        if (result) {
            result->message = "UPnP AddPortMapping failed: " + std::string(strupnperror(rc));
        }
        return false;
    }

    if (result) {
        result->success = true;
        result->protocol = "UPnP";
        result->external_address = std::string(external_ip) + ":" + external_port;
        result->message = "mapped TCP " + external_port + " to local TCP " + internal_port;
    }
    return true;
#else
    if (result) {
        result->message = "UPnP support was not compiled in";
    }
    return false;
#endif
}

bool PortMappingSession::TryNatPmp(const PortMappingConfig& config, PortMappingResult* result) {
#if defined(DINERO_HAVE_NATPMP)
    natpmp_t natpmp;
    std::memset(&natpmp, 0, sizeof(natpmp));
    if (initnatpmp(&natpmp, 0, 0) < 0) {
        if (result) {
            result->message = "NAT-PMP init failed";
        }
        return false;
    }

    std::string public_address;
    if (sendpublicaddressrequest(&natpmp) == 0) {
        natpmpresp_t public_response;
        std::memset(&public_response, 0, sizeof(public_response));
        const int public_rc = ReadNatPmpResponse(&natpmp, &public_response);
        if (public_rc >= 0 && public_response.type == NATPMP_RESPTYPE_PUBLICADDRESS) {
            public_address = InAddrToString(public_response.pnu.publicaddress.addr);
        }
    }

    if (sendnewportmappingrequest(&natpmp,
                                  NATPMP_PROTOCOL_TCP,
                                  config.internal_port,
                                  config.external_port,
                                  config.lifetime_seconds) < 0) {
        closenatpmp(&natpmp);
        if (result) {
            result->message = "NAT-PMP port mapping request failed";
        }
        return false;
    }

    natpmpresp_t response;
    std::memset(&response, 0, sizeof(response));
    const int rc = ReadNatPmpResponse(&natpmp, &response);
    closenatpmp(&natpmp);

    if (rc < 0 || response.type != NATPMP_RESPTYPE_TCPPORTMAPPING) {
        if (result) {
            result->message = "NAT-PMP port mapping failed: " + std::to_string(rc);
        }
        return false;
    }

    if (result) {
        const std::string mapped_port = PortToString(response.pnu.newportmapping.mappedpublicport);
        result->success = true;
        result->protocol = "NAT-PMP";
        result->external_address = public_address.empty()
            ? ":" + mapped_port
            : public_address + ":" + mapped_port;
        result->message = "mapped TCP " + mapped_port +
            " to local TCP " + PortToString(config.internal_port) +
            (public_address.empty() ? "" : " at " + public_address);
    }
    return true;
#else
    if (result) {
        result->message = "NAT-PMP support was not compiled in";
    }
    return false;
#endif
}

void PortMappingSession::StopUpnp() {
#if defined(DINERO_HAVE_MINIUPNPC)
    int discover_error = 0;
#if defined(MINIUPNPC_API_VERSION) && MINIUPNPC_API_VERSION >= 14
    UPNPDev* devices = upnpDiscover(2000, nullptr, nullptr, 0, 0, 2, &discover_error);
#else
    UPNPDev* devices = upnpDiscover(2000, nullptr, nullptr, 0, 0, &discover_error);
#endif
    if (!devices) {
        return;
    }

    UPNPUrls urls;
    IGDdatas data;
    char lan_address[64] = {0};
    char wan_address[64] = {0};
    std::memset(&urls, 0, sizeof(urls));
    std::memset(&data, 0, sizeof(data));
#if defined(MINIUPNPC_API_VERSION) && MINIUPNPC_API_VERSION >= 18
    const int valid_igd = UPNP_GetValidIGD(devices,
                                           &urls,
                                           &data,
                                           lan_address,
                                           sizeof(lan_address),
                                           wan_address,
                                           sizeof(wan_address));
#else
    const int valid_igd = UPNP_GetValidIGD(devices, &urls, &data, lan_address, sizeof(lan_address));
#endif
    if (valid_igd > 0) {
        const std::string external_port = PortToString(external_port_);
        UPNP_DeletePortMapping(urls.controlURL,
                               data.first.servicetype,
                               external_port.c_str(),
                               "TCP",
                               nullptr);
    }
    FreeUPNPUrls(&urls);
    freeUPNPDevlist(devices);
#endif
}

void PortMappingSession::StopNatPmp() {
#if defined(DINERO_HAVE_NATPMP)
    natpmp_t natpmp;
    std::memset(&natpmp, 0, sizeof(natpmp));
    if (initnatpmp(&natpmp, 0, 0) < 0) {
        return;
    }

    if (sendnewportmappingrequest(&natpmp,
                                  NATPMP_PROTOCOL_TCP,
                                  internal_port_,
                                  external_port_,
                                  0) == 0) {
        natpmpresp_t response;
        std::memset(&response, 0, sizeof(response));
        ReadNatPmpResponse(&natpmp, &response);
    }
    closenatpmp(&natpmp);
#endif
}

}  // namespace dinero::network
