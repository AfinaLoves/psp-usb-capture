#include "usb.h"

#include <pspkernel.h>
#include <pspusb.h>
#include <pspusbbus.h>
#include <string.h>

#include "pspcap_protocol.h"

#define DRIVER_NAME "pspcap_driver"

// Wait slices: detach is noticed within this time even inside long waits.
#define WAIT_SLICE_US 100000

enum {
    EV_ATTACH   = 1 << 0,
    EV_OUT_DONE = 1 << 1,
    EV_IN_DONE0 = 1 << 2, // + slot
    EV_WAKE     = 1 << 8, // usb_wake()
};

#define EV_IN_ALL (((1 << USB_SEND_SLOTS) - 1) * EV_IN_DONE0)

static SceUID g_events = -1;
static volatile int g_attached;
static volatile int g_speed;

static struct UsbdDeviceReq g_out_req;
static int g_out_pending;
static struct UsbdDeviceReq g_in_req[USB_SEND_SLOTS];
static int g_last_error;

// ---- descriptors ------------------------------------------------------------

#define EP_COUNT 2

static struct DeviceDescriptor devdesc_hi = {
    .bLength = 18, .bDescriptorType = 0x01, .bcdUSB = 0x200,
    .bDeviceClass = 0, .bDeviceSubClass = 0, .bDeviceProtocol = 0,
    .bMaxPacketSize = 64,
    .idVendor = 0, .idProduct = 0, // filled in by the bus driver
    .bcdDevice = 0x100, .iManufacturer = 0, .iProduct = 0, .iSerialNumber = 0,
    .bNumConfigurations = 1,
};

static struct ConfigDescriptor confdesc_hi = {
    .bLength = 9, .bDescriptorType = 0x02, .wTotalLength = 9 + 9 + EP_COUNT * 7,
    .bNumInterfaces = 1, .bConfigurationValue = 1, .iConfiguration = 0,
    .bmAttributes = 0xC0, .bMaxPower = 0,
};

static struct InterfaceDescriptor interdesc_hi = {
    .bLength = 9, .bDescriptorType = 0x04, .bInterfaceNumber = PSPCAP_USB_INTERFACE,
    .bAlternateSetting = 0, .bNumEndpoints = EP_COUNT,
    .bInterfaceClass = 0xFF, .bInterfaceSubClass = 0x01, .bInterfaceProtocol = 0xFF,
    .iInterface = 1,
};

static struct EndpointDescriptor endpdesc_hi[EP_COUNT] = {
    { .bLength = 7, .bDescriptorType = 0x05, .bEndpointAddress = PSPCAP_EP_IN,
      .bmAttributes = 0x02, .wMaxPacketSize = 512, .bInterval = 0 },
    { .bLength = 7, .bDescriptorType = 0x05, .bEndpointAddress = PSPCAP_EP_OUT,
      .bmAttributes = 0x02, .wMaxPacketSize = 512, .bInterval = 0 },
};

static struct DeviceDescriptor devdesc_full;
static struct ConfigDescriptor confdesc_full;
static struct InterfaceDescriptor interdesc_full;
static struct EndpointDescriptor endpdesc_full[EP_COUNT];

// UTF-16 "pspcap"
static unsigned char g_string[] __attribute__((aligned(4))) = {
    14, 0x03, 'p', 0, 's', 0, 'p', 0, 'c', 0, 'a', 0, 'p', 0, 0, 0,
};

// Endpoint 0 is control; 1 = bulk IN, 2 = bulk OUT (numbered as in the descriptors).
static struct UsbEndpoint g_endp[EP_COUNT + 1] = {
    { 0, 0, 0 },
    { 1, 0, 0 },
    { 2, 0, 0 },
};

static struct UsbInterface g_intf = { 0xFFFFFFFF, 0, 1 };

static struct UsbData g_usbdata[2];

static void fill_usbdata(struct UsbData *d, const struct DeviceDescriptor *dev,
                         const struct ConfigDescriptor *conf,
                         const struct InterfaceDescriptor *intf,
                         const struct EndpointDescriptor *endp)
{
    memset(d, 0, sizeof(*d));
    memcpy(d->devdesc, dev, sizeof(*dev));
    d->config.pconfdesc = &d->confdesc;
    d->config.pinterfaces = &d->interfaces;
    d->config.pinterdesc = &d->interdesc;
    d->config.pendp = d->endp;
    memcpy(d->confdesc.desc, conf, sizeof(*conf));
    d->confdesc.pinterfaces = &d->interfaces;
    d->interfaces.pinterdesc[0] = &d->interdesc;
    d->interfaces.intcount = 1;
    memcpy(d->interdesc.desc, intf, sizeof(*intf));
    d->interdesc.pendp = d->endp;
    for (int i = 0; i < EP_COUNT; i++)
        memcpy(d->endp[i].desc, &endp[i], sizeof(endp[i]));
}

// ---- bus driver callbacks (may run in interrupt context: no I/O here) --------

static int on_control(int arg1, int arg2, struct DeviceRequest *req)
{
    (void)arg1;
    (void)arg2;
    (void)req;
    return 0;
}

static int on_func28(int arg1, int arg2, int arg3)
{
    (void)arg1;
    (void)arg2;
    (void)arg3;
    return 0;
}

static int on_attach(int speed, void *arg2, void *arg3)
{
    (void)arg2;
    (void)arg3;
    g_speed = speed;
    g_attached = 1;
    sceKernelSetEventFlag(g_events, EV_ATTACH);
    return 0;
}

static int on_detach(int arg1, int arg2, int arg3)
{
    (void)arg1;
    (void)arg2;
    (void)arg3;
    g_attached = 0;
    return 0;
}

static int on_out_done(struct UsbdDeviceReq *req, int arg2, int arg3)
{
    (void)req;
    (void)arg2;
    (void)arg3;
    sceKernelSetEventFlag(g_events, EV_OUT_DONE);
    return 0;
}

static int on_in_done(struct UsbdDeviceReq *req, int arg2, int arg3)
{
    (void)arg2;
    (void)arg3;
    sceKernelSetEventFlag(g_events, EV_IN_DONE0 << (int)req->arg);
    return 0;
}

static int on_start(int size, void *args)
{
    (void)size;
    (void)args;
    fill_usbdata(&g_usbdata[0], &devdesc_hi, &confdesc_hi, &interdesc_hi, endpdesc_hi);
    fill_usbdata(&g_usbdata[1], &devdesc_full, &confdesc_full, &interdesc_full, endpdesc_full);
    return 0;
}

static int on_stop(int size, void *args)
{
    (void)size;
    (void)args;
    return 0;
}

static struct UsbDriver g_driver = {
    DRIVER_NAME,
    EP_COUNT + 1,
    g_endp,
    &g_intf,
    &g_usbdata[0].devdesc, &g_usbdata[0].config,
    &g_usbdata[1].devdesc, &g_usbdata[1].config,
    (struct StringDescriptor *)g_string,
    on_control, on_func28, on_attach, on_detach,
    0,
    on_start, on_stop,
    NULL,
};

// ---- helpers ------------------------------------------------------------------

static int is_cached(const void *p)
{
    // 0x4xxxxxxx / 0xAxxxxxxx are the uncached mirrors.
    return ((u32)p & 0x40000000) == 0 && ((u32)p >> 29) != 5;
}

static void cancel_all(void)
{
    sceUsbbdReqCancelAll(&g_endp[1]);
    sceUsbbdReqCancelAll(&g_endp[2]);
    g_out_pending = 0;
}

// Waits for any of `bits`, polling the attach state every WAIT_SLICE_US.
// On success returns 0 with the matched bits in *hit; they are not cleared.
static int wait_bits(u32 bits, SceUInt timeout_us, u32 *hit)
{
    for (;;) {
        SceUInt slice = timeout_us < WAIT_SLICE_US ? timeout_us : WAIT_SLICE_US;
        u32 result = 0;
        int ret = sceKernelWaitEventFlag(g_events, bits, PSP_EVENT_WAITOR, &result, &slice);
        if (ret == 0) {
            *hit = result & bits;
            return 0;
        }
        if (!g_attached)
            return USB_ERR_DETACHED;
        if (ret != (int)SCE_KERNEL_ERROR_WAIT_TIMEOUT)
            return USB_ERR_REQUEST;
        if (timeout_us <= WAIT_SLICE_US)
            return USB_ERR_TIMEOUT;
        timeout_us -= WAIT_SLICE_US;
    }
}

// Waits for `bit` and clears it.
static int wait_done(u32 bit, SceUInt timeout_us)
{
    u32 hit = 0;
    int ret = wait_bits(bit, timeout_us, &hit);
    if (ret == 0)
        sceKernelClearEventFlag(g_events, ~bit);
    return ret;
}

// ---- public API -----------------------------------------------------------------

int usb_init(void)
{
    // Full-speed descriptors: same layout, 64-byte bulk packets.
    devdesc_full = devdesc_hi;
    confdesc_full = confdesc_hi;
    interdesc_full = interdesc_hi;
    for (int i = 0; i < EP_COUNT; i++) {
        endpdesc_full[i] = endpdesc_hi[i];
        endpdesc_full[i].wMaxPacketSize = 64;
    }

    g_events = sceKernelCreateEventFlag("pspcap_usb", 0x200, 0, NULL);
    if (g_events < 0)
        return g_events;
    return sceUsbbdRegister(&g_driver);
}

int usb_start(void)
{
    int ret = sceUsbStart(PSP_USBBUS_DRIVERNAME, 0, NULL);
    if (ret != 0)
        return ret;
    ret = sceUsbStart(DRIVER_NAME, 0, NULL);
    if (ret != 0)
        return ret;
    return sceUsbActivate(PSPCAP_USB_PID);
}

void usb_term(void)
{
    cancel_all();
    sceUsbDeactivate(PSPCAP_USB_PID);
    sceUsbStop(DRIVER_NAME, 0, NULL);
    sceUsbStop(PSP_USBBUS_DRIVERNAME, 0, NULL);
    sceUsbbdUnregister(&g_driver);
    if (g_events >= 0)
        sceKernelDeleteEventFlag(g_events);
    g_events = -1;
}

int usb_wait_attach(SceUInt timeout_us)
{
    u32 result = 0;
    int ret = sceKernelWaitEventFlag(g_events, EV_ATTACH, PSP_EVENT_WAITOR | PSP_EVENT_WAITCLEAR,
                                     &result, &timeout_us);
    if (ret != 0)
        return USB_ERR_TIMEOUT;
    // Drop anything left over from the previous connection.
    cancel_all();
    sceKernelClearEventFlag(g_events, ~(u32)(EV_OUT_DONE | EV_IN_ALL));
    return g_speed;
}

int usb_attached(void)
{
    return g_attached;
}

void usb_wake(void)
{
    if (g_events >= 0)
        sceKernelSetEventFlag(g_events, EV_WAKE);
}

// Receive that also returns USB_WOKEN when one of `wake_bits` is set.
static int recv_or_wake(void *buf, int size, SceUInt timeout_us, u32 wake_bits)
{
    if (!g_attached) {
        g_out_pending = 0;
        return USB_ERR_DETACHED;
    }

    if (!g_out_pending) {
        sceKernelDcacheInvalidateRange(buf, (size + 63) & ~63);
        memset(&g_out_req, 0, sizeof(g_out_req));
        g_out_req.endp = &g_endp[2];
        g_out_req.data = buf;
        g_out_req.size = size;
        g_out_req.func = on_out_done;
        sceKernelClearEventFlag(g_events, ~(u32)EV_OUT_DONE);
        int ret = sceUsbbdReqRecv(&g_out_req);
        if (ret < 0) {
            g_last_error = ret;
            return USB_ERR_REQUEST;
        }
        g_out_pending = 1;
    }

    u32 hit = 0;
    int ret = wait_bits(EV_OUT_DONE | wake_bits, timeout_us, &hit);
    if (ret == USB_ERR_TIMEOUT)
        return ret;
    if (ret < 0) {
        cancel_all();
        return ret;
    }
    // A finished command wins over a wake-up, so commands are never starved.
    if (!(hit & EV_OUT_DONE)) {
        sceKernelClearEventFlag(g_events, ~wake_bits);
        return USB_WOKEN;
    }
    sceKernelClearEventFlag(g_events, ~(u32)EV_OUT_DONE);
    g_out_pending = 0;
    if (g_out_req.retcode != 0) {
        g_last_error = g_out_req.retcode;
        return USB_ERR_REQUEST;
    }
    return g_out_req.recvsize;
}

int usb_recv(void *buf, int size, SceUInt timeout_us)
{
    return recv_or_wake(buf, size, timeout_us, 0);
}

int usb_recv_or_wake(void *buf, int size, SceUInt timeout_us)
{
    return recv_or_wake(buf, size, timeout_us, EV_WAKE);
}

int usb_send_start(int slot, const void *buf, int size)
{
    if (!g_attached)
        return USB_ERR_DETACHED;

    if (is_cached(buf))
        sceKernelDcacheWritebackRange(buf, size);

    struct UsbdDeviceReq *req = &g_in_req[slot];
    memset(req, 0, sizeof(*req));
    req->endp = &g_endp[1];
    req->data = (void *)buf;
    req->size = size;
    req->func = on_in_done;
    req->arg = (void *)slot;
    sceKernelClearEventFlag(g_events, ~(u32)(EV_IN_DONE0 << slot));
    int ret = sceUsbbdReqSend(req);
    if (ret < 0) {
        g_last_error = ret;
        return USB_ERR_REQUEST;
    }
    return 0;
}

int usb_send_wait(int slot, SceUInt timeout_us)
{
    int ret = wait_done(EV_IN_DONE0 << slot, timeout_us);
    if (ret < 0) {
        cancel_all();
        return ret;
    }
    if (g_in_req[slot].retcode != 0) {
        g_last_error = g_in_req[slot].retcode;
        cancel_all();
        return USB_ERR_REQUEST;
    }
    return g_in_req[slot].recvsize;
}

int usb_send(const void *buf, int size, SceUInt timeout_us)
{
    int ret = usb_send_start(0, buf, size);
    if (ret < 0)
        return ret;
    return usb_send_wait(0, timeout_us);
}

int usb_last_error(void)
{
    return g_last_error;
}
