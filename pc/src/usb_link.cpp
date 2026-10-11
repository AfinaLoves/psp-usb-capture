#include "usb_link.h"

#include <libusb.h>

#include <chrono>
#include <cstdio>
#include <cstring>

uint64_t now_us()
{
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count());
}

const char *usb_error_name(int code)
{
    return libusb_error_name(code);
}

UsbLink::UsbLink() = default;

UsbLink::~UsbLink()
{
    close();
    if (ctx_)
        libusb_exit(ctx_);
}

bool UsbLink::init(std::string &err)
{
    int rc = libusb_init_context(&ctx_, nullptr, 0);
    if (rc != 0) {
        err = std::string("libusb init failed: ") + libusb_error_name(rc);
        ctx_ = nullptr;
        return false;
    }
    return true;
}

UsbLink::OpenResult UsbLink::open(std::string &err)
{
    close();

    libusb_device **list = nullptr;
    ssize_t count = libusb_get_device_list(ctx_, &list);
    if (count < 0) {
        err = std::string("cannot list USB devices: ") + libusb_error_name(static_cast<int>(count));
        return OpenResult::Error;
    }

    OpenResult result = OpenResult::NotFound;
    for (ssize_t i = 0; i < count; i++) {
        libusb_device_descriptor desc{};
        if (libusb_get_device_descriptor(list[i], &desc) != 0)
            continue;
        if (desc.idVendor != PSPCAP_USB_VID || desc.idProduct != PSPCAP_USB_PID)
            continue;

        int rc = libusb_open(list[i], &dev_);
        if (rc == LIBUSB_ERROR_NOT_SUPPORTED || rc == LIBUSB_ERROR_NOT_FOUND) {
            result = OpenResult::NoDriver;
        } else if (rc == LIBUSB_ERROR_ACCESS || rc == LIBUSB_ERROR_BUSY) {
            result = OpenResult::Busy;
        } else if (rc != 0) {
            err = std::string("cannot open the PSP: ") + libusb_error_name(rc);
            result = OpenResult::Error;
        } else {
            rc = libusb_claim_interface(dev_, PSPCAP_USB_INTERFACE);
            if (rc == 0) {
                result = OpenResult::Ok;
            } else {
                result = (rc == LIBUSB_ERROR_BUSY || rc == LIBUSB_ERROR_ACCESS)
                             ? OpenResult::Busy
                             : OpenResult::Error;
                err = std::string("cannot claim the PSP interface: ") + libusb_error_name(rc);
                libusb_close(dev_);
                dev_ = nullptr;
            }
        }
        break;
    }
    libusb_free_device_list(list, 1);

    switch (result) {
    case OpenResult::NotFound:
        err = "PSP not found (USB 054C:01CF). Start a game with pspcap.prx loaded "
              "and connect the cable.";
        break;
    case OpenResult::NoDriver:
        err = "PSP found, but it is not bound to WinUSB. Install the driver with "
              "Zadig, see docs/usb-driver.md.";
        break;
    case OpenResult::Busy:
        if (err.empty())
            err = "PSP is in use by another program (is another pspcap running?).";
        break;
    default:
        break;
    }
    seq_ = 0;
    return result;
}

void UsbLink::close()
{
    if (dev_) {
        libusb_release_interface(dev_, PSPCAP_USB_INTERFACE);
        libusb_close(dev_);
        dev_ = nullptr;
    }
}

int UsbLink::send(uint8_t type, const void *payload, uint32_t len)
{
    if (sizeof(pspcap_header) + len > PSPCAP_MAX_PACKET)
        return LIBUSB_ERROR_INVALID_PARAM;

    uint8_t buf[PSPCAP_MAX_PACKET];
    pspcap_header hdr{};
    hdr.magic = PSPCAP_MAGIC;
    hdr.version = PSPCAP_PROTO_VERSION;
    hdr.type = type;
    hdr.seq = seq_++;
    hdr.length = len;
    hdr.timestamp_us = now_us();
    std::memcpy(buf, &hdr, sizeof(hdr));
    if (len)
        std::memcpy(buf + sizeof(hdr), payload, len);

    int total = static_cast<int>(sizeof(hdr) + len);
    int sent = 0;
    int rc = libusb_bulk_transfer(dev_, PSPCAP_EP_OUT, buf, total, &sent, 1000);
    if (rc == 0 && sent != total)
        rc = LIBUSB_ERROR_IO;
    return rc;
}

int UsbLink::recv(pspcap_header &hdr, std::vector<uint8_t> &payload, unsigned timeout_ms)
{
    uint8_t buf[PSPCAP_MAX_PACKET];
    int got = 0;
    int rc = libusb_bulk_transfer(dev_, PSPCAP_EP_IN, buf, sizeof(buf), &got, timeout_ms);
    if (rc != 0)
        return rc;
    if (got < static_cast<int>(sizeof(hdr)))
        return LIBUSB_ERROR_IO;

    std::memcpy(&hdr, buf, sizeof(hdr));
    if (hdr.magic != PSPCAP_MAGIC || hdr.length != got - sizeof(hdr))
        return LIBUSB_ERROR_IO;
    payload.assign(buf + sizeof(hdr), buf + got);
    return 0;
}

int UsbLink::read_raw(void *buf, int len, int &got, unsigned timeout_ms)
{
    got = 0;
    return libusb_bulk_transfer(dev_, PSPCAP_EP_IN, static_cast<unsigned char *>(buf), len,
                                &got, timeout_ms);
}

void UsbLink::drain(unsigned quiet_ms)
{
    std::vector<uint8_t> buf(1024 * 1024);
    int got = 0;
    while (libusb_bulk_transfer(dev_, PSPCAP_EP_IN, buf.data(), static_cast<int>(buf.size()),
                                &got, quiet_ms) == 0) {
    }
}

int UsbLink::request(uint8_t type, const void *payload, uint32_t len, uint8_t expect,
                     pspcap_header &hdr, std::vector<uint8_t> &reply, unsigned timeout_ms,
                     std::string &err)
{
    int rc = send(type, payload, len);
    if (rc != 0) {
        err = std::string("send failed: ") + libusb_error_name(rc);
        return rc;
    }
    rc = recv(hdr, reply, timeout_ms);
    if (rc != 0) {
        err = std::string("no reply: ") + libusb_error_name(rc);
        return rc;
    }
    if (hdr.type == PSPCAP_PKT_ERROR && reply.size() == sizeof(pspcap_error)) {
        pspcap_error e{};
        std::memcpy(&e, reply.data(), sizeof(e));
        char msg[96];
        std::snprintf(msg, sizeof(msg), "PSP replied ERROR code %u (detail %u)", e.code, e.detail);
        err = msg;
        return LIBUSB_ERROR_OTHER;
    }
    if (hdr.type != expect) {
        char msg[96];
        std::snprintf(msg, sizeof(msg), "unexpected packet type 0x%02X (wanted 0x%02X)", hdr.type,
                      expect);
        err = msg;
        return LIBUSB_ERROR_OTHER;
    }
    return 0;
}

bool UsbLink::handshake(pspcap_header &hdr, pspcap_hello &hello, std::string &err)
{
    // Harmless if nothing is streaming; an old plugin answers with an ERROR,
    // which the drain swallows.
    send(PSPCAP_CMD_STREAM_STOP, nullptr, 0);
    drain();

    std::vector<uint8_t> reply;
    if (request(PSPCAP_CMD_HELLO, nullptr, 0, PSPCAP_PKT_HELLO, hdr, reply, 1000, err) != 0) {
        err = "HELLO failed: " + err;
        return false;
    }
    if (reply.size() != sizeof(hello)) {
        err = "HELLO has a wrong size";
        return false;
    }
    std::memcpy(&hello, reply.data(), sizeof(hello));
    if (hdr.version != PSPCAP_PROTO_VERSION) {
        char msg[128];
        std::snprintf(msg, sizeof(msg),
                      "Protocol mismatch: PSP speaks v%u, this client v%u. Update both.",
                      hdr.version, PSPCAP_PROTO_VERSION);
        err = msg;
        return false;
    }
    return true;
}
