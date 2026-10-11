// USB connection to the pspcap plugin (see common/pspcap_protocol.h).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "pspcap_protocol.h"

struct libusb_context;
struct libusb_device_handle;

// Microseconds on the PC's monotonic clock.
uint64_t now_us();

class UsbLink {
public:
    enum class OpenResult { Ok, NotFound, NoDriver, Busy, Error };

    UsbLink();
    ~UsbLink();
    UsbLink(const UsbLink &) = delete;
    UsbLink &operator=(const UsbLink &) = delete;

    // Creates the libusb context. Returns false and fills `err` on failure.
    bool init(std::string &err);

    OpenResult open(std::string &err);
    void close();
    bool is_open() const { return dev_ != nullptr; }

    // All of these return 0 or a negative libusb error code.
    int send(uint8_t type, const void *payload, uint32_t len);
    int recv(pspcap_header &hdr, std::vector<uint8_t> &payload, unsigned timeout_ms);
    int read_raw(void *buf, int len, int &got, unsigned timeout_ms);

    // Discards whatever the PSP is still sending (e.g. after an aborted run)
    // until it has been quiet for `quiet_ms`.
    void drain(unsigned quiet_ms = 300);

    // Brings a freshly opened link to a known state: stops a stream a previous
    // client may have left running, discards stale data, exchanges HELLO and
    // checks the protocol version. Returns false with `err` set on failure
    // (the link stays open; `hdr`/`hello` are filled whenever HELLO arrived).
    bool handshake(pspcap_header &hdr, pspcap_hello &hello, std::string &err);

    // Sends `type` and waits for a packet of `expect`. A PKT_ERROR reply or a
    // wrong packet is reported in `err`. Returns 0 on success.
    int request(uint8_t type, const void *payload, uint32_t len, uint8_t expect,
                pspcap_header &hdr, std::vector<uint8_t> &reply, unsigned timeout_ms,
                std::string &err);

private:
    libusb_context *ctx_ = nullptr;
    libusb_device_handle *dev_ = nullptr;
    uint32_t seq_ = 0;
};

const char *usb_error_name(int code);
