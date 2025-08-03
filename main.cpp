#include <gtk/gtk.h>
#include <libevdev/libevdev.h>
#include <thread>
#include <atomic>
#include <fcntl.h>
#include <unistd.h>
#include <vector>
#include <string>
#include <linux/input.h>
#include <linux/uinput.h>
#include <sys/ioctl.h>
#include <pthread.h>
#include <cmath>
#include <cerrno>
#include <cstring>
#include <iostream>

// Config structure
typedef struct {
    int deadzone;
    float sensitivity;
    int mouse_speed;
} Config;

struct Key {
    std::string label;
    GtkWidget *button;
    int row, col, width;
};

// Global variables
std::atomic<int> mode(0); // 0 = keyboard mode, 1 = game mode
std::vector<Key> keys;
std::atomic<int> currentIndex(0);
std::atomic<bool> running(true);
GtkWidget *window = nullptr;
GtkWidget *grid = nullptr;
static Config config = { .deadzone = 200, .sensitivity = 1.0, .mouse_speed = 10 };
static int uinput_fd = -1;
pthread_t movement_thread;
std::atomic<bool> movement_thread_running(false);

// Joystick state for mouse control
typedef struct {
    float right_x;
    float right_y;
    pthread_mutex_t mutex;
} JoystickState;
static JoystickState joystick_state = { 0.0f, 0.0f, PTHREAD_MUTEX_INITIALIZER };
static GtkCssProvider *css_provider = nullptr;

// Function prototypes
void setup_uinput();
void* movement_thread_func(void* arg);
void send_mouse_rel(int fd, int dx, int dy);
void send_key(int fd, int key, int value);
int apply_deadzone(int value, int deadzone);
float analog_to_key_speed(int value, int deadzone, float sensitivity);
void focusButton(int index);
bool moveFocus(int dx, int dy);
int findKeyIndex(const std::string& label);

// GTK UI functions
GtkWidget* createKeyButton(const std::string& label) {
    GtkWidget *button = gtk_button_new_with_label(label.c_str());
    int width = 40;
    if (label.length() > 4) width = 27;
    if (label == "SPACE") width = 100;
    gtk_widget_set_size_request(button, width, 30);

    if (!css_provider) {
        css_provider = gtk_css_provider_new();
        const char *css = 
            "button { font-size: 8px; padding: 2px; margin: 1px; border: 1px solid #ccc; border-radius: 3px; }"
            "button:focus { background-color: #aaf; border: 2px solid #00f; }";
        gtk_css_provider_load_from_data(css_provider, css, -1, nullptr);
    }
    
    gtk_style_context_add_provider(gtk_widget_get_style_context(button),
                                 GTK_STYLE_PROVIDER(css_provider),
                                 GTK_STYLE_PROVIDER_PRIORITY_USER);
    return button;
}

void setupKeyboard() {
    std::vector<std::vector<std::string>> keyboardLayout = {
        {"ESC", "F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12"},
        {"`", "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "-", "=", "BACKSPACE"},
        {"TAB", "Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", "[", "]", "\\"},
        {"CAPS", "A", "S", "D", "F", "G", "H", "J", "K", "L", ";", "'", "ENTER"},
        {"SHIFT", "Z", "X", "C", "V", "B", "N", "M", ",", ".", "/", "SHIFT"},
        {"CTRL", "WIN", "ALT", "SPACE", "ALT", "WIN", "MENU", "CTRL"}
    };

    int row = 0;
    for (const auto& line : keyboardLayout) {
        int col = 0;
        for (const auto& label : line) {
            Key key;
            key.label = label;
            key.button = createKeyButton(label);
            key.row = row;
            key.col = col;

            int width = 1;
            if (label == "TAB") width = 2;
            else if (label == "BACKSPACE") width = 2;
            else if (label == "CAPS") width = 2;
            else if (label == "ENTER") width = 2;
            else if (label == "SHIFT") width = 2;
            else if (label == "SPACE") width = 6;

            key.width = width;
            gtk_grid_attach(GTK_GRID(grid), key.button, col, row, width, 1);

            keys.push_back(key);
            col += width;
        }
        row++;
    }
}

int findKeyIndex(const std::string& label) {
    for (size_t i = 0; i < keys.size(); ++i) {
        if (keys[i].label == label) {
            return i;
        }
    }
    return -1;
}

void focusButton(int index) {
    if (index < 0 || index >= static_cast<int>(keys.size())) return;
    currentIndex = index;
    g_idle_add([](gpointer data) -> gboolean {
        int idx = GPOINTER_TO_INT(data);
        if (idx >= 0 && idx < static_cast<int>(keys.size())) {
            gtk_widget_grab_focus(keys[idx].button);
        }
        return G_SOURCE_REMOVE;
    }, GINT_TO_POINTER(index));
}

bool moveFocus(int dx, int dy) {
    Key& current = keys[currentIndex];
    int bestScore = INT_MAX;
    int bestIndex = currentIndex;

    for (size_t i = 0; i < keys.size(); ++i) {
        if (static_cast<int>(i) == currentIndex) continue;
        Key& k = keys[i];

        if ((dx != 0 && ((dx > 0 && k.col <= current.col) || (dx < 0 && k.col >= current.col))) ||
            (dy != 0 && ((dy > 0 && k.row <= current.row) || (dy < 0 && k.row >= current.row)))) {
            continue;
        }

        int dcol = k.col - current.col;
        int drow = k.row - current.row;
        int score = dcol * dcol + drow * drow;

        if (score < bestScore) {
            bestScore = score;
            bestIndex = i;
        }
    }

    if (bestIndex != currentIndex) {
        focusButton(bestIndex);
        return true;
    }
    return false;
}

gboolean click_button_idle(gpointer data) {
    int idx = GPOINTER_TO_INT(data);
    if (idx >= 0 && idx < static_cast<int>(keys.size())) {
        gtk_button_clicked(GTK_BUTTON(keys[idx].button));
    }
    return G_SOURCE_REMOVE;
}

gboolean toggle_keyboard_visibility(gpointer data) {
    if (mode == 0) {
        gtk_widget_show_all(window);
    } else {
        gtk_widget_hide(window);
    }
    return G_SOURCE_REMOVE;
}

// Mouse Control

void send_mouse_wheel(int fd, int value) {
    struct input_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = EV_REL;
    ev.code = REL_WHEEL;
    ev.value = value; // Positive for wheel up, negative for wheel down
    if (write(fd, &ev, sizeof(ev)) < 0) {
        std::cerr << "Failed to write REL_WHEEL: " << strerror(errno) << std::endl;
    }
    
    memset(&ev, 0, sizeof(ev));
    ev.type = EV_SYN;
    ev.code = SYN_REPORT;
    ev.value = 0;
    if (write(fd, &ev, sizeof(ev)) < 0) {
        std::cerr << "Failed to write SYN_REPORT for wheel: " << strerror(errno) << std::endl;
    }
}

void setup_uinput() {
    struct uinput_setup usetup;
    
    uinput_fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (uinput_fd < 0) {
        std::cerr << "Cannot open /dev/uinput: " << strerror(errno) << std::endl;
        return;
    }
    
    // Enable key events
    if (ioctl(uinput_fd, UI_SET_EVBIT, EV_KEY) < 0) {
        std::cerr << "UI_SET_EVBIT (EV_KEY) failed: " << strerror(errno) << std::endl;
        close(uinput_fd);
        uinput_fd = -1;
        return;
    }
    
    // Enable all keyboard keys
    for (int i = 0; i < KEY_MAX; i++) {
        if (ioctl(uinput_fd, UI_SET_KEYBIT, i) < 0) {
            std::cerr << "UI_SET_KEYBIT failed for key " << i << ": " << strerror(errno) << std::endl;
        }
    }
    
    // Enable relative mouse movement and wheel
    if (ioctl(uinput_fd, UI_SET_EVBIT, EV_REL) < 0 ||
        ioctl(uinput_fd, UI_SET_RELBIT, REL_X) < 0 ||
        ioctl(uinput_fd, UI_SET_RELBIT, REL_Y) < 0 ||
        ioctl(uinput_fd, UI_SET_RELBIT, REL_WHEEL) < 0) { // Added REL_WHEEL
        std::cerr << "Failed to enable relative mouse events or wheel: " << strerror(errno) << std::endl;
        close(uinput_fd);
        uinput_fd = -1;
        return;
    }
    
    // Enable mouse buttons
    if (ioctl(uinput_fd, UI_SET_KEYBIT, BTN_LEFT) < 0 ||
        ioctl(uinput_fd, UI_SET_KEYBIT, BTN_RIGHT) < 0) {
        std::cerr << "Failed to enable mouse buttons: " << strerror(errno) << std::endl;
        close(uinput_fd);
        uinput_fd = -1;
        return;
    }
    
    // Setup device info
    memset(&usetup, 0, sizeof(usetup));
    usetup.id.bustype = BUS_USB;
    usetup.id.vendor = 0x1234;
    usetup.id.product = 0x5678;
    strcpy(usetup.name, "Virtual Mouse");
    
    if (ioctl(uinput_fd, UI_DEV_SETUP, &usetup) < 0) {
        std::cerr << "UI_DEV_SETUP failed: " << strerror(errno) << std::endl;
        close(uinput_fd);
        uinput_fd = -1;
        return;
    }
    
    if (ioctl(uinput_fd, UI_DEV_CREATE) < 0) {
        std::cerr << "UI_DEV_CREATE failed: " << strerror(errno) << std::endl;
        close(uinput_fd);
        uinput_fd = -1;
    }
}

void send_mouse_rel(int fd, int dx, int dy) {
    struct input_event ev;
    
    // Swap X and Y axes: dx controls REL_Y, dy controls REL_X
    if (dy != 0) { // Note: dy is used for REL_X
        memset(&ev, 0, sizeof(ev));
        ev.type = EV_REL;
        ev.code = REL_X;
        ev.value = -dy; // Swapped
        if (write(fd, &ev, sizeof(ev)) < 0) {
            std::cerr << "Failed to write REL_X: " << strerror(errno) << std::endl;
        }
    }
    
    if (dx != 0) { // Note: dx is used for REL_Y
        memset(&ev, 0, sizeof(ev));
        ev.type = EV_REL;
        ev.code = REL_Y;
        ev.value = -dx; // Swapped
        if (write(fd, &ev, sizeof(ev)) < 0) {
            std::cerr << "Failed to write REL_Y: " << strerror(errno) << std::endl;
        }
    }
    
    if (dx != 0 || dy != 0) {
        memset(&ev, 0, sizeof(ev));
        ev.type = EV_SYN;
        ev.code = SYN_REPORT;
        ev.value = 0;
        if (write(fd, &ev, sizeof(ev)) < 0) {
            std::cerr << "Failed to write SYN_REPORT: " << strerror(errno) << std::endl;
        }
    }
}

void send_key(int fd, int key, int value) {
    struct input_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = EV_KEY;
    ev.code = key;
    ev.value = value;
    if (write(fd, &ev, sizeof(ev)) < 0) {
        std::cerr << "Failed to write key event: " << strerror(errno) << std::endl;
    }
    
    memset(&ev, 0, sizeof(ev));
    ev.type = EV_SYN;
    ev.code = SYN_REPORT;
    ev.value = 0;
    if (write(fd, &ev, sizeof(ev)) < 0) {
        std::cerr << "Failed to write SYN_REPORT for key: " << strerror(errno) << std::endl;
    }
}

int apply_deadzone(int value, int deadzone) {
    if (std::abs(value) < deadzone) {
        return 0;
    }
    return value;
}

float analog_to_key_speed(int value, int deadzone, float sensitivity) {
    value = apply_deadzone(value, deadzone);
    if (value == 0) return 0.0f;
    
    float normalized = static_cast<float>(value) / 1800.0f;
    return normalized * sensitivity;
}

void* movement_thread_func(void* arg) {
    while (movement_thread_running) {
        pthread_mutex_lock(&joystick_state.mutex);
        float right_x = joystick_state.right_x;
        float right_y = joystick_state.right_y;
        pthread_mutex_unlock(&joystick_state.mutex);
        
        if (std::fabs(right_x) > 0.05f || std::fabs(right_y) > 0.05f) {
            int dx = static_cast<int>(right_x * config.mouse_speed);
            int dy = static_cast<int>(right_y * config.mouse_speed);
            send_mouse_rel(uinput_fd, dx, dy);
        }
        usleep(16667); // ~60 FPS
    }
    return nullptr;
}

// Joystick thread
void joystick_thread() {
    struct libevdev *dev = nullptr;
    int fd = open("/dev/input/event2", O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
        std::cerr << "Failed to open joystick device: " << strerror(errno) << std::endl;
        return;
    }

    if (libevdev_new_from_fd(fd, &dev) < 0) {
        std::cerr << "Failed to init libevdev: " << strerror(errno) << std::endl;
        close(fd);
        return;
    }

    const int DEADZONE = 400;
    int abs_x = 0, abs_y = 0, abs_rx = 0, abs_ry = 0;
    int last_move_x = 0, last_move_y = 0;

    // Start mouse control thread
    movement_thread_running = true;
    if (pthread_create(&movement_thread, nullptr, movement_thread_func, nullptr) != 0) {
        std::cerr << "Failed to create movement thread: " << strerror(errno) << std::endl;
        libevdev_free(dev);
        close(fd);
        return;
    }

    while (running) {
        struct input_event ev;
        int rc = libevdev_next_event(dev, LIBEVDEV_READ_FLAG_NORMAL, &ev);
        
        if (rc == 0) {
            // Handle mouse movement in both modes
            if (ev.type == EV_ABS && (ev.code == ABS_RX || ev.code == ABS_RY)) {
                if (ev.code == ABS_RX) abs_rx = ev.value;
                else if (ev.code == ABS_RY) abs_ry = ev.value;

                pthread_mutex_lock(&joystick_state.mutex);
                joystick_state.right_x = analog_to_key_speed(abs_rx, DEADZONE, config.sensitivity);
                joystick_state.right_y = analog_to_key_speed(abs_ry, DEADZONE, config.sensitivity);
                pthread_mutex_unlock(&joystick_state.mutex);
            }

            if (mode == 0) {
                // Keyboard navigation mode
                if (ev.type == EV_ABS && (ev.code == ABS_X || ev.code == ABS_Y)) {
                    if (ev.code == ABS_X) abs_x = ev.value;
                    else if (ev.code == ABS_Y) abs_y = ev.value;

                    int move_x = (abs_x > DEADZONE) ? -1 : (abs_x < -DEADZONE) ? 1 : 0;
                    int move_y = (abs_y > DEADZONE) ? -1 : (abs_y < -DEADZONE) ? 1 : 0;

                    if (move_x != last_move_x || move_y != last_move_y) {
                        if (move_x != 0) moveFocus(move_x, 0);
                        if (move_y != 0) moveFocus(0, move_y);
                        last_move_x = move_x;
                        last_move_y = move_y;
                    }
                }
                else if (ev.type == EV_KEY && ev.value == 1) {
                    if (ev.code == BTN_THUMBR) {
                        g_idle_add(click_button_idle, GINT_TO_POINTER(currentIndex));
                    }
                    else if (ev.code == BTN_MODE) {
                        mode = 1;
                        g_idle_add(toggle_keyboard_visibility, nullptr);
                        usleep(200000);
                    }
                    // Mouse buttons and wheel
                    else if (ev.code == BTN_TL) { // Left click
                        send_key(uinput_fd, BTN_LEFT, 1);
                    }
                    else if (ev.code == BTN_TR) { // Right click
                        send_key(uinput_fd, BTN_RIGHT, 1);
                    }
                    else if (ev.code == BTN_TL2) { // Wheel up
                        send_mouse_wheel(uinput_fd, 1);
                    }
                    else if (ev.code == BTN_TR2) { // Wheel down
                        send_mouse_wheel(uinput_fd, -1);
                    }
                }
                else if (ev.type == EV_KEY && ev.value == 0) {
                    if (ev.code == BTN_TL) {
                        send_key(uinput_fd, BTN_LEFT, 0);
                    }
                    else if (ev.code == BTN_TR) {
                        send_key(uinput_fd, BTN_RIGHT, 0);
                    }
                }
            }
            else if (mode == 1) {
                // Game control mode
                if (ev.type == EV_ABS && (ev.code == ABS_X || ev.code == ABS_Y)) {
                    if (ev.code == ABS_X) abs_x = ev.value;
                    else if (ev.code == ABS_Y) abs_y = ev.value;

                    int move_x = (abs_x > DEADZONE) ? 1 : (abs_x < -DEADZONE) ? -1 : 0;
                    int move_y = (abs_y > DEADZONE) ? 1 : (abs_y < -DEADZONE) ? -1 : 0;

                    if (move_x != last_move_x || move_y != last_move_y) {
                        if (move_x == 1) g_idle_add(click_button_idle, GINT_TO_POINTER(findKeyIndex("D")));
                        else if (move_x == -1) g_idle_add(click_button_idle, GINT_TO_POINTER(findKeyIndex("A")));

                        if (move_y == 1) g_idle_add(click_button_idle, GINT_TO_POINTER(findKeyIndex("S")));
                        else if (move_y == -1) g_idle_add(click_button_idle, GINT_TO_POINTER(findKeyIndex("W")));

                        last_move_x = move_x;
                        last_move_y = move_y;
                    }
                }
                else if (ev.type == EV_KEY && ev.value == 1) {
                    if (ev.code == BTN_MODE) {
                        mode = 0;
                        g_idle_add(toggle_keyboard_visibility, nullptr);
                        usleep(200000);
                    }
                    else if (ev.code == BTN_TL) { // Left click
                        send_key(uinput_fd, BTN_LEFT, 1);
                    }
                    else if (ev.code == BTN_TR) { // Right click
                        send_key(uinput_fd, BTN_RIGHT, 1);
                    }
                    else if (ev.code == BTN_TL2) { // Wheel up
                        send_mouse_wheel(uinput_fd, 1);
                    }
                    else if (ev.code == BTN_TR2) { // Wheel down
                        send_mouse_wheel(uinput_fd, -1);
                    }
                }
                else if (ev.type == EV_KEY && ev.value == 0) {
                    if (ev.code == BTN_TL) {
                        send_key(uinput_fd, BTN_LEFT, 0);
                    }
                    else if (ev.code == BTN_TR) {
                        send_key(uinput_fd, BTN_RIGHT, 0);
                    }
                }
            }
        }
        else if (rc == -EAGAIN) {
            usleep(5000);
        }
    }

    // Cleanup
    movement_thread_running = false;
    pthread_join(movement_thread, nullptr);
    libevdev_free(dev);
    close(fd);
}
// GTK activate
static void activate(GtkApplication* app, gpointer user_data) {
    window = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(window), "Joystick Controller");
    gtk_window_set_default_size(GTK_WINDOW(window), 640, 200);
    gtk_window_set_resizable(GTK_WINDOW(window), FALSE);

    grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 2);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 2);
    gtk_container_set_border_width(GTK_CONTAINER(grid), 5);
    gtk_widget_set_halign(grid, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(grid, GTK_ALIGN_CENTER);

    setupKeyboard();
    gtk_container_add(GTK_CONTAINER(window), grid);
    gtk_widget_show_all(window);

    focusButton(0);

    // Setup uinput for mouse control
    setup_uinput();
}

int main(int argc, char **argv) {
    std::thread joystick(joystick_thread);

    GtkApplication *app = gtk_application_new("com.example.joystickcontroller", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(activate), nullptr);
    int status = g_application_run(G_APPLICATION(app), argc, argv);

    running = false;
    joystick.join(); // Wait for joystick thread to finish

    if (uinput_fd >= 0) {
        ioctl(uinput_fd, UI_DEV_DESTROY);
        close(uinput_fd);
    }
    if (css_provider) {
        g_object_unref(css_provider);
    }
    pthread_mutex_destroy(&joystick_state.mutex);
    g_object_unref(app);
    return status;
}
