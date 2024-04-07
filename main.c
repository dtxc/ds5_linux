#include <stdio.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>
#include <string.h>
#include <dirent.h>
#include <libudev.h>


#define EVENT_BUTTON    0x01
#define EVENT_AXIS      0x02

#define AXIS_DEADZONE   5000
#define AXIS_MAX        32767
#define AXIS_MIN        -32767

#define BTN_X           0x00
#define BTN_O           0x01
#define BTN_DELTA       0x02
#define BTN_SQUARE      0x03
#define BTN_L1          0x04
#define BTN_R1          0x05
#define BTN_L2          0x06
#define BTN_R2          0x07
#define BTN_SHARE       0x08
#define BTN_SETTINGS    0x09
#define BTN_PS          0x0A
#define BTN_L3          0x0B
#define BTN_R3          0x0C


struct js_event {
    uint32_t time; // in ms
    int16_t value; 
    uint8_t type;
    uint8_t num; // axis/button num
} __attribute__((packed));

struct axis_state {
    short x, y;
};

int GET_AXIS(int x) {
    switch (x) {
        case 0:
            return 0;
        case 1:
            return 0;
        case 3:
            return 1;
        case 4:
            return 1;
        case 2:
            return 2;
        case 5:
            return 2;
        case 6:
            return 3;
        case 7:
            return 3;
        default:
            return -1;
    }
}


int read_event(int fd, struct js_event *ev) {
    ssize_t bytes = read(fd, ev, sizeof(*ev));

    if (bytes == sizeof(*ev)) {
        return 0;
    }

    return -1;
}

int read_axis_state(struct js_event *ev, struct axis_state axes[4]) {
    int axis = ev->num;

    if (axis < 8) {
        // x -> 0, 2, 3, 6
        // y -> 1, 4, 5, 7
        if (axis == 0 || axis == 3 || axis == 6) {
            axes[GET_AXIS(axis)].x = ev->value;
        } else if (axis == 2){
            axes[GET_AXIS(axis)].x = -ev->value;
        } else {
            axes[GET_AXIS(axis)].y = -ev->value;
        }
    }

    return GET_AXIS(axis);
}  

char *get_led_path(char *sysfs_path) {
    char *ret = malloc(256);
    strcpy(ret, sysfs_path);
    strcat(ret, "leds/");

    DIR *dp;
    struct dirent *ep;

    dp = opendir(ret);
    if (!dp) {
        perror("get_led_path(): failed to open dir");
    }

    while ((ep = readdir(dp)) != NULL) {
        if (strstr(ep->d_name, "rgb") != NULL) {
            strcat(ret, ep->d_name);
            break;
        }
    }

    strcat(ret, "/multi_intensity");

    closedir(dp);

    return ret;
}

// requires root
void set_led(char *led_path, uint8_t red, uint8_t green, uint8_t blue) {
    FILE *fp = fopen(led_path, "w");
    char data[11];

    sprintf(data, "%d %d %d", red, green, blue);
    fprintf(fp, "%s", data);
    fclose(fp);
}

// function used for checking if a point is in the deadzone
int f(short x, short y) {
    return abs(x) + abs(y) + abs(abs(x) - abs(y));
}

int main() {
    if (geteuid() != 0) {
        perror("permission denied");
        return -1;
    }

    int axis;
    struct js_event event;

    /* axis 0 -> (l_x, l_y)    axes 0 + 1
     * axis 1 -> (r_x, r_y)    axes 3 + 4
     * axis 2 -> (l2, r2)      axes 2 + 5
     * axis 3 -> arrow buttons axes 6 + 7 */
    struct axis_state axes[4] = {0};

    const char *path = malloc(256);
    // the sysfs path is needed to change the led colors
    char sysfs_path[88];

    struct udev *udev;
    struct udev_device *dev;
    struct udev_enumerate *enumerate;
    struct udev_list_entry *devices, *dev_entry;

    // create context
    udev = udev_new();
    if (!udev) {
        perror("failed to create udev context");
    }

    enumerate = udev_enumerate_new(udev);
    if (!enumerate) {
        udev_unref(udev);
        perror("failed to create enumerate object");
    }

    // include only input devices
    udev_enumerate_add_match_subsystem(enumerate, "input");
    udev_enumerate_scan_devices(enumerate);

    devices = udev_enumerate_get_list_entry(enumerate);

    udev_list_entry_foreach(dev_entry, devices) {
        path = udev_list_entry_get_name(dev_entry);
        dev = udev_device_new_from_syspath(udev, path);

        if (!dev) {
            perror("failed to create device");
        }

        const char *devnode = udev_device_get_devnode(dev);
        if (devnode && strcmp(devnode, "/dev/input/js0") == 0) {
            char *temp = (char *) udev_device_get_syspath(dev);
            temp[87] = '\0';

            strcpy(sysfs_path, temp);
            udev_device_unref(dev);
            break;
        }

        udev_device_unref(dev);
    }

    udev_enumerate_unref(enumerate);
    udev_unref(udev);

    char *led_path = get_led_path(sysfs_path);
    set_led(led_path, 100, 100, 100);

    int fd = open("/dev/input/js0", O_RDONLY);
    if (fd == -1) {
        perror("failed to load device");
    }

    // this will run until an I/O error is thrown (disconnect)
    while(!read_event(fd, &event)) {
        if (event.type == EVENT_BUTTON) {
            // event.value tells us whether the button was released or pressed.
            // 1 -> pressed, 0 -> released
            printf("event received: %u, %u\n", event.num, event.value);

            // on release event
            if (event.value == 0) {
                // axes data is not automatically cleared
                if (event.num == BTN_R2) axes[2].y = 0;
                if (event.num == BTN_L2) axes[2].x = 0;
            }
        } else if (event.type == EVENT_AXIS) {
            axis = read_axis_state(&event, axes);

            if (axis != 2) { // axis 2 starts from AXIS_MAX
                // if f(x,y) < 2r, the point (x,y) is in the deadzone
                // Cf is a square
                if (f(axes[axis].x, axes[axis].y) < 2 * AXIS_DEADZONE) {
                    // reset axis data
                    axes[axis].x = 0;
                    axes[axis].y = 0;
                    continue;
                }
            }

            printf("axis %u at (%6d, %6d)\n", axis, axes[axis].x, axes[axis].y);
        }
    }

    // I/O error
    printf("controller disconnected");
    return 0;
}
