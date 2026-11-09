// Interfaces the T-Deck Pro build never creates (TCP client, RNS AutoInterface).
// The protocol pump references them, so they exist here as inert stubs.
#include "SPI.h"
#include "transport/RnsAutoInterface.h"
#include "transport/TCPClientInterface.h"

SPIClass SPI;

size_t RnsAutoInterface::peerCount() const { return 0; }
bool RnsAutoInterface::sendRaw(const uint8_t*, size_t) { return false; }

bool TCPClientInterface::reapConnectTask() { return true; }
bool TCPClientInterface::send_outgoing(const uint8_t*, size_t) { return false; }
