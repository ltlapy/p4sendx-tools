#include "petit4send_usb.h"

#include <furi_hal_usb.h>
#include <furi_hal_usb_hid.h>
#include <stm32wbxx.h>

#include "usb.h"
#include "usb_hid.h"

#define EP_KEYBOARD   0x81
#define EP_MOUSE      0x82
#define EP_SIZE       8
#define EP_INTERVAL   2
#define USB_CTRL_SIZE 8

#define INTF_KEYBOARD 0
#define INTF_MOUSE    1
#define INTF_COUNT    2

#define KEYBOARD_REPORT_SIZE 8
#define MOUSE_REPORT_SIZE    4

/* How long a report may wait for the one before it, in ms: more than the
 * 8 ms the Switch has been measured to poll at. */
#define SEND_TIMEOUT 20

/* Often enough to see every turn of the 11-bit frame number. */
#define FRAME_TIMER_MS  500
#define FRAME_MASK      0x7FF

#define REQ_GET_REPORT   0x01
#define REQ_GET_IDLE     0x02
#define REQ_GET_PROTOCOL 0x03
#define REQ_SET_REPORT   0x09
#define REQ_SET_IDLE     0x0A
#define REQ_SET_PROTOCOL 0x0B

/* A boot keyboard, but with all key usages up to 255. */
static const uint8_t keyboard_report_desc[] = {
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, /* Generic Desktop, Keyboard */
    0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00, 0x25, 0x01, /* modifiers */
    0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
    0x95, 0x01, 0x75, 0x08, 0x81, 0x01, /* reserved */
    0x95, 0x05, 0x75, 0x01, 0x05, 0x08, 0x19, 0x01, 0x29, 0x05, 0x91, 0x02, /* LEDs */
    0x95, 0x01, 0x75, 0x03, 0x91, 0x01,
    0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x26, 0xFF, 0x00, /* keys */
    0x05, 0x07, 0x19, 0x00, 0x2A, 0xFF, 0x00, 0x81, 0x00,
    0xC0,
};

/* Five buttons: P4SEND reads them all. */
static const uint8_t mouse_report_desc[] = {
    0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x09, 0x01, 0xA1, 0x00, /* Mouse, Pointer */
    0x05, 0x09, 0x19, 0x01, 0x29, 0x05, 0x15, 0x00, 0x25, 0x01, /* buttons */
    0x95, 0x05, 0x75, 0x01, 0x81, 0x02,
    0x95, 0x01, 0x75, 0x03, 0x81, 0x01, /* padding */
    0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x09, 0x38, /* X, Y, wheel */
    0x15, 0x81, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x03, 0x81, 0x06,
    0xC0, 0xC0,
};

struct IntfDescriptor {
    struct usb_interface_descriptor intf;
    struct usb_hid_descriptor hid;
    struct usb_endpoint_descriptor ep;
} FURI_PACKED;

struct ConfigDescriptor {
    struct usb_config_descriptor config;
    struct IntfDescriptor keyboard;
    struct IntfDescriptor mouse;
} FURI_PACKED;

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

#define INTF_DESCRIPTOR(number, subclass, protocol, report_desc, endpoint)     \
    {                                                                          \
        .intf =                                                                \
            {                                                                  \
                .bLength = sizeof(struct usb_interface_descriptor),            \
                .bDescriptorType = USB_DTYPE_INTERFACE,                        \
                .bInterfaceNumber = number,                                    \
                .bAlternateSetting = 0,                                        \
                .bNumEndpoints = 1,                                            \
                .bInterfaceClass = USB_CLASS_HID,                              \
                .bInterfaceSubClass = subclass,                                \
                .bInterfaceProtocol = protocol,                                \
                .iInterface = NO_DESCRIPTOR,                                   \
            },                                                                 \
        .hid =                                                                 \
            {                                                                  \
                .bLength = sizeof(struct usb_hid_descriptor),                  \
                .bDescriptorType = USB_DTYPE_HID,                              \
                .bcdHID = VERSION_BCD(1, 1, 0),                                \
                .bCountryCode = USB_HID_COUNTRY_NONE,                          \
                .bNumDescriptors = 1,                                          \
                .bDescriptorType0 = USB_DTYPE_HID_REPORT,                      \
                .wDescriptorLength0 = sizeof(report_desc),                     \
            },                                                                 \
        .ep = {                                                                \
            .bLength = sizeof(struct usb_endpoint_descriptor),                 \
            .bDescriptorType = USB_DTYPE_ENDPOINT,                             \
            .bEndpointAddress = endpoint,                                      \
            .bmAttributes = USB_EPTYPE_INTERRUPT,                              \
            .wMaxPacketSize = EP_SIZE,                                         \
            .bInterval = EP_INTERVAL,                                          \
        },                                                                     \
    }

static const struct ConfigDescriptor config_desc = {
    .config =
        {
            .bLength = sizeof(struct usb_config_descriptor),
            .bDescriptorType = USB_DTYPE_CONFIGURATION,
            .wTotalLength = sizeof(struct ConfigDescriptor),
            .bNumInterfaces = INTF_COUNT,
            .bConfigurationValue = 1,
            .iConfiguration = NO_DESCRIPTOR,
            .bmAttributes = USB_CFG_ATTR_RESERVED | USB_CFG_ATTR_SELFPOWERED,
            .bMaxPower = USB_CFG_POWER_MA(100),
        },
    .keyboard = INTF_DESCRIPTOR(
        INTF_KEYBOARD,
        USB_HID_SUBCLASS_BOOT,
        USB_HID_PROTO_KEYBOARD,
        keyboard_report_desc,
        EP_KEYBOARD),
    .mouse = INTF_DESCRIPTOR(
        INTF_MOUSE,
        USB_HID_SUBCLASS_NONBOOT,
        USB_HID_PROTO_NONBOOT,
        mouse_report_desc,
        EP_MOUSE),
};

static usbd_device* usb_dev;
static FuriSemaphore* keyboard_ready;
static FuriSemaphore* mouse_ready;
static volatile bool connected;
static Petit4UsbStateCallback state_callback;
static void* state_context;
static FuriHalUsbInterface* previous_interface;
static uint8_t protocol[INTF_COUNT];
static uint8_t idle[INTF_COUNT];
static uint8_t keyboard_report[KEYBOARD_REPORT_SIZE];
static uint8_t mouse_report[MOUSE_REPORT_SIZE];
static FuriTimer* frame_timer;
static uint32_t frame_ms;
static uint16_t frame_last;

static void ep_callback(usbd_device* dev, uint8_t event, uint8_t ep) {
    UNUSED(dev);
    if(event == usbd_evt_eptx) {
        furi_semaphore_release(ep == EP_KEYBOARD ? keyboard_ready : mouse_ready);
    }
}

static usbd_respond ep_config(usbd_device* dev, uint8_t cfg) {
    switch(cfg) {
    case 0:
        usbd_ep_deconfig(dev, EP_KEYBOARD);
        usbd_ep_deconfig(dev, EP_MOUSE);
        usbd_reg_endpoint(dev, EP_KEYBOARD, 0);
        usbd_reg_endpoint(dev, EP_MOUSE, 0);
        return usbd_ack;
    case 1:
        usbd_ep_config(dev, EP_KEYBOARD, USB_EPTYPE_INTERRUPT, EP_SIZE);
        usbd_ep_config(dev, EP_MOUSE, USB_EPTYPE_INTERRUPT, EP_SIZE);
        usbd_reg_endpoint(dev, EP_KEYBOARD, ep_callback);
        usbd_reg_endpoint(dev, EP_MOUSE, ep_callback);
        usbd_ep_write(dev, EP_KEYBOARD, 0, 0);
        usbd_ep_write(dev, EP_MOUSE, 0, 0);
        return usbd_ack;
    default:
        return usbd_fail;
    }
}

static usbd_respond control(usbd_device* dev, usbd_ctlreq* req, usbd_rqc_callback* callback) {
    UNUSED(callback);
    uint8_t recipient = (USB_REQ_RECIPIENT | USB_REQ_TYPE) & req->bmRequestType;
    if(req->wIndex >= INTF_COUNT) return usbd_fail;
    bool is_keyboard = req->wIndex == INTF_KEYBOARD;

    if(recipient == (USB_REQ_INTERFACE | USB_REQ_CLASS)) {
        switch(req->bRequest) {
        case REQ_GET_REPORT:
            dev->status.data_ptr = is_keyboard ? keyboard_report : mouse_report;
            dev->status.data_count = is_keyboard ? KEYBOARD_REPORT_SIZE : MOUSE_REPORT_SIZE;
            return usbd_ack;
        case REQ_GET_IDLE:
            dev->status.data_ptr = &idle[req->wIndex];
            dev->status.data_count = 1;
            return usbd_ack;
        case REQ_GET_PROTOCOL:
            dev->status.data_ptr = &protocol[req->wIndex];
            dev->status.data_count = 1;
            return usbd_ack;
        case REQ_SET_REPORT: /* the keyboard's LEDs: nothing to show them on */
            return usbd_ack;
        case REQ_SET_IDLE:
            idle[req->wIndex] = req->wValue >> 8;
            return usbd_ack;
        case REQ_SET_PROTOCOL: /* the reports are the same in both protocols */
            protocol[req->wIndex] = req->wValue & 1;
            return usbd_ack;
        default:
            return usbd_fail;
        }
    }

    if(recipient == (USB_REQ_INTERFACE | USB_REQ_STANDARD) &&
       req->bRequest == USB_STD_GET_DESCRIPTOR) {
        const struct IntfDescriptor* intf = is_keyboard ? &config_desc.keyboard :
                                                          &config_desc.mouse;
        switch(req->wValue >> 8) {
        case USB_DTYPE_HID:
            dev->status.data_ptr = (uint8_t*)&intf->hid;
            dev->status.data_count = sizeof(intf->hid);
            return usbd_ack;
        case USB_DTYPE_HID_REPORT:
            dev->status.data_ptr = is_keyboard ? (uint8_t*)keyboard_report_desc :
                                                 (uint8_t*)mouse_report_desc;
            dev->status.data_count = is_keyboard ? sizeof(keyboard_report_desc) :
                                                   sizeof(mouse_report_desc);
            return usbd_ack;
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
    protocol[INTF_KEYBOARD] = 1;
    protocol[INTF_MOUSE] = 1;
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
        furi_semaphore_release(keyboard_ready);
        furi_semaphore_release(mouse_ready);
        if(state_callback) state_callback(false, state_context);
    }
}

static FuriHalUsbInterface petit4_interface = {
    .init = interface_init,
    .deinit = interface_deinit,
    .wakeup = interface_wakeup,
    .suspend = interface_suspend,
    .dev_descr = &device_desc,
    .str_manuf_descr = NULL,
    .str_prod_descr = NULL,
    .str_serial_descr = NULL,
    .cfg_descr = (void*)&config_desc,
};

uint32_t petit4_usb_ms(void) {
    FURI_CRITICAL_ENTER();
    uint16_t frame = USB->FNR & FRAME_MASK;
    frame_ms += (frame - frame_last) & FRAME_MASK;
    frame_last = frame;
    uint32_t ms = frame_ms;
    FURI_CRITICAL_EXIT();
    return ms;
}

static void frame_timer_callback(void* context) {
    UNUSED(context);
    petit4_usb_ms();
}

void petit4_usb_start(uint16_t vid, uint16_t pid) {
    device_desc.idVendor = vid ? vid : HID_VID_DEFAULT;
    device_desc.idProduct = pid ? pid : HID_PID_DEFAULT;
    keyboard_ready = furi_semaphore_alloc(1, 1);
    mouse_ready = furi_semaphore_alloc(1, 1);
    memset(keyboard_report, 0, sizeof(keyboard_report));
    memset(mouse_report, 0, sizeof(mouse_report));
    previous_interface = furi_hal_usb_get_config();
    furi_hal_usb_unlock();
    furi_check(furi_hal_usb_set_config(&petit4_interface, NULL));
    frame_ms = 0;
    frame_last = USB->FNR & FRAME_MASK;
    frame_timer = furi_timer_alloc(frame_timer_callback, FuriTimerTypePeriodic, NULL);
    furi_timer_start(frame_timer, FRAME_TIMER_MS);
}

void petit4_usb_stop(void) {
    furi_timer_stop(frame_timer);
    furi_timer_free(frame_timer);
    frame_timer = NULL;
    furi_check(furi_hal_usb_set_config(previous_interface, NULL));
    state_callback = NULL;
    furi_semaphore_free(keyboard_ready);
    furi_semaphore_free(mouse_ready);
    keyboard_ready = NULL;
    mouse_ready = NULL;
    usb_dev = NULL;
}

bool petit4_usb_is_connected(void) {
    return connected;
}

void petit4_usb_set_state_callback(Petit4UsbStateCallback callback, void* context) {
    state_callback = NULL;
    state_context = context;
    state_callback = callback;
    if(callback && connected) callback(true, context);
}

static bool send_report(FuriSemaphore* ready, uint8_t ep, const uint8_t* report, size_t size) {
    if(!connected) return false;
    if(furi_semaphore_acquire(ready, SEND_TIMEOUT) != FuriStatusOk) return false;
    if(!connected) return false;
    usbd_ep_write(usb_dev, ep, (void*)report, size);
    return true;
}

bool petit4_usb_send(const Petit4Packet* packet) {
    bool sent = true;
    /* The buttons only change for a packet the receiver does not read them
     * with, so they need not arrive together with the keys. */
    if(mouse_report[0] != packet->mouse) {
        mouse_report[0] = packet->mouse;
        sent = send_report(mouse_ready, EP_MOUSE, mouse_report, MOUSE_REPORT_SIZE);
    }
    keyboard_report[0] = packet->mods;
    keyboard_report[1] = 0;
    memcpy(&keyboard_report[2], packet->keys, sizeof(packet->keys));
    return send_report(keyboard_ready, EP_KEYBOARD, keyboard_report, KEYBOARD_REPORT_SIZE) && sent;
}

bool petit4_usb_release_all(void) {
    Petit4Packet nothing;
    memset(&nothing, 0, sizeof(nothing));
    return petit4_usb_send(&nothing);
}
