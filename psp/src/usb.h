// Vendor-specific USB device driver: one bulk IN and one bulk OUT endpoint.
//
// All functions except usb_init() must be called from a single thread.
// Buffers passed to usb_recv()/usb_send*() must be 64-byte aligned.
#ifndef PSPCAP_USB_H
#define PSPCAP_USB_H

#include <pspkerneltypes.h>

#define USB_ERR_TIMEOUT  (-1) // nothing completed in time; a recv stays queued
#define USB_ERR_DETACHED (-2) // cable unplugged or host went away
#define USB_ERR_REQUEST  (-3) // request rejected or completed with an error
#define USB_WOKEN        (-4) // usb_recv_or_wake(): usb_wake() was called

#define USB_SEND_SLOTS 4

// Registers the driver with the USB bus. Call from module_start.
int usb_init(void);

// Starts the bus and our driver and activates PSPCAP_USB_PID.
// Returns 0 or the first failing sce* error code.
int usb_start(void);

// Stops everything started by usb_start() and unregisters the driver.
void usb_term(void);

// Waits until the host configures the device. Returns the speed
// (1 = full, 2 = high), or USB_ERR_TIMEOUT.
int usb_wait_attach(SceUInt timeout_us);

int usb_attached(void);

// Receives one transfer into `buf` (up to `size` bytes). On USB_ERR_TIMEOUT
// the request stays queued and the next call with the same buffer keeps
// waiting for it. Returns the number of bytes received or a USB_ERR_*.
int usb_recv(void *buf, int size, SceUInt timeout_us);

// Like usb_recv(), but also returns USB_WOKEN once usb_wake() has been called
// (the receive stays queued). A received command is reported first.
int usb_recv_or_wake(void *buf, int size, SceUInt timeout_us);

// Wakes a thread blocked in usb_recv_or_wake(). Safe from any thread,
// including a syscall hook running on a game thread.
void usb_wake(void);

// Queues a bulk IN transfer in `slot` (0..USB_SEND_SLOTS-1) without waiting.
int usb_send_start(int slot, const void *buf, int size);

// Waits for the transfer queued in `slot`. Returns bytes sent or a USB_ERR_*;
// on any error all pending requests are cancelled.
int usb_send_wait(int slot, SceUInt timeout_us);

// usb_send_start + usb_send_wait on slot 0.
int usb_send(const void *buf, int size, SceUInt timeout_us);

// Raw cause of the last USB_ERR_REQUEST: the sce error code of a rejected
// request or the retcode of a failed one.
int usb_last_error(void);

#endif
