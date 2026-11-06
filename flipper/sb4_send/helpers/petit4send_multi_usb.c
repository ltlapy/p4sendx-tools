#include "petit4send_multi_usb.h"

#include <furi_hal_usb.h>
#include <furi_hal_usb_hid.h>

#include "usb.h"
#include "usb_hid.h"

#define USB_CTRL_SIZE 8
#define EP_BASE       0x81
#define REPORT_SIZE   8
#define IFACE_SIZE    25
#define CONFIG_HEAD   9
#define HID_OFFSET    9 /* into an interface's bytes: past the interface */
#define HID_SIZE      9

/* How long a report may wait for the one before it, in ms: more than the
 * 8 ms the Switch has been measured to poll at. */
#define SEND_TIMEOUT 20

#define REQ_GET_REPORT   0x01
#define REQ_GET_IDLE     0x02
#define REQ_GET_PROTOCOL 0x03
#define REQ_SET_REPORT   0x09
#define REQ_SET_IDLE     0x0A
#define REQ_SET_PROTOCOL 0x0B

static struct usb_device_descriptor device_desc = {
    .bLength = sizeof(struct usb_device_descriptor),
    .bDescriptorType = USB_DTYPE_DEVICE,
    .bcdUSB = VERSION_BCD(2, 0, 0),
    .bDeviceClass = USB_CLASS_PER_INTERFACE,
    .bDeviceSubClass = USB_SUBCLASS_NONE,
    .bDeviceProtocol = USB_PROTO_NONE,
    .bMaxPacketSize0 = USB_CTRL_SIZE,
    .idVendor = HID_VID_DEFAULT,
    .idProduct = HID_PID_DEFAULT,
    .bcdDevice = VERSION_BCD(1, 0, 0),
    .iManufacturer = NO_DESCRIPTOR,
    .iProduct = NO_DESCRIPTOR,
    .iSerialNumber = NO_DESCRIPTOR,
    .bNumConfigurations = 1,
};

static uint8_t config_desc[PETIT4_MULTI_CONFIG_SIZE];

static usbd_device* usb_dev;
static FuriSemaphore* ready[PETIT4_MULTI_KEYBOARDS];
static volatile bool connected;
static Petit4UsbStateCallback state_callback;
static void* state_context;
static FuriHalUsbInterface* previous_interface;
static uint8_t protocol[PETIT4_MULTI_KEYBOARDS];
static uint8_t idle[PETIT4_MULTI_KEYBOARDS];
static uint8_t reports[PETIT4_MULTI_KEYBOARDS][REPORT_SIZE];
static bool sent_once;
static uint8_t keyboards = PETIT4_MULTI_KEYBOARDS;

static void ep_callback(usbd_device* dev, uint8_t event, uint8_t ep) {
    UNUSED(dev);
    if(event == usbd_evt_eptx) furi_semaphore_release(ready[(ep & 0x07) - 1]);
}

static usbd_respond ep_config(usbd_device* dev, uint8_t cfg) {
    switch(cfg) {
    case 0:
        for(uint8_t i = 0; i < keyboards; i++) {
            usbd_ep_deconfig(dev, EP_BASE + i);
            usbd_reg_endpoint(dev, EP_BASE + i, 0);
        }
        return usbd_ack;
    case 1:
        for(uint8_t i = 0; i < keyboards; i++) {
            usbd_ep_config(dev, EP_BASE + i, USB_EPTYPE_INTERRUPT, REPORT_SIZE);
            usbd_reg_endpoint(dev, EP_BASE + i, ep_callback);
            usbd_ep_write(dev, EP_BASE + i, 0, 0);
        }
        return usbd_ack;
    default:
        return usbd_fail;
    }
}

static usbd_respond control(usbd_device* dev, usbd_ctlreq* req, usbd_rqc_callback* callback) {
    UNUSED(callback);
    uint8_t recipient = (USB_REQ_RECIPIENT | USB_REQ_TYPE) & req->bmRequestType;
    if(req->wIndex >= keyboards) return usbd_fail;
    uint8_t iface = req->wIndex;

    if(recipient == (USB_REQ_INTERFACE | USB_REQ_CLASS)) {
        switch(req->bRequest) {
        case REQ_GET_REPORT:
            dev->status.data_ptr = reports[iface];
            dev->status.data_count = REPORT_SIZE;
            return usbd_ack;
        case REQ_GET_IDLE:
            dev->status.data_ptr = &idle[iface];
            dev->status.data_count = 1;
            return usbd_ack;
        case REQ_GET_PROTOCOL:
            dev->status.data_ptr = &protocol[iface];
            dev->status.data_count = 1;
            return usbd_ack;
        case REQ_SET_REPORT: /* the keyboard's LEDs: nothing to show them on */
            return usbd_ack;
        case REQ_SET_IDLE:
            idle[iface] = req->wValue >> 8;
            return usbd_ack;
        case REQ_SET_PROTOCOL: /* the reports are the same in both protocols */
            protocol[iface] = req->wValue & 1;
            return usbd_ack;
        default:
            return usbd_fail;
        }
    }

    if(recipient == (USB_REQ_INTERFACE | USB_REQ_STANDARD) &&
       req->bRequest == USB_STD_GET_DESCRIPTOR) {
        switch(req->wValue >> 8) {
        case USB_DTYPE_HID:
            dev->status.data_ptr = &config_desc[CONFIG_HEAD + iface * IFACE_SIZE + HID_OFFSET];
            dev->status.data_count = HID_SIZE;
            return usbd_ack;
        case USB_DTYPE_HID_REPORT: {
            uint16_t size;
            dev->status.data_ptr = (uint8_t*)petit4_multi_report_desc(&size);
            dev->status.data_count = size;
            return usbd_ack;
        }
        default:
            return usbd_fail;
        }
    }
    return usbd_fail;
}

static void interface_init(usbd_device* dev, FuriHalUsbInterface* intf, void* ctx) {
    UNUSED(intf);
    UNUSED(ctx);
    usb_dev = dev;
    memset(protocol, 1, sizeof(protocol));
    usbd_reg_config(dev, ep_config);
    usbd_reg_control(dev, control);
    usbd_connect(dev, true);
}

static void interface_deinit(usbd_device* dev) {
    usbd_reg_config(dev, NULL);
    usbd_reg_control(dev, NULL);
    connected = false;
}

static void interface_wakeup(usbd_device* dev) {
    UNUSED(dev);
    if(!connected) {
        connected = true;
        if(state_callback) state_callback(true, state_context);
    }
}

static void interface_suspend(usbd_device* dev) {
    UNUSED(dev);
    if(connected) {
        connected = false;
        for(uint8_t i = 0; i < PETIT4_MULTI_KEYBOARDS; i++) furi_semaphore_release(ready[i]);
        if(state_callback) state_callback(false, state_context);
    }
}

static FuriHalUsbInterface multi_interface = {
    .init = interface_init,
    .deinit = interface_deinit,
    .wakeup = interface_wakeup,
    .suspend = interface_suspend,
    .dev_descr = &device_desc,
    .str_manuf_descr = NULL,
    .str_prod_descr = NULL,
    .str_serial_descr = NULL,
    .cfg_descr = (void*)config_desc,
};

void petit4_multi_usb_set_keyboards(uint8_t count) {
    keyboards = count;
}

void petit4_multi_usb_start(void) {
    petit4_multi_config_desc(config_desc, keyboards);
    for(uint8_t i = 0; i < PETIT4_MULTI_KEYBOARDS; i++) ready[i] = furi_semaphore_alloc(1, 1);
    memset(reports, 0, sizeof(reports));
    sent_once = false;
    previous_interface = furi_hal_usb_get_config();
    furi_hal_usb_unlock();
    furi_check(furi_hal_usb_set_config(&multi_interface, NULL));
}

void petit4_multi_usb_stop(void) {
    furi_check(furi_hal_usb_set_config(previous_interface, NULL));
    state_callback = NULL;
    for(uint8_t i = 0; i < PETIT4_MULTI_KEYBOARDS; i++) {
        furi_semaphore_free(ready[i]);
        ready[i] = NULL;
    }
    usb_dev = NULL;
}

bool petit4_multi_usb_is_connected(void) {
    return connected;
}

void petit4_multi_usb_set_state_callback(Petit4UsbStateCallback callback, void* context) {
    state_callback = NULL;
    state_context = context;
    state_callback = callback;
    if(callback && connected) callback(true, context);
}

bool petit4_multi_usb_send(const Petit4MultiPacket* packet) {
    bool sent = true;
    for(int8_t i = keyboards - 1; i >= 0; i--) {
        uint8_t report[REPORT_SIZE] = {0};
        report[0] = i == 0 ? packet->mods : 0;
        memcpy(&report[2], packet->keys[i], 6);
        if(sent_once && memcmp(reports[i], report, REPORT_SIZE) == 0) continue;
        memcpy(reports[i], report, REPORT_SIZE);
        if(!connected || furi_semaphore_acquire(ready[i], SEND_TIMEOUT) != FuriStatusOk ||
           !connected) {
            sent = false;
            continue;
        }
        usbd_ep_write(usb_dev, EP_BASE + i, reports[i], REPORT_SIZE);
    }
    sent_once = true;
    return sent;
}

bool petit4_multi_usb_release_all(void) {
    Petit4MultiPacket nothing;
    memset(&nothing, 0, sizeof(nothing));
    return petit4_multi_usb_send(&nothing);
}
