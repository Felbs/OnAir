// IPv4 and UDP (as carried by ALP) and the ATSC 3.0 low level signaling on top of it: the LLS table header and the Service List Table (A/331 section 6).
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace dect2 {
namespace atsc3 {

struct UdpDatagram {
    uint32_t srcIp = 0, dstIp = 0;     // host order
    int srcPort = 0, dstPort = 0;
    std::vector<uint8_t> payload;
};

std::string ipToString(uint32_t ip);
bool ipFromString(const std::string& s, uint32_t& ip);

// Reassembles IPv4 fragments and returns UDP datagrams. Other protocols and malformed packets are dropped.
class IpReceiver {
public:
    // returns true and fills `out` when a complete UDP datagram came out
    bool push(const uint8_t* packet, size_t size, UdpDatagram& out);
    long dropped() const { return dropped_; }
private:
    struct Frag { std::vector<uint8_t> data; std::vector<std::pair<int, int>> got; int total = -1; long age = 0; };
    std::map<std::string, Frag> frags_;
    long dropped_ = 0, counter_ = 0;
};

// Builds an IPv4/UDP packet (for tests); `mtu` > 0 splits it into fragments.
std::vector<std::vector<uint8_t>> makeUdpPackets(uint32_t src, uint32_t dst, int sport, int dport, const std::vector<uint8_t>& payload, int id = 1, int mtu = 0);

// ---- low level signaling
constexpr uint32_t kLlsAddress = (224u << 24) | (0u << 16) | (23u << 8) | 60u;   // 224.0.23.60
constexpr int kLlsPort = 4937;

struct SltService {
    int serviceId = 0;
    int majorChannel = 0, minorChannel = 0;
    int category = 0;                  // 1 linear A/V, 2 audio only, 3 app based, 4 ESG, 5 EAS, 6 DRM data
    std::string shortName, globalId;
    bool hidden = false, protectedService = false;
    int slsProtocol = 0;               // 1 = ROUTE, 2 = MMTP
    uint32_t slsDstIp = 0, slsSrcIp = 0;
    int slsDstPort = 0;
    int seqNum = 0;
};

struct Slt {
    std::vector<int> bsid;
    std::vector<SltService> services;
};

struct LlsTable {
    int tableId = 0, groupId = 0, groupCount = 1, version = 0;
    std::string xml;                   // the decompressed table (tables 1 to 5)
    Slt slt;                           // when tableId == 1
    bool ok = false;
    // A SignedMultiTable (LLS_table_id 0xFE) carries several tables and a signature in one datagram; the result is then the SLT it
    // holds (or its first table), with the outer header kept here. The signature is not verified.
    int container = 0;                 // 0xFE when the table came out of a SignedMultiTable
    int containerTables = 0;           // how many tables the container held
    size_t signatureBytes = 0;         // length of the signature that followed them
};

// Parses the payload of a UDP datagram to 224.0.23.60:4937 -- a single table, or a SignedMultiTable holding several.
LlsTable parseLls(const uint8_t* payload, size_t size);
std::vector<uint8_t> makeLls(int tableId, int group, int version, const std::string& xml);
// A SignedMultiTable from single tables made by makeLls() (their headers are dropped) and an opaque signature.
std::vector<uint8_t> makeLlsMulti(int group, int version, const std::vector<std::vector<uint8_t>>& tables,
                                  const std::vector<uint8_t>& signature);

} // namespace atsc3
} // namespace dect2
