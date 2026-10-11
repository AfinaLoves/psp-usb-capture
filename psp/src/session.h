// Command loop for one USB connection (see common/pspcap_protocol.h).
#ifndef PSPCAP_SESSION_H
#define PSPCAP_SESSION_H

// Serves PC commands until the cable is unplugged. `usb_speed` as reported
// by usb_wait_attach().
void session_run(int usb_speed);

#endif
